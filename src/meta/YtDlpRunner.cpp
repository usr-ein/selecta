// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/YtDlpRunner.h"

#include "match/Similarity.h"
#include "match/Tracklist.h"
#include "match/VersionClassifier.h"
#include "meta/RequestPacer.h"
#include "meta/ServicePolicy.h"
#include "meta/detail/Async.h"
#include "meta/detail/ServiceGate.h"
#include "meta/detail/TitleInfo.h"
#include "meta/detail/ToolProcess.h"
#include "meta/detail/YtDlpOutput.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <deque>
#include <tuple>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace selecta::meta {

namespace {

Q_LOGGING_CATEGORY(lcYtDlp, "selecta.meta.ytdlp")

namespace yt = detail::ytdlp;

constexpr int kLengthWindowSec = 3; // D23
constexpr double kArtistMatch = 0.9;

MetaError ytError(ErrorCode code, const QString &message)
{
    return makeError(Service::YtDlp, code, message);
}

MetaError cancelledError()
{
    return ytError(ErrorCode::Cancelled, u"Cancelled."_s);
}

MetaError disabledError()
{
    return ytError(ErrorCode::NotConfigured, u"yt-dlp previews are off in this session."_s);
}

// What identifies the file that was probed: a change (brew upgrade) means probing again.
struct FileStamp
{
    QString path;
    qint64 size = -1;
    QDateTime modified;

