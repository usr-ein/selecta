// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/YtDlpOutput.h"

#include "meta/ServicePolicy.h"
#include "meta/detail/Json.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

#include <array>
#include <cmath>
#include <initializer_list>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail::ytdlp {

namespace {

constexpr qsizetype kMaxSearchText = 200;
constexpr qsizetype kMaxReason = 200;
constexpr double kMaxBytes = 9007199254740992.0; // 2^53
constexpr int kMaxDurationSec = 24 * 3600;

const QLatin1StringView kProgressMarker{"SELECTA-PROGRESS "};
const QLatin1StringView kFormatMarker{"SELECTA-FORMAT "};
const QLatin1StringView kFileMarker{"SELECTA-FILE "};

MetaError ytError(ErrorCode code, const QString &message)
{
    return makeError(Service::YtDlp, code, message);
}

bool containsAny(const QString &haystack, std::initializer_list<QLatin1StringView> needles)
{
    return std::ranges::any_of(needles, [&](QLatin1StringView needle) { return haystack.contains(needle); });
}

// A count of bytes printed by a template field: a number (yt-dlp prints estimates as floats) or "NA".
std::optional<qint64> bytesField(QByteArrayView field)
{
    bool ok = false;
    const double value = QByteArray(field.data(), field.size()).toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < 0 || value >= kMaxBytes) {
        return std::nullopt;
    }
    return static_cast<qint64>(value);
}

QList<QByteArrayView> fields(QByteArrayView line)
{
    QList<QByteArrayView> out;
    qsizetype i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line.at(i) == ' ' || line.at(i) == '\t')) {
            ++i;
        }
        const qsizetype start = i;
        while (i < line.size() && line.at(i) != ' ' && line.at(i) != '\t') {
            ++i;
        }
        if (i > start) {
            out.append(line.sliced(start, i - start));
        }
    }
    return out;
}

// The text of the first "ERROR:" line without its "[extractor] id: " prefix, home folder hidden, bounded.
QString reasonOf(const QList<QByteArray> &errorLines)
{
    static const QRegularExpression extractorPrefix(uR"(^\[[^\]]{1,40}\]\s*(?:[A-Za-z0-9_-]{1,64}:\s*)?)"_s);
    for (const QByteArray &line : errorLines) {
        if (!line.startsWith("ERROR:")) {
            continue;
        }
        QString reason = QString::fromUtf8(line.sliced(6)).trimmed();
        reason.remove(extractorPrefix);
        const QString home = QDir::homePath();
        if (home.size() > 1) {
            reason.replace(home, u"~"_s);
        }
        return json::cleanText(reason, kMaxReason);
    }
    return {};
}

std::optional<MetaError> youTubeRefusal(const QString &text)
{
    if (containsAny(text, {"rate-limited by youtube"_L1, "rate limited by youtube"_L1, "http error 429"_L1,
                           "too many requests"_L1})) {
        MetaError e = ytError(ErrorCode::RateLimited, u"YouTube is rate-limiting previews for up to an hour."_s);
        e.retryAfter = std::chrono::duration_cast<std::chrono::seconds>(kYtDlpPauseAfterRateLimit);
        return e;
    }
    if (text.contains("captcha"_L1)) {
        MetaError e = ytError(ErrorCode::BotCheck, u"YouTube asked for a captcha. Selecta doesn't use accounts, so "
                                                   u"YouTube previews are paused for an hour."_s);
        e.retryAfter = std::chrono::duration_cast<std::chrono::seconds>(kYtDlpPauseAfterRateLimit);
        return e;
    }
    return std::nullopt;
}

std::optional<MetaError> videoProblem(const QString &text)
{
    if (containsAny(text, {"confirm your age"_L1, "age-restricted"_L1, "age restricted"_L1,
                           "inappropriate for some users"_L1})) {
        return ytError(ErrorCode::AgeRestricted, u"Age-restricted on YouTube."_s);
    }
    if (text.contains("private video"_L1)) {
        return ytError(ErrorCode::Unavailable, u"This YouTube video is private."_s);
    }
    if (containsAny(text, {"not available from your location"_L1, "geo restriction"_L1, "geo-restrict"_L1,
                           "available in your country"_L1})) {
        return ytError(ErrorCode::GeoBlocked, u"This YouTube video isn't available in your region."_s);
    }
    if (containsAny(text, {"drm protected"_L1, "drm-protected"_L1})) {
        return ytError(ErrorCode::Unavailable, u"This YouTube video is DRM-protected."_s);
    }
    if (containsAny(text,
                    {"video unavailable"_L1, "has been removed"_L1, "no longer available"_L1,
                     "this video is unavailable"_L1, "this video is not available"_L1, "has been terminated"_L1,
                     "this live event will begin"_L1, "premieres in"_L1, "members-only"_L1, "join this channel"_L1})) {
        return ytError(ErrorCode::Unavailable, u"This YouTube video is unavailable."_s);
    }
    return std::nullopt;
}

