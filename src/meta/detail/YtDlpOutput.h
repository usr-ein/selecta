// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Result.h"
#include "meta/YtDlpRunner.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// yt-dlp's command lines and output (references/metadata-and-preview-services.md §12): the arguments Selecta passes,
// and strict readers for what comes back. Pure functions, thread-safe.
namespace selecta::meta::detail::ytdlp {

// What every run starts with (§12.5): no configuration files, plugins, cookies or remote components; Deno when one
// was found; Selecta's own cache directory or none; plain output; short network timeouts and few retries.
QStringList safetyArguments(const QString &denoPath, const QString &cacheDir);

QStringList versionArguments(const QString &denoPath, const QString &cacheDir);

// A flat search (§12.2): `ytsearch<count>:<text>`, nothing downloaded. `text` must already be clean.
QStringList searchArguments(const QString &denoPath, const QString &cacheDir, const QString &text, int count);

// One video's audio into `directory` as <id>.<ext> (§12.4): no .part file, no fixups, progress and marker lines.
QStringList fetchArguments(const QString &denoPath, const QString &cacheDir, const QString &videoId,
                           const QString &directory);

// 11 characters of [A-Za-z0-9_-].
bool isValidVideoId(QStringView id);

// Search text without control characters or line breaks, whitespace collapsed, at most 200 characters.
QString cleanSearchText(const QString &text);

// The date of a `--version` answer ("2026.08.19", nightly "2026.08.19.232838"), else nullopt.
std::optional<QDate> parseVersion(const QByteArray &standardOutput);
// The version as printed: the first word of the first line, at most 32 characters.
QString versionText(const QByteArray &standardOutput);

// The entries of a flat search answer. Entries without a valid ID are dropped; at most YtDlpRunner::kMaxSearchResults.
// BadResponse when the answer isn't a JSON object with an `entries` array.
Result<QList<YouTubeCandidate>> parseSearch(const QByteArray &standardOutput);

// The marker lines of a fetch.
struct Progress
{
    qint64 downloadedBytes = 0;
    qint64 totalBytes = 0; // 0 while unknown
};
struct Format
{
    QString formatId;
    QString extension; // [a-z0-9]{1,5}
    QString codec;     // empty when yt-dlp doesn't say
};
std::optional<Progress> parseProgress(QByteArrayView line);
std::optional<Format> parseFormat(QByteArrayView line);
std::optional<QString> parseFile(QByteArrayView line); // the path as printed (may contain spaces)

// The error for a run that failed (§12.6), from its stderr lines and exit code. `runtimePassed`: Deno was given to
// yt-dlp. Messages are short, user-facing and never contain the home folder's path.
MetaError classifyFailure(const QList<QByteArray> &errorLines, int exitCode, bool runtimePassed);

// Errors that pause YouTube previews for an hour (RateLimited, BotCheck).
bool startsPause(ErrorCode code);

} // namespace selecta::meta::detail::ytdlp