    bool operator==(const FileStamp &) const = default;
};

FileStamp stampOf(const QString &path)
{
    const QFileInfo info(path);
    return {path, info.size(), info.lastModified(QTimeZone::UTC)};
}

// A file Selecta may run (§12.1): a regular executable file once symlinks are resolved, not writable by group or
// others, owned by the user or root. Returns the canonical path.
Result<QString> checkExecutable(const QString &candidate, const QString &name)
{
    const QFileInfo target(QFileInfo(candidate).canonicalFilePath());
    if (target.filePath().isEmpty() || !target.isFile() || !target.isExecutable()) {
        return ytError(ErrorCode::ToolNotFound, u"%1 at %2 isn't an executable file."_s.arg(name, candidate));
    }
    if ((target.permissions() & (QFile::WriteGroup | QFile::WriteOther)) != 0) {
        return ytError(ErrorCode::ToolUnsafe, u"%1 at %2 can be changed by other users, so Selecta won't run it."_s.arg(
                                                  name, target.filePath()));
    }
    const uint owner = target.ownerId();
    if (owner != ::getuid() && owner != 0) {
        return ytError(ErrorCode::ToolUnsafe,
                       u"%1 at %2 belongs to another user, so Selecta won't run it."_s.arg(name, target.filePath()));
    }
    return target.filePath();
}

// yt-dlp and Deno, found and checked; the version is still to be asked.
struct Located
{
    QString path;
    QString denoPath;
    FileStamp stamp;
    std::optional<MetaError> error;
};

Located locate(const QString &executable, const QStringList &directories)
{
    Located located;
    if (!executable.isEmpty()) {
        if (QDir::isRelativePath(executable)) {
            located.error = ytError(ErrorCode::ToolNotFound, u"The yt-dlp path in Settings must be absolute."_s);
            return located;
        }
        const Result<QString> checked = checkExecutable(executable, u"yt-dlp"_s);
        if (!checked.ok()) {
            located.error = checked.error();
            return located;
        }
        located.path = checked.value();
    } else {
        for (const QString &directory : directories) {
            const QString candidate = QDir(directory).filePath(u"yt-dlp"_s);
            if (QDir::isRelativePath(directory) || !QFileInfo::exists(candidate)) {
                continue;
            }
            const Result<QString> checked = checkExecutable(candidate, u"yt-dlp"_s);
            if (!checked.ok()) {
                located.error = checked.error(); // the first one found decides: never fall back to another silently
                return located;
            }
            located.path = checked.value();
            break;
        }
        if (located.path.isEmpty()) {
            located.error =
                ytError(ErrorCode::ToolNotFound, u"yt-dlp not found. Install it with `brew install yt-dlp`, "
                                                 u"or set its path in Settings."_s);
            return located;
        }
    }
    located.stamp = stampOf(located.path);
    for (const QString &directory : directories) {
        const QString candidate = QDir(directory).filePath(u"deno"_s);
        if (QDir::isRelativePath(directory) || !QFileInfo::exists(candidate)) {
            continue;
        }
        const Result<QString> checked = checkExecutable(candidate, u"Deno"_s);
        if (checked.ok()) {
            located.denoPath = checked.value();
            break;
        }
        qCWarning(lcYtDlp).noquote() << checked.error().message;
    }
    return located;
}

// The child's whole environment (§12.5): nothing inherited from Selecta's.
QProcessEnvironment environmentFor(const QString &ytDlpPath, const QString &denoPath)
{
    QStringList path{QFileInfo(ytDlpPath).absolutePath()};
    if (!denoPath.isEmpty()) {
        path << QFileInfo(denoPath).absolutePath();
    }
    path << u"/opt/homebrew/bin"_s << u"/usr/local/bin"_s << u"/usr/bin"_s << u"/bin"_s << u"/usr/sbin"_s << u"/sbin"_s;
    path.removeDuplicates();
    QProcessEnvironment environment;
    environment.insert(u"HOME"_s, QDir::homePath());
    environment.insert(u"PATH"_s, path.join(u':'));
    environment.insert(u"LANG"_s, u"en_US.UTF-8"_s);
    environment.insert(u"TMPDIR"_s, QDir::tempPath());
    return environment;
}

MetaError outcomeError(const detail::ToolOutcome &outcome, bool runtimePassed)
{
    using End = detail::ToolOutcome::End;
    switch (outcome.end) {
    case End::FailedToStart:
        return ytError(ErrorCode::ToolNotFound, u"yt-dlp couldn't be started: %1"_s.arg(outcome.startError));
    case End::IdleTimeout:
        return ytError(ErrorCode::Timeout, u"The YouTube preview timed out: yt-dlp stopped making progress."_s);
    case End::TotalTimeout:
        return ytError(ErrorCode::Timeout, u"The YouTube preview timed out."_s);
    case End::OutputTooLarge:
        return ytError(ErrorCode::TooLarge, u"yt-dlp printed more than Selecta reads."_s);
    case End::Crashed:
        return ytError(ErrorCode::ToolFailed, u"yt-dlp crashed."_s);
    case End::Killed:
        return cancelledError();
    case End::Exited:
        break;
    }
    return yt::classifyFailure(outcome.errorLines, outcome.exitCode, runtimePassed);
}

bool samePath(const QString &a, const QString &b)
{
    const QString ca = QFileInfo(a).canonicalFilePath();
    return !ca.isEmpty() ? ca == QFileInfo(b).canonicalFilePath() : QDir::cleanPath(a) == QDir::cleanPath(b);
}

// --- pick() --------------------------------------------------------------------------------------------------------

QStringList artistKeysOf(const QStringList &names)
{
    QStringList keys;
    for (const QString &name : names) {
        const QStringList set = match::Normalizer::artistKeys(name).set;
        for (const QString &key : set) {
            if (!keys.contains(key)) {
                keys.append(key);
            }
        }
    }
    return keys;
}

// A channel's name without the decorations official channels add: "BicepVEVO", "Bicep Official" → "Bicep".
QString channelName(const QString &channel)
{
    static const QRegularExpression kDecorations(uR"((?:\s*\bofficial\b|\s*vevo)+\s*$|^\s*official\s+)"_s,
                                                 QRegularExpression::CaseInsensitiveOption);
    QString name = channel;
    name.remove(kDecorations);
    return name.trimmed().isEmpty() ? channel : name.trimmed();
}

bool namesArtist(const QString &name, const QStringList &artistKeys)
{
    const QStringList keys = match::Normalizer::artistKeys(name).set;
    return std::ranges::any_of(keys, [&](const QString &key) {
        return std::ranges::any_of(artistKeys, [&](const QString &artist) {
            return match::FieldMatcher::compareKeys(key, artist, {}).similarity >= kArtistMatch;
        });
    });
}

bool namesLabel(const QString &channel, const QString &label)
{
    if (label.isEmpty()) {
        return false;
    }
    const QString a = match::Normalizer::normalize(channel);
    const QString b = match::Normalizer::normalize(label);
    return !a.isEmpty() && !b.isEmpty() && match::FieldMatcher::compareKeys(a, b, {}).similarity >= kArtistMatch;
}

// "Kestrel Moon - Topic": YouTube's automatic channel for an artist's official audio, the audio YouTube Music plays.
constexpr QLatin1StringView kTopicSuffix{" - Topic"};

bool isTopicChannel(const QString &channel)
{
    return channel.endsWith(kTopicSuffix, Qt::CaseInsensitive) && channel.size() > kTopicSuffix.size();
}

bool isTopicChannelOf(const QString &channel, const QStringList &artistKeys)
{
    return isTopicChannel(channel) && namesArtist(channel.chopped(kTopicSuffix.size()), artistKeys);
}

// The title to compare: a video's "Artist - Title (…)" without the artist part when it names the release's artist,
// else the whole title (a Topic video's bare title, or "Title - Extended Mix").
QString videoTitle(const YouTubeCandidate &candidate, const QStringList &artistKeys)
{
    const match::TracklistLine line = match::TracklistParser::parseLine(candidate.title);
    if (line.kind == match::TracklistLineKind::Track && !line.artist.isEmpty() && !line.title.isEmpty() &&
        namesArtist(line.artist, artistKeys)) {
        return line.title;
    }
    return candidate.title;
}

int versionRank(match::VersionCompatibility version)
{
    switch (version) {
    case match::VersionCompatibility::Same:
        return 2;
    case match::VersionCompatibility::ProbablySame:
        return 1;
    case match::VersionCompatibility::Unsure:
    case match::VersionCompatibility::OtherVersion:
        break;
    }
    return 0;
}

struct PickCandidate
{
    const YouTubeCandidate *candidate = nullptr;
    int delta = 0;
    YouTubePick::Channel channel = YouTubePick::Channel::Other;
    bool topic = false; // the artist's Topic channel, first among the artist's channels
    int version = 0;
    QStringList reasons;