std::optional<MetaError> toolProblem(const QString &text, bool runtimePassed)
{
    const bool runtimeMissing = text.contains("no supported javascript runtime"_L1);
    if (containsAny(text, {"requested format is not available"_L1, "challenge solving failed"_L1,
                           "signature solving failed"_L1})) {
        if (runtimeMissing || !runtimePassed) {
            return ytError(ErrorCode::ToolMissingRuntime,
                           u"yt-dlp needs Deno for YouTube. Install it with `brew install deno`."_s);
        }
        return ytError(ErrorCode::ToolTooOld, u"yt-dlp couldn't read this YouTube page. Updating yt-dlp usually fixes "
                                              u"this: `brew upgrade yt-dlp`."_s);
    }
    if (text.contains("http error 403"_L1)) {
        return ytError(ErrorCode::ToolTooOld,
                       u"YouTube refused the audio; yt-dlp may be out of date: `brew upgrade yt-dlp`."_s);
    }
    if (text.contains("no such option"_L1)) {
        return ytError(ErrorCode::ToolTooOld, u"This yt-dlp doesn't know an option Selecta needs. Update it with "
                                              u"`brew upgrade yt-dlp`."_s);
    }
    if (containsAny(text, {"larger than max-filesize"_L1, "file is larger than"_L1})) {
        return ytError(ErrorCode::TooLarge, u"The YouTube audio is larger than Selecta allows for a preview."_s);
    }
    return std::nullopt;
}

} // namespace

QStringList safetyArguments(const QString &denoPath, const QString &cacheDir)
{
    QStringList arguments{u"--ignore-config"_s, u"--no-plugin-dirs"_s, u"--no-cookies"_s,
                          u"--no-cookies-from-browser"_s, u"--no-remote-components"_s};
    if (!denoPath.isEmpty()) {
        arguments << u"--js-runtimes"_s << QString(u"deno:"_s + denoPath);
    }
    if (cacheDir.isEmpty()) {
        arguments << u"--no-cache-dir"_s;
    } else {
        arguments << u"--cache-dir"_s << cacheDir;
    }
    arguments << u"--color"_s << u"never"_s << u"--socket-timeout"_s << u"15"_s << u"--retries"_s << u"2"_s
              << u"--fragment-retries"_s << u"2"_s;
    return arguments;
}

QStringList versionArguments(const QString &denoPath, const QString &cacheDir)
{
    return safetyArguments(denoPath, cacheDir) << u"--version"_s;
}

QStringList searchArguments(const QString &denoPath, const QString &cacheDir, const QString &text, int count)
{
    return safetyArguments(denoPath, cacheDir) << u"--sleep-requests"_s << u"0.75"_s << u"--flat-playlist"_s << u"-J"_s
                                               << u"--"_s << u"ytsearch%1:%2"_s.arg(count).arg(text);
}

QStringList fetchArguments(const QString &denoPath, const QString &cacheDir, const QString &videoId,
                           const QString &directory)
{
    return safetyArguments(denoPath, cacheDir)
           << u"--sleep-requests"_s << u"0.75"_s << u"--no-playlist"_s << u"--abort-on-error"_s
           << u"--force-overwrites"_s << u"--max-filesize"_s << u"200M"_s << u"-f"_s
           << u"bestaudio[ext=m4a]/bestaudio[acodec=opus]/bestaudio"_s << u"-P"_s << directory << u"-o"_s
           << u"%(id)s.%(ext)s"_s << u"--no-part"_s << u"--no-mtime"_s << u"--fixup"_s << u"never"_s << u"-q"_s
           << u"--progress"_s << u"--newline"_s << u"--progress-template"_s
           << u"download:SELECTA-PROGRESS %(progress.downloaded_bytes)s "
              u"%(progress.total_bytes,progress.total_bytes_estimate)s %(progress.eta)s"_s
           << u"--print"_s << u"before_dl:SELECTA-FORMAT %(format_id)s %(ext)s %(acodec)s %(abr)s %(duration)s"_s
           << u"--print"_s << u"after_move:SELECTA-FILE %(filepath)s"_s << u"--"_s
           << QString(u"https://www.youtube.com/watch?v="_s + videoId);
}

