// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli meta-ytdlp-search "artist - title": the YouTube preview path of the player — probe the user's own
// yt-dlp, find the release (or take --length), search YouTube without downloading, and show the ±3 s pick. With
// --fetch <folder>, also fetch the pick's audio there (one short download; delete the folder afterwards).
// A manual smoke tool: run it by hand, sparingly; never from tests or CI. The audio is never filed, shared or tagged.
#include "Command.h"
#include "MetaSession.h"
#include "match/Tracklist.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

#include <cstdio>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::cli::MetaSession;
using selecta::cli::printLine;
using selecta::meta::Release;
using selecta::meta::YouTubeCandidate;

namespace {

constexpr int kDefaultCount = 10;

QString describeVideo(int rank, const YouTubeCandidate &video)
{
    return u"%1. %2  %3  %4  \"%5\"%6"_s.arg(rank, 2).arg(
        video.id,
        selecta::cli::formatLength(
            video.durationSec.has_value() ? std::optional<qint64>(qint64{*video.durationSec} * 1000) : std::nullopt),
        video.channel.isEmpty() ? u"?"_s : video.channel, video.title,
        video.viewCount.has_value() ? u", %1 views"_s.arg(*video.viewCount) : QString());
}

// The release to match: the line itself when its length is known, else what the resolver finds.
std::optional<Release> releaseFor(const selecta::match::TracklistLine &line, std::optional<qint64> length,
                                  MetaSession &session)
{
    if (length.has_value()) {
        Release release;
        release.title = line.title;
        release.artists = QStringList{line.artist};
        release.durationMs = length;
        return release;
    }
    selecta::meta::ResolveQuery query;
    query.artist = line.artist;
    query.title = line.title;
    const auto result = MetaSession::wait(session.resolver().resolve(query, selecta::meta::Priority::Interactive));
    if (!result.has_value() || !result->ok()) {
        printLine(stderr, result.has_value() ? result->error().toString() : u"No answer in time."_s);
        return std::nullopt;
    }
    const selecta::meta::Resolution &resolution = result->value();
    if (resolution.candidates.isEmpty() || resolution.confidence < selecta::meta::Confidence::Likely ||
        !resolution.candidates.first().release.durationMs.has_value()) {
        printLine(stderr, u"No confident release with a length for that line; pass --length m:ss."_s);
        return std::nullopt;
    }
    return resolution.candidates.first().release;
}

int fetch(selecta::meta::YtDlpRunner &runner, const QString &videoId, const QString &folder)
{
    printLine(stdout, u"fetching %1 into %2"_s.arg(videoId, folder));
    QEventLoop loop;
    const std::unique_ptr<selecta::meta::YtDlpFetch> job(runner.fetch(videoId, folder));
    int exitCode = selecta::cli::kExitFailed;
    int lastPercent = -1;
    QObject::connect(job.get(), &selecta::meta::YtDlpFetch::started, &loop,
                     [](const QString &path) { printLine(stdout, u"  writing %1"_s.arg(path)); });
    QObject::connect(job.get(), &selecta::meta::YtDlpFetch::progress, &loop, [&lastPercent](qint64 done, qint64 total) {
        const int percent = total > 0 ? static_cast<int>((done * 100) / total) : -1;
        if (percent >= 0 && percent / 25 != lastPercent / 25) {
            printLine(stdout, u"  %1% of %2 bytes"_s.arg(percent).arg(total));
            lastPercent = percent;
        }
    });
    QObject::connect(job.get(), &selecta::meta::YtDlpFetch::finished, &loop,
                     [&loop, &exitCode](const selecta::meta::Result<selecta::meta::FetchedAudio> &result) {
                         if (result.ok()) {
                             const selecta::meta::FetchedAudio &audio = result.value();
                             printLine(stdout,
                                       u"  done: %1 (%2, %3, %4 bytes)"_s
                                           .arg(audio.path, audio.format, audio.codec.isEmpty() ? u"?"_s : audio.codec)
                                           .arg(audio.bytes));
                             exitCode = selecta::cli::kExitOk;
                         } else {
                             printLine(stderr, u"  "_s + result.error().toString());
                         }
                         loop.quit();
                     });
    QTimer::singleShot(std::chrono::minutes(3), &loop, &QEventLoop::quit);
    if (!job->isFinished() || exitCode != selecta::cli::kExitOk) {
        loop.exec();
    }
    return exitCode;
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    MetaSession::addCommonOptions(parser);
    parser.addOption({u"length"_s, u"The release's length (skips the metadata lookup)."_s, u"m:ss"_s});
    parser.addOption({u"count"_s, u"How many results to ask YouTube for (1–20)."_s, u"n"_s});
    parser.addOption({u"ytdlp"_s, u"The yt-dlp to run (default: found as the app finds it)."_s, u"path"_s});
    parser.addOption({u"fetch"_s, u"Fetch the pick's audio into this existing folder."_s, u"folder"_s});
    if (!parser.parse(QStringList{u"meta-ytdlp-search"_s} + arguments) || parser.positionalArguments().size() != 1) {
        printLine(stderr, u"usage: selecta-cli meta-ytdlp-search [--length m:ss] [--count n] [--ytdlp path] "
                          u"[--fetch folder] [--verbose] [--no-spotify] \"artist - title\""_s);
        return selecta::cli::kExitUsage;
    }
    const selecta::match::TracklistLine line =
        selecta::match::TracklistParser::parseLine(parser.positionalArguments().constFirst());
    if (line.kind != selecta::match::TracklistLineKind::Track || line.artist.isEmpty()) {
        printLine(stderr, u"Give an \"artist - title\" line."_s);
        return selecta::cli::kExitUsage;
    }
    std::optional<qint64> length =
        line.durationSec > 0 ? std::optional<qint64>(qint64{line.durationSec} * 1000) : std::nullopt;
    if (parser.isSet(u"length"_s)) {
        length = selecta::cli::parseLength(parser.value(u"length"_s));
        if (!length.has_value()) {
            printLine(stderr, u"--length takes m:ss or seconds."_s);
            return selecta::cli::kExitUsage;
        }
    }
    bool countOk = true;
    const int count = parser.isSet(u"count"_s) ? parser.value(u"count"_s).toInt(&countOk) : kDefaultCount;
    if (!countOk || count < 1 || count > selecta::meta::YtDlpRunner::kMaxSearchResults) {
        printLine(stderr, u"--count takes 1 to 20."_s);
        return selecta::cli::kExitUsage;
    }
    const QString folder = parser.value(u"fetch"_s);
    if (parser.isSet(u"fetch"_s) && !QFileInfo(folder).isDir()) {
        printLine(stderr, u"--fetch needs an existing folder."_s);
        return selecta::cli::kExitUsage;
    }

    MetaSession session(MetaSession::commonOptions(parser));
    selecta::meta::YtDlpRunner &runner = session.ytDlp(parser.value(u"ytdlp"_s));
    const auto install = MetaSession::wait(runner.probe());
    if (!install.has_value() || !install->ok()) {
        printLine(stderr, install.has_value() ? install->error().toString() : u"yt-dlp didn't answer."_s);
        return selecta::cli::kExitFailed;
    }
    printLine(stdout,
              u"yt-dlp %1 at %2, %3%4"_s.arg(install->value().version, install->value().path,
                                             install->value().denoPath.isEmpty() ? u"no Deno"_s : u"with Deno"_s,
                                             install->value().stale ? u" (old: brew upgrade yt-dlp)"_s : QString()));
    if (!length.has_value()) {
        session.start();
    }
    const std::optional<Release> release = releaseFor(line, length, session);
    if (!release.has_value()) {
        return selecta::cli::kExitFailed;
    }
    printLine(stdout, u"release: %1 — %2 (%3)"_s.arg(release->artists.join(u", "_s), release->title,
                                                     selecta::cli::formatLength(release->durationMs)));
    const QString text = selecta::meta::YtDlpRunner::searchText(*release);
    printLine(stdout, u"search: \"%1\""_s.arg(text));
    const auto found = MetaSession::wait(runner.search(text, count));
    if (!found.has_value() || !found->ok()) {
        printLine(stderr, found.has_value() ? found->error().toString() : u"yt-dlp didn't answer."_s);
        return selecta::cli::kExitFailed;
    }
    for (qsizetype i = 0; i < found->value().size(); ++i) {
        printLine(stdout, describeVideo(static_cast<int>(i + 1), found->value().at(i)));
    }
    const std::optional<selecta::meta::YouTubePick> pick = selecta::meta::YtDlpRunner::pick(found->value(), *release);
    if (!pick.has_value()) {
        printLine(stdout, u"pick: none within ±3 s"_s);
        return selecta::cli::kExitOk;
    }
    printLine(stdout,
              u"pick: %1 \"%2\" — %3"_s.arg(pick->candidate.id, pick->candidate.title, pick->reasons.join(u" · "_s)));
    if (!parser.isSet(u"fetch"_s)) {
        return selecta::cli::kExitOk;
    }
    return fetch(runner, pick->candidate.id, QFileInfo(folder).absoluteFilePath());
}

const selecta::cli::CommandRegistration registration{
    u"meta-ytdlp-search", u"[--length m:ss] [--fetch folder] [--verbose] \"artist - title\"",
    u"Search YouTube with your yt-dlp and show the ±3 s pick (optionally fetch it). Manual smoke tool.", &run};

} // namespace