    auto key() const
    {
        return std::make_tuple(static_cast<int>(channel), topic, version, candidate->channelVerified, -delta,
                               candidate->viewCount.value_or(0));
    }
};

std::optional<PickCandidate> considered(const YouTubeCandidate &candidate, const Release &release,
                                        const detail::TitleInfo &releaseTitle, const QStringList &coreTokens,
                                        const QStringList &artistKeys, int releaseSec)
{
    if (!yt::isValidVideoId(candidate.id) || !candidate.durationSec.has_value()) {
        return std::nullopt;
    }
    if (candidate.liveStatus == u"is_live"_s || candidate.liveStatus == u"is_upcoming"_s ||
        candidate.liveStatus == u"post_live"_s) {
        return std::nullopt;
    }
    const int delta = std::abs(*candidate.durationSec - releaseSec);
    if (delta > kLengthWindowSec) {
        return std::nullopt;
    }
    const QStringList tokens = match::Normalizer::tokens(candidate.title);
    if (!std::ranges::all_of(coreTokens, [&](const QString &token) { return tokens.contains(token); })) {
        return std::nullopt; // the release's title words must all be there
    }
    const detail::TitleInfo title = detail::analyseTitle(videoTitle(candidate, artistKeys));
    const match::VersionCompatibility version = match::VersionClassifier::compare(releaseTitle.version, title.version);
    if (version == match::VersionCompatibility::OtherVersion || (title.djMix && !releaseTitle.djMix) ||
        !std::ranges::all_of(title.junk, [&](const QString &marker) { return releaseTitle.junk.contains(marker); })) {
        return std::nullopt;
    }
    PickCandidate pick;
    pick.candidate = &candidate;
    pick.delta = delta;
    const QString channel = channelName(candidate.channel);
    pick.topic = isTopicChannelOf(candidate.channel, artistKeys);
    if (pick.topic || namesArtist(channel, artistKeys) || namesLabel(channel, release.label)) {
        pick.channel = YouTubePick::Channel::Artist;
    } else if (isTopicChannel(candidate.channel)) {
        pick.channel = YouTubePick::Channel::Music;
    }
    pick.version = versionRank(version);
    pick.reasons.append(u"length Δ %1 s"_s.arg(delta));
    if (pick.topic) {
        pick.reasons.append(u"official Topic channel"_s);
    } else if (pick.channel == YouTubePick::Channel::Artist) {
        pick.reasons.append(u"the artist's or label's channel"_s);
    } else if (pick.channel == YouTubePick::Channel::Music) {
        pick.reasons.append(u"YouTube Music's official audio"_s);
    } else if (candidate.channelVerified) {
        pick.reasons.append(u"verified channel"_s);
    }
    if (candidate.viewCount.has_value()) {
        pick.reasons.append(u"%1 views"_s.arg(*candidate.viewCount));
    }
    return pick;
}

} // namespace

// --- YtDlpRunner::Private ------------------------------------------------------------------------------------------

struct YtDlpRunner::Private
{
    struct Run
    {
        enum class Kind : quint8 { Probe, Search, Fetch };
        enum class Stage : quint8 { Queued, Version, Gate, Permit, Running, Done };

        Kind kind = Kind::Probe;
        Stage stage = Stage::Queued;
        bool cancelled = false;
        detail::ServiceGate::Ticket ticket = 0;
        QPointer<detail::ToolProcess> process;
        base::Clock::TimePoint startedAt;
        std::optional<detail::SharedPromise<Result<YtDlpInstall>>> probePromise;
        std::optional<detail::SharedPromise<Result<QList<YouTubeCandidate>>>> searchPromise;
        QString text;
        int count = 0;
        QPointer<YtDlpFetch> job;
        QString videoId;
        QString directory;
        QString expectedPath; // <directory>/<id>.<ext> once yt-dlp named the format
        QString extension;
        QString codec;
        std::optional<QString> reportedFile;
        bool started = false;
        bool runtimePassed = false; // Deno was given to this run's yt-dlp
        YtDlpInstall install;       // what this run checked and runs
        QDateTime writeStart;       // files of this video changed since then are this run's
    };
    using RunPtr = std::shared_ptr<Run>;

    YtDlpRunner *q;
    Config config;
    RequestPacer &pacer;
    const base::Clock &clock;
    detail::ServiceGate gate;
    std::deque<RunPtr> queue;
    RunPtr current;
    std::optional<YtDlpInstall> install;
    FileStamp installStamp;
    std::optional<base::Clock::TimePoint> pausedUntil;
    bool pumpScheduled = false;

    Private(YtDlpRunner *owner, Config c, RequestPacer &p, const base::Clock &k)
        : q(owner)
        , config(std::move(c))
        , pacer(p)
        , clock(k)
        , gate(detail::ServiceGate::Limits{kYtDlpPolicy.minSpacing, kYtDlpPolicy.windowCount, kYtDlpPolicy.window,
                                           kYtDlpPolicy.maxInFlight},
               k)
    {}

    // --- scheduling ------------------------------------------------------------------------------------------------

    void submit(const RunPtr &run)
    {
        queue.push_back(run);
        schedulePump();
    }

    void schedulePump()
    {
        if (pumpScheduled) {
            return;
        }
        pumpScheduled = true;
        QMetaObject::invokeMethod(q, [this] { pump(); }, Qt::QueuedConnection);
    }

    // One run at a time, in order: its version check (if needed), its turn at the gate, its permit, its process.
    void pump()
    {
        pumpScheduled = false;
        if (current) {
            return;
        }
        while (!queue.empty()) {
            const RunPtr run = queue.front();
            queue.pop_front();
            if (run->cancelled) {
                continue;
            }
            current = run;
            begin(run);
            return;
        }
    }

    void begin(const RunPtr &run)
    {
        run->startedAt = clock.now();
        if (run->kind == Run::Kind::Probe || !installStillValid()) {
            startVersion(run);
            return;
        }
        afterInstall(run);
    }