bool isValidVideoId(QStringView id)
{
    static const QRegularExpression pattern(uR"(^[A-Za-z0-9_-]{11}$)"_s);
    return pattern.matchView(id).hasMatch();
}

QString cleanSearchText(const QString &text)
{
    return json::cleanText(text, kMaxSearchText);
}

std::optional<QDate> parseVersion(const QByteArray &standardOutput)
{
    static const QRegularExpression pattern(uR"(^\s*(\d{4})\.(\d{1,2})\.(\d{1,2})(?:\.\d+)?(?:\s|$))"_s);
    const QList<QByteArray> lines = standardOutput.split('\n');
    for (const QByteArray &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(line.left(64)));
        if (!match.hasMatch()) {
            return std::nullopt;
        }
        const QDate date(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
        return date.isValid() ? std::optional<QDate>(date) : std::nullopt;
    }
    return std::nullopt;
}

QString versionText(const QByteArray &standardOutput)
{
    const QList<QByteArray> lines = standardOutput.split('\n');
    for (const QByteArray &line : lines) {
        const QList<QByteArrayView> words = fields(line);
        if (!words.isEmpty()) {
            return json::cleanText(QString::fromUtf8(words.first()), 32);
        }
    }
    return {};
}

Result<QList<YouTubeCandidate>> parseSearch(const QByteArray &standardOutput)
{
    const Result<QJsonObject> json = json::parseObject(standardOutput, Service::YtDlp);
    if (!json.ok()) {
        return json.error();
    }
    if (!json.value().value("entries"_L1).isArray()) {
        return ytError(ErrorCode::BadResponse, u"yt-dlp's search answer has no entries."_s);
    }
    QList<YouTubeCandidate> candidates;
    const QJsonArray entries = json::array(json.value(), "entries"_L1);
    for (const auto &value : entries) {
        if (candidates.size() >= YtDlpRunner::kMaxSearchResults) {
            break;
        }
        const QJsonObject entry = value.toObject();
        const QString type = json::text(entry, "_type"_L1, 32);
        const QString extractor = json::text(entry, "ie_key"_L1, 64);
        YouTubeCandidate candidate;
        candidate.id = json::text(entry, "id"_L1, 32);
        if (!isValidVideoId(candidate.id) || (!type.isEmpty() && type != u"url"_s && type != u"video"_s) ||
            (!extractor.isEmpty() && extractor != u"Youtube"_s)) {
            continue; // channels, playlists, broken entries
        }
        candidate.title = json::text(entry, "title"_L1, 300);
        candidate.channel = json::text(entry, "channel"_L1, 200);
        if (candidate.channel.isEmpty()) {
            candidate.channel = json::text(entry, "uploader"_L1, 200);
        }
        candidate.channelId = json::text(entry, "channel_id"_L1, 64);
        if (const std::optional<double> seconds = json::number(entry, "duration"_L1);
            seconds.has_value() && std::isfinite(*seconds) && *seconds > 0 && *seconds < kMaxDurationSec) {
            candidate.durationSec = static_cast<int>(std::lround(*seconds));
        }
        if (const std::optional<qint64> views = json::integer(entry, "view_count"_L1);
            views.has_value() && *views >= 0) {
            candidate.viewCount = views;
        }
        candidate.liveStatus = json::text(entry, "live_status"_L1, 32);
        candidate.channelVerified = json::boolean(entry, "channel_is_verified"_L1).value_or(false);
        candidates.append(std::move(candidate));
    }
    return candidates;
}