    bool installStillValid() const
    {
        if (!install.has_value()) {
            return false;
        }
        const Located now = locate(config.executable, config.searchDirectories);
        return !now.error.has_value() && now.path == install->path && now.stamp == installStamp &&
               now.denoPath == install->denoPath;
    }

    std::optional<MetaError> pauseError() const
    {
        if (!pausedUntil.has_value() || *pausedUntil <= clock.now()) {
            return std::nullopt;
        }
        const auto remaining = std::chrono::ceil<std::chrono::seconds>(*pausedUntil - clock.now());
        MetaError e = ytError(ErrorCode::RateLimited,
                              u"YouTube previews are paused for %1 more minutes after a rate limit or bot check."_s.arg(
                                  std::chrono::ceil<std::chrono::minutes>(remaining).count()));
        e.retryAfter = remaining;
        return e;
    }

    // --- the version check -----------------------------------------------------------------------------------------

    void startVersion(const RunPtr &run)
    {
        const Located located = locate(config.executable, config.searchDirectories);
        if (located.error.has_value()) {
            install.reset();
            fail(run, *located.error);
            return;
        }
        run->stage = Run::Stage::Version;
        detail::ToolProcess::Spec spec;
        spec.program = located.path;
        spec.arguments = yt::versionArguments(located.denoPath, config.ytDlpCacheDir);
        spec.workingDirectory = QDir::tempPath();
        spec.environment = environmentFor(located.path, located.denoPath);
        spec.totalTimeout = config.versionTimeout;
        spec.maxStdoutBytes = qint64{64} * 1024;
        startProcess(run, std::move(spec), [this, run, located] { onVersionDone(run, located); });
    }

    void onVersionDone(const RunPtr &run, const Located &located)
    {
        const detail::ToolOutcome outcome = retireProcess(run);
        if (run->cancelled) {
            end(run);
            return;
        }
        const Result<YtDlpInstall> result = readVersion(located, outcome);
        if (!result.ok()) {
            install.reset();
            fail(run, result.error());
            return;
        }
        install = result.value();
        installStamp = located.stamp;
        qCInfo(lcYtDlp).noquote() << "yt-dlp" << install->version << "at" << install->path
                                  << (install->denoPath.isEmpty() ? u"without Deno"_s : u"with Deno"_s);
        afterInstall(run);
    }

    Result<YtDlpInstall> readVersion(const Located &located, const detail::ToolOutcome &outcome) const
    {
        using End = detail::ToolOutcome::End;
        if (outcome.end == End::TotalTimeout) {
            return ytError(ErrorCode::Timeout, u"yt-dlp didn't report its version in time."_s);
        }
        if (outcome.end != End::Exited) {
            return outcomeError(outcome, !located.denoPath.isEmpty());
        }
        if (outcome.exitCode != 0) {
            MetaError e = yt::classifyFailure(outcome.errorLines, outcome.exitCode, !located.denoPath.isEmpty());
            if (e.code != ErrorCode::ToolTooOld) {
                e = ytError(ErrorCode::ToolFailed,
                            u"yt-dlp couldn't report its version (exit %1)."_s.arg(outcome.exitCode));
            }
            return e;
        }
        const std::optional<QDate> date = yt::parseVersion(outcome.standardOutput);
        if (!date.has_value()) {
            return ytError(ErrorCode::ToolFailed, u"yt-dlp reported a version Selecta can't read."_s);
        }
        YtDlpInstall found;
        found.path = located.path;
        found.version = yt::versionText(outcome.standardOutput);
        found.versionDate = *date;
        found.denoPath = located.denoPath;
        found.stale = date->daysTo(config.today()) > kStaleDays;
        if (*date < minimumVersion()) {
            return ytError(
                ErrorCode::ToolTooOld,
                u"Your yt-dlp is from %1; Selecta needs %2 or newer. Update it with "
                u"`brew upgrade yt-dlp`."_s.arg(date->toString(Qt::ISODate), minimumVersion().toString(Qt::ISODate)));
        }
        return found;
    }

    // --- gate, permit, process -------------------------------------------------------------------------------------

    void afterInstall(const RunPtr &run)
    {
        if (!install.has_value()) { // can't happen: begin() and onVersionDone() set it
            fail(run, ytError(ErrorCode::Internal, u"yt-dlp wasn't probed."_s));
            return;
        }
        run->install = *install; // settings may change while the run waits; it keeps what it checked
        if (run->kind == Run::Kind::Probe) {
            if (run->probePromise.has_value()) {
                run->probePromise->finish(run->install);
            }
            end(run);
            return;
        }
        if (std::optional<MetaError> paused = pauseError()) {
            fail(run, *paused);
            return;
        }
        const auto wait = std::chrono::ceil<std::chrono::milliseconds>(gate.nextStartIn());
        if (wait > kYtDlpPolicy.maxWait) {
            MetaError e = ytError(ErrorCode::RateLimited, u"Selecta already ran yt-dlp often in the last minutes; try "
                                                          u"again shortly."_s);
            e.retryAfter = std::chrono::ceil<std::chrono::seconds>(wait);
            fail(run, e);
            return;
        }
        enqueueAtGate(run);
    }

    void enqueueAtGate(const RunPtr &run)
    {
        run->stage = Run::Stage::Gate;
        run->ticket = gate.enqueue([this, weak = std::weak_ptr<Run>(run)](detail::ServiceGate::Ticket ticket) {
            const RunPtr r = weak.lock();
            if (!r || r->cancelled || r != current || r->ticket != ticket) {
                gate.abandon(ticket);
                return;
            }
            askPermit(r);
        });
    }

    void askPermit(const RunPtr &run)
    {
        run->stage = Run::Stage::Permit;
        const QPointer<YtDlpRunner> guard(q);
        const detail::ServiceGate::Ticket ticket = run->ticket;
        pacer.acquire(governor::Operation::YtDlpRun, [guard, run, ticket](PermitDecision decision) {
            if (!guard.isNull()) {
                guard->d->onPermit(run, ticket, decision);
            }
        });
    }

    void onPermit(const RunPtr &run, detail::ServiceGate::Ticket ticket, PermitDecision &decision)
    {
        std::optional<governor::Permit> permit = std::move(decision.permit);
        if (run->cancelled || run->stage != Run::Stage::Permit || run->ticket != ticket) {
            return; // the gate was released when it was cancelled; the unused permit is dropped
        }
        if (!permit.has_value()) {
            gate.abandon(ticket);
            if (decision.retryAfter > std::chrono::milliseconds::zero() &&
                decision.retryAfter <= kYtDlpPolicy.maxWait) {
                gate.blockUntil(clock.now() + decision.retryAfter);
                enqueueAtGate(run);
                return;
            }
            MetaError e = ytError(ErrorCode::Refused,
                                  decision.refusal.isEmpty() ? u"The network governor refused."_s : decision.refusal);
            if (decision.retryAfter > std::chrono::milliseconds::zero()) {
                e.retryAfter = std::chrono::ceil<std::chrono::seconds>(decision.retryAfter);
            }
            report(run, 0, u"refused: %1"_s.arg(e.message));
            fail(run, e);
            return;
        }
        if (!permit->consume(governor::Operation::YtDlpRun)) {
            gate.abandon(ticket);
            fail(run, ytError(ErrorCode::Internal, u"The governor issued a permit for another operation."_s));
            return;
        }
        gate.markStarted(ticket);
        startMain(run);
    }

    void startMain(const RunPtr &run)
    {
        run->stage = Run::Stage::Running;
        run->startedAt = clock.now();
        const YtDlpInstall &tool = run->install;
        run->runtimePassed = !tool.denoPath.isEmpty();
        run->writeStart = QDateTime::currentDateTimeUtc().addSecs(-1); // file times can be coarse
        detail::ToolProcess::Spec spec;
        spec.program = tool.path;
        spec.environment = environmentFor(tool.path, tool.denoPath);
        spec.maxStdoutBytes = kYtDlpPolicy.maxBodyBytes;
        if (run->kind == Run::Kind::Search) {
            spec.arguments = yt::searchArguments(tool.denoPath, config.ytDlpCacheDir, run->text, run->count);
            spec.workingDirectory = QDir::tempPath();
            spec.totalTimeout = config.searchTimeout;
        } else {
            spec.arguments = yt::fetchArguments(tool.denoPath, config.ytDlpCacheDir, run->videoId, run->directory);
            spec.workingDirectory = run->directory;
            spec.idleTimeout = config.idleTimeout;
            spec.totalTimeout = config.totalTimeout;
            spec.streamLines = true;
        }
        startProcess(run, std::move(spec), [this, run] { onMainDone(run); });
    }

    void startProcess(const RunPtr &run, detail::ToolProcess::Spec spec, const std::function<void()> &done)
    {
        auto *process = new detail::ToolProcess(std::move(spec), q);
        run->process = process;
        if (run->kind == Run::Kind::Fetch) {
            QObject::connect(process, &detail::ToolProcess::lineReady, q,
                             [this, run](QProcess::ProcessChannel, const QByteArray &line) { onFetchLine(run, line); });
        }
        QObject::connect(process, &detail::ToolProcess::finished, q, done);
        process->start();
    }

    void onMainDone(const RunPtr &run)
    {
        gate.markFinished(run->ticket);
        const detail::ToolOutcome outcome = retireProcess(run);
        if (run->cancelled) {
            removePartial(run);
            report(run, 0, u"cancelled"_s);
            end(run);
            return;
        }
        if (outcome.end == detail::ToolOutcome::End::FailedToStart) {
            install.reset(); // probe again next time
        }
        const bool succeeded = outcome.end == detail::ToolOutcome::End::Exited && outcome.exitCode == 0;
        if (!succeeded) {
            const MetaError e = outcomeError(outcome, run->runtimePassed);
            report(run, outcome.standardOutput.size(), e.message);
            removePartial(run);
            if (yt::startsPause(e.code)) {
                startPause(e);
            }
            fail(run, e);
            return;
        }
        if (run->kind == Run::Kind::Search) {
            finishSearch(run, outcome);
        } else {
            finishFetch(run);
        }
    }

    void finishSearch(const RunPtr &run, const detail::ToolOutcome &outcome)
    {
        const Result<QList<YouTubeCandidate>> candidates = yt::parseSearch(outcome.standardOutput);
        report(run, outcome.standardOutput.size(),
               candidates.ok() ? u"ok, %1 results"_s.arg(candidates.value().size()) : candidates.error().message);
        if (!candidates.ok()) {
            fail(run, candidates.error());
            return;
        }
        if (run->searchPromise.has_value()) {
            run->searchPromise->finish(candidates.value());
        }
        end(run);
    }