std::optional<Progress> parseProgress(QByteArrayView line)
{
    if (!line.startsWith(QByteArrayView(kProgressMarker.data(), kProgressMarker.size()))) {
        return std::nullopt;
    }
    const QList<QByteArrayView> parts = fields(line.sliced(kProgressMarker.size()));
    if (parts.isEmpty()) {
        return std::nullopt;
    }
    const std::optional<qint64> downloaded = bytesField(parts.at(0));
    if (!downloaded.has_value()) {
        return std::nullopt;
    }
    Progress progress;
    progress.downloadedBytes = *downloaded;
    if (parts.size() > 1) {
        progress.totalBytes = bytesField(parts.at(1)).value_or(0);
    }
    return progress;
}

std::optional<Format> parseFormat(QByteArrayView line)
{
    if (!line.startsWith(QByteArrayView(kFormatMarker.data(), kFormatMarker.size()))) {
        return std::nullopt;
    }
    const QList<QByteArrayView> parts = fields(line.sliced(kFormatMarker.size()));
    if (parts.size() < 2) {
        return std::nullopt;
    }
    static const QRegularExpression extension(uR"(^[a-z0-9]{1,5}$)"_s);
    static const QRegularExpression token(uR"(^[A-Za-z0-9._+-]{1,64}$)"_s);
    Format format;
    format.formatId = QString::fromLatin1(parts.at(0).left(64));
    format.extension = QString::fromLatin1(parts.at(1));
    if (!extension.match(format.extension).hasMatch() || !token.match(format.formatId).hasMatch()) {
        return std::nullopt;
    }
    if (parts.size() > 2) {
        const QString codec = QString::fromLatin1(parts.at(2).left(64));
        if (codec != u"none"_s && codec != u"NA"_s && token.match(codec).hasMatch()) {
            format.codec = codec;
        }
    }
    return format;
}

std::optional<QString> parseFile(QByteArrayView line)
{
    if (!line.startsWith(QByteArrayView(kFileMarker.data(), kFileMarker.size()))) {
        return std::nullopt;
    }
    QString path = QString::fromUtf8(line.sliced(kFileMarker.size())).trimmed();
    if (path.isEmpty()) {
        return std::nullopt;
    }
    return path;
}

MetaError classifyFailure(const QList<QByteArray> &errorLines, int exitCode, bool runtimePassed)
{
    QString text;
    for (const QByteArray &line : errorLines) {
        text += QString::fromUtf8(line).toLower();
        text += u'\n';
    }
    if (std::optional<MetaError> e = youTubeRefusal(text)) {
        return *e;
    }
    if (std::optional<MetaError> e = videoProblem(text)) {
        return *e;
    }
    // "Sign in to confirm you're not a bot" (any sign-in request left after age and private videos).
    if (containsAny(text, {"not a bot"_L1, "sign in"_L1})) {
        MetaError e =
            ytError(ErrorCode::BotCheck, u"YouTube wants a sign-in to prove you're not a bot. Selecta doesn't "
                                         u"use accounts, so YouTube previews are paused for an hour."_s);
        e.retryAfter = std::chrono::duration_cast<std::chrono::seconds>(kYtDlpPauseAfterRateLimit);
        return e;
    }
    if (std::optional<MetaError> e = toolProblem(text, runtimePassed)) {
        return *e;
    }
    if (containsAny(text,
                    {"urlopen error"_L1, "unable to download api page"_L1, "unable to download webpage"_L1,
                     "timed out"_L1, "nodename nor servname"_L1, "name or service not known"_L1,
                     "temporary failure in name resolution"_L1, "network is unreachable"_L1, "connection refused"_L1,
                     "connection reset"_L1, "failed to resolve"_L1, "transporterror"_L1})) {
        return ytError(ErrorCode::Network, u"yt-dlp couldn't reach YouTube."_s);
    }
    const QString reason = reasonOf(errorLines);
    if (exitCode == 2) {
        return ytError(ErrorCode::ToolFailed, reason.isEmpty()
                                                  ? u"yt-dlp rejected Selecta's command line."_s
                                                  : u"yt-dlp rejected Selecta's command line: %1"_s.arg(reason));
    }
    return ytError(ErrorCode::ToolFailed,
                   reason.isEmpty() ? u"yt-dlp failed (exit %1)."_s.arg(exitCode) : u"yt-dlp failed: %1"_s.arg(reason));
}

bool startsPause(ErrorCode code)
{
    return code == ErrorCode::RateLimited || code == ErrorCode::BotCheck;
}

} // namespace selecta::meta::detail::ytdlp