    void finishFetch(const RunPtr &run)
    {
        std::optional<MetaError> problem;
        if (run->expectedPath.isEmpty()) {
            problem = ytError(ErrorCode::ToolFailed, u"yt-dlp didn't say which audio format it fetched."_s);
        } else if (run->reportedFile.has_value() && !samePath(*run->reportedFile, run->expectedPath)) {
            problem = ytError(ErrorCode::ToolFailed, u"yt-dlp wrote the audio somewhere unexpected."_s);
        } else if (const QFileInfo file(run->expectedPath); !file.isFile() || file.size() <= 0) {
            problem = ytError(ErrorCode::ToolFailed, u"yt-dlp finished without writing the audio."_s);
        }
        if (problem.has_value()) {
            report(run, 0, problem->message);
            removePartial(run);
            fail(run, *problem);
            return;
        }
        markStarted(run);
        FetchedAudio audio;
        audio.videoId = run->videoId;
        audio.path = run->expectedPath;
        audio.format = run->extension;
        audio.codec = run->codec;
        audio.bytes = QFileInfo(run->expectedPath).size();
        report(run, audio.bytes, u"ok"_s);
        if (run->job) {
            run->job->finish(audio);
        }
        end(run);
    }

    void onFetchLine(const RunPtr &run, const QByteArray &line)
    {
        if (run->cancelled || run->stage != Run::Stage::Running) {
            return;
        }
        if (const std::optional<yt::Format> format = yt::parseFormat(line)) {
            run->extension = format->extension;
            run->codec = format->codec;
            run->expectedPath = QDir(run->directory).filePath(QString(run->videoId + u'.' + format->extension));
            markStarted(run); // stdout and stderr are separate pipes: progress lines may have been read first
            return;
        }
        if (const std::optional<yt::Progress> progress = yt::parseProgress(line)) {
            markStarted(run);
            if (run->started && run->job) {
                run->job->notifyProgress(progress->downloadedBytes, progress->totalBytes);
            }
            return;
        }
        if (const std::optional<QString> file = yt::parseFile(line)) {
            run->reportedFile = *file;
            markStarted(run);
        }
    }

    // started() once the named file exists, so a reader can open it.
    static void markStarted(const RunPtr &run)
    {
        if (run->started || run->expectedPath.isEmpty() || !QFileInfo::exists(run->expectedPath)) {
            return;
        }
        run->started = true;
        if (run->job) {
            run->job->notifyStarted(run->expectedPath);
        }
    }

    // Deletes what a failed or cancelled fetch wrote: the named file, and any <id>.* file in the folder that changed
    // during the run (yt-dlp may not have named its format yet when it was stopped). Older files are left alone.
    static void removePartial(const RunPtr &run)
    {
        if (run->kind != Run::Kind::Fetch) {
            return;
        }
        if (!run->expectedPath.isEmpty()) {
            QFile::remove(run->expectedPath);
        }
        if (!run->writeStart.isValid()) {
            return; // the process never started
        }
        const QFileInfoList files =
            QDir(run->directory).entryInfoList({QString(run->videoId + u".*"_s)}, QDir::Files | QDir::Hidden);
        for (const QFileInfo &file : files) {
            if (file.lastModified(QTimeZone::UTC) >= run->writeStart) {
                QFile::remove(file.absoluteFilePath());
            }
        }
    }

    // --- endings ---------------------------------------------------------------------------------------------------

    void fail(const RunPtr &run, const MetaError &error)
    {
        if (run->probePromise.has_value()) {
            run->probePromise->finish(error);
        }
        if (run->searchPromise.has_value()) {
            run->searchPromise->finish(error);
        }
        if (run->job) {
            run->job->finish(error);
        }
        end(run);
    }

    // The finished process's outcome; the process object goes (later: we may be inside its signal).
    detail::ToolOutcome retireProcess(const RunPtr &run) const
    {
        detail::ToolOutcome outcome;
        if (run->process) {
            outcome = run->process->outcome();
            run->process->disconnect(q);
            run->process->deleteLater();
            run->process = nullptr;
        }
        return outcome;
    }

    void end(const RunPtr &run)
    {
        run->stage = Run::Stage::Done;
        retireProcess(run);
        if (current == run) {
            current.reset();
        }
        schedulePump();
    }

    void cancel(const RunPtr &run)
    {
        if (run->cancelled || run->stage == Run::Stage::Done) {
            return;
        }
        run->cancelled = true;
        switch (run->stage) {
        case Run::Stage::Queued:
            std::erase(queue, run);
            run->stage = Run::Stage::Done;
            return;
        case Run::Stage::Gate:
        case Run::Stage::Permit:
            gate.abandon(run->ticket);
            end(run);
            return;
        case Run::Stage::Version:
        case Run::Stage::Running:
            removePartial(run);
            if (run->process) {
                run->process->kill(); // end() follows when it has exited: one process at a time
            }
            return;
        case Run::Stage::Done:
            return;
        }
    }

    RunPtr runOf(const YtDlpFetch *job) const
    {
        if (current && current->job == job) {
            return current;
        }
        const auto it = std::ranges::find_if(queue, [job](const RunPtr &run) { return run->job == job; });
        return it != queue.end() ? *it : RunPtr();
    }

    void cancelFetch(YtDlpFetch *job, bool notify)
    {
        if (const RunPtr run = runOf(job)) {
            cancel(run);
        }
        if (notify) {
            job->finish(cancelledError());
        }
    }

    void startPause(const MetaError &reason)
    {
        pausedUntil = clock.now() + kYtDlpPauseAfterRateLimit;
        qCWarning(lcYtDlp).noquote() << "YouTube previews paused for an hour:" << reason.message;
        QMetaObject::invokeMethod(q, [owner = q, reason] { Q_EMIT owner->paused(reason); }, Qt::QueuedConnection);
        const std::optional<MetaError> paused = pauseError();
        const std::deque<RunPtr> waiting = std::exchange(queue, {});
        for (const RunPtr &run : waiting) {
            if (run->kind == Run::Kind::Probe) {
                queue.push_back(run); // local: not affected
            } else if (!run->cancelled) {
                fail(run, paused.value_or(reason));
            }
        }
    }

    void report(const RunPtr &run, qint64 bytes, const QString &outcome) const
    {
        RequestRecord record;
        record.service = Service::YtDlp;
        record.method = QByteArrayLiteral("RUN");
        record.target = run->kind == Run::Kind::Search
                            ? u"search %1 results for \"%2\""_s.arg(run->count).arg(run->text)
                            : u"fetch audio of %1"_s.arg(run->videoId);
        record.bytes = bytes;
        record.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - run->startedAt);
        record.outcome = outcome;
        qCDebug(lcYtDlp).noquote() << "yt-dlp" << record.target << outcome;
        pacer.report(record);
    }

    void shutdown()
    {
        std::deque<RunPtr> runs = std::exchange(queue, {});
        if (current) {
            runs.push_front(std::exchange(current, nullptr));
        }
        for (const RunPtr &run : runs) {
            run->cancelled = true;
            run->stage = Run::Stage::Done;
            delete run->process.data(); // kills the process group and reaps it
            removePartial(run);
            if (run->probePromise.has_value()) {
                run->probePromise->cancel();
            }
            if (run->searchPromise.has_value()) {
                run->searchPromise->cancel();
            }
            if (run->job) {
                run->job->m_runner = nullptr;
                run->job->finish(cancelledError());
            }
        }
    }
};

// --- YtDlpRunner ---------------------------------------------------------------------------------------------------

QUrl YouTubeCandidate::watchUrl() const
{
    return QUrl(u"https://www.youtube.com/watch?v="_s + id);
}

YtDlpRunner::Config YtDlpRunner::Config::disabled()
{
    Config config;
    config.enabled = false;
    config.executable.clear();
    config.searchDirectories.clear();
    return config;
}

QDate YtDlpRunner::minimumVersion()
{
    // The release that added --js-runtimes and --no-remote-components (and YouTube's JavaScript-runtime support).
    return {2025, 11, 12};
}

QStringList YtDlpRunner::defaultSearchDirectories()
{
    QStringList directories{u"/opt/homebrew/bin"_s, u"/usr/local/bin"_s,
                            QDir(QDir::homePath()).filePath(u".local/bin"_s)};
    const QStringList path = qEnvironmentVariable("PATH").split(u':', Qt::SkipEmptyParts);
    for (const QString &entry : path) {
        if (!QDir::isRelativePath(entry) && !directories.contains(entry)) {
            directories.append(entry); // never "." or another relative entry: no working-directory lookups
        }
    }
    return directories;
}

YtDlpRunner::YtDlpRunner(Config config, RequestPacer &pacer, const base::Clock &clock, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, std::move(config), pacer, clock))
{}

YtDlpRunner::~YtDlpRunner()
{
    try {
        d->shutdown();
    } catch (const std::exception &e) {
        qCCritical(lcYtDlp) << "Stopping yt-dlp failed:" << e.what();
    } catch (...) {
        qCCritical(lcYtDlp) << "Stopping yt-dlp failed.";
    }
}

QFuture<Result<YtDlpInstall>> YtDlpRunner::probe()
{
    if (!d->config.enabled) {
        return detail::deferred<Result<YtDlpInstall>>(this, disabledError());
    }
    const auto run = std::make_shared<Private::Run>();
    run->kind = Private::Run::Kind::Probe;
    run->probePromise.emplace();
    run->probePromise->onCanceled(this, [this, weak = std::weak_ptr<Private::Run>(run)] {
        if (const auto r = weak.lock()) {
            d->cancel(r);
        }
    });
    d->submit(run);
    return run->probePromise->future();
}

QFuture<Result<QList<YouTubeCandidate>>> YtDlpRunner::search(const QString &text, int count)
{
    if (!d->config.enabled) {
        return detail::deferred<Result<QList<YouTubeCandidate>>>(this, disabledError());
    }
    const QString clean = detail::ytdlp::cleanSearchText(text);
    if (clean.isEmpty()) {
        return detail::deferred<Result<QList<YouTubeCandidate>>>(
            this, ytError(ErrorCode::InvalidArgument, u"Nothing to search for."_s));
    }
    if (std::optional<MetaError> paused = d->pauseError()) {
        return detail::deferred<Result<QList<YouTubeCandidate>>>(this, *paused);
    }
    const auto run = std::make_shared<Private::Run>();
    run->kind = Private::Run::Kind::Search;
    run->text = clean;
    run->count = std::clamp(count, 1, kMaxSearchResults);
    run->searchPromise.emplace();
    run->searchPromise->onCanceled(this, [this, weak = std::weak_ptr<Private::Run>(run)] {
        if (const auto r = weak.lock()) {
            d->cancel(r);
        }
    });
    d->submit(run);
    return run->searchPromise->future();
}

YtDlpFetch *YtDlpRunner::fetch(const QString &videoId, const QString &previewCacheDir, QObject *parent)
{
    auto *job = new YtDlpFetch(this, videoId, previewCacheDir, parent);
    if (!d->config.enabled) {
        job->m_runner = nullptr;
        job->finish(disabledError());
        return job;
    }
    const QFileInfo directory(previewCacheDir);
    if (!detail::ytdlp::isValidVideoId(videoId)) {
        job->m_runner = nullptr;
        job->finish(ytError(ErrorCode::InvalidArgument, u"Not a YouTube video ID."_s));
        return job;
    }
    if (previewCacheDir.isEmpty() || QDir::isRelativePath(previewCacheDir) || !directory.isDir() ||
        !directory.isWritable()) {
        job->m_runner = nullptr;
        job->finish(
            ytError(ErrorCode::InvalidArgument, u"The preview cache folder doesn't exist or isn't writable."_s));
        return job;
    }
    if (std::optional<MetaError> paused = d->pauseError()) {
        job->m_runner = nullptr;
        job->finish(*paused);
        return job;
    }
    const auto run = std::make_shared<Private::Run>();
    run->kind = Private::Run::Kind::Fetch;
    run->job = job;
    run->videoId = videoId;
    run->directory = directory.absoluteFilePath();
    d->submit(run);
    return job;
}

std::optional<base::Clock::TimePoint> YtDlpRunner::pausedUntil() const
{
    if (!d->pausedUntil.has_value() || *d->pausedUntil <= d->clock.now()) {
        return std::nullopt;
    }
    return d->pausedUntil;
}

void YtDlpRunner::setExecutable(const QString &path)
{
    d->config.executable = path;
    d->install.reset();
}

namespace {

// The base title and the version words of a release's search.
QString titleWords(const Release &release)
{
    const detail::TitleInfo title = detail::analyseTitle(release.title);
    QString text = title.coreDisplay.isEmpty() ? release.title : title.coreDisplay;
    const QString version = detail::versionWords(title.version);
    if (!version.isEmpty()) {
        text += u' ' + version;
    }
    return text;
}

} // namespace

QString YtDlpRunner::searchText(const Release &release)
{
    const QString artist = release.artists.isEmpty() ? QString() : detail::primaryArtist(release.artists.first());
    return detail::ytdlp::cleanSearchText(artist + u' ' + titleWords(release));
}

QString YtDlpRunner::topicSearchText(const Release &release)
{
    const QString artist = release.artists.isEmpty() ? QString() : detail::primaryArtist(release.artists.first());
    if (artist.isEmpty()) {
        return {};
    }
    return detail::ytdlp::cleanSearchText(artist + kTopicSuffix + u' ' + titleWords(release));
}

std::optional<YouTubePick> YtDlpRunner::pick(const QList<YouTubeCandidate> &candidates, const Release &release)
{
    if (!release.durationMs.has_value() || *release.durationMs <= 0) {
        return std::nullopt; // ±3 s can't be checked
    }
    const int releaseSec = static_cast<int>(std::lround(static_cast<double>(*release.durationMs) / 1000.0));
    const detail::TitleInfo releaseTitle = detail::analyseTitle(release.title);
    const QStringList coreTokens = match::Normalizer::tokens(releaseTitle.core.key);
    const QStringList artistKeys = artistKeysOf(release.artists);
    std::optional<PickCandidate> best;
    for (const YouTubeCandidate &candidate : candidates) {
        std::optional<PickCandidate> scored =
            considered(candidate, release, releaseTitle, coreTokens, artistKeys, releaseSec);
        if (scored.has_value() && (!best.has_value() || scored->key() > best->key())) {
            best = std::move(scored);
        }
    }
    if (!best.has_value()) {
        return std::nullopt;
    }
    return YouTubePick{*best->candidate, best->reasons, best->channel};
}

// --- YtDlpFetch ----------------------------------------------------------------------------------------------------

YtDlpFetch::YtDlpFetch(YtDlpRunner *runner, QString videoId, QString directory, QObject *parent)
    : QObject(parent)
    , m_runner(runner)
    , m_videoId(std::move(videoId))
    , m_directory(std::move(directory))
{}

YtDlpFetch::~YtDlpFetch()
{
    if (!m_finished && !m_runner.isNull()) {
        m_runner->d->cancelFetch(this, false);
    }
}

void YtDlpFetch::cancel()
{
    if (m_finished) {
        return;
    }
    if (!m_runner.isNull()) {
        m_runner->d->cancelFetch(this, true);
        return;
    }
    finish(cancelledError());
}

void YtDlpFetch::notifyStarted(const QString &path)
{
    m_filePath = path;
    QMetaObject::invokeMethod(this, [this, path] { Q_EMIT started(path); }, Qt::QueuedConnection);
}

void YtDlpFetch::notifyProgress(qint64 downloadedBytes, qint64 totalBytes)
{
    QMetaObject::invokeMethod(
        this, [this, downloadedBytes, totalBytes] { Q_EMIT progress(downloadedBytes, totalBytes); },
        Qt::QueuedConnection);
}

void YtDlpFetch::finish(Result<FetchedAudio> result)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    QMetaObject::invokeMethod(
        this, [this, result = std::move(result)] { Q_EMIT finished(result); }, Qt::QueuedConnection);
}

} // namespace selecta::meta
