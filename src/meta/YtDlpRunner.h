// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "meta/Release.h"
#include "meta/Result.h"
#include "meta/TextMatcher.h"

#include <QDate>
#include <QFuture>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace selecta::meta {

class RequestPacer;

struct YtDlpInstall
{
    QString path;       // canonical path of the executable that will run
    QString version;    // as yt-dlp prints it: "2026.08.19"
    QDate versionDate;
    QString denoPath;   // the JavaScript runtime yt-dlp needs for YouTube, or empty when none was found
    bool stale = false; // more than 90 days older than today: suggest `brew upgrade yt-dlp`
};

struct YouTubeCandidate
{
    QString id; // 11 characters of [A-Za-z0-9_-]
    QString title;
    QString channel;
    QString channelId;
    std::optional<int> durationSec;
    std::optional<qint64> viewCount;
    QString liveStatus; // "not_live", "is_live", "is_upcoming", "was_live", or empty

    QUrl watchUrl() const; // https://www.youtube.com/watch?v=<id>
};

struct YouTubePick
{
    YouTubeCandidate candidate;
    QStringList reasons; // "length Δ 1 s", "official Topic channel"…
};

struct FetchedAudio
{
    QString videoId;
    QString path;   // inside the preview cache folder
    QString format; // container extension: "m4a", "webm"…
    QString codec;  // "mp4a.40.2", "opus"…, when yt-dlp says
    qint64 bytes = 0;
};

class YtDlpFetch;

// Runs the user's own yt-dlp for previews (D26, PREV-11; references/metadata-and-preview-services.md §12). The only
// place in Selecta that starts yt-dlp:
// - an external process with an argument list (QProcess, no shell), a minimal environment and its own session, so a
//   kill also ends anything yt-dlp started;
// - always --ignore-config, --no-plugin-dirs, --no-cookies, --no-cookies-from-browser, --no-remote-components; never
//   cookies, accounts, updates, --exec or config files;
// - one process at a time, launches ≥ 2 s apart and ≤ 30 per 10 minutes (kYtDlpPolicy), each YouTube run with a
//   governor permit (Operation::YtDlpRun); after a YouTube rate limit or bot check, no yt-dlp for 60 minutes;
// - output only into the preview cache folder the caller gives. The audio is never filed, shared, converted or
//   tagged, and Selecta never bundles, downloads, updates or reimplements yt-dlp.
//
// Threading: lives on the meta thread; futures and job signals arrive there. Ownership: the pacer must outlive the
// runner. Destroying the runner kills a running process and fails queued work (futures canceled, jobs Cancelled).
class YtDlpRunner : public QObject
{
    Q_OBJECT

public:
    // yt-dlp from before this date predates the JavaScript-runtime support and the safety flags Selecta passes.
    static QDate minimumVersion();
    static constexpr int kStaleDays = 90;
    static constexpr int kMaxSearchResults = 20;

    struct Config
    {
        QString executable;              // the path set in Settings; empty: look in searchDirectories
        QStringList searchDirectories = defaultSearchDirectories();
        QString ytDlpCacheDir;           // yt-dlp's own cache (--cache-dir); empty: --no-cache-dir
        std::chrono::milliseconds versionTimeout{10'000};
        std::chrono::milliseconds searchTimeout{60'000};
        std::chrono::milliseconds idleTimeout{30'000};   // a fetch with no output for this long is killed
        std::chrono::milliseconds totalTimeout{120'000}; // a fetch longer than this is killed
        std::function<QDate()> today = [] { return QDate::currentDate(); };
    };

    // /opt/homebrew/bin, /usr/local/bin, ~/.local/bin (pipx), then the directories of PATH; never the working
    // directory. An app started from the Finder has launchd's minimal PATH, hence the explicit Homebrew paths.
    static QStringList defaultSearchDirectories();

    YtDlpRunner(Config config, RequestPacer &pacer, const base::Clock &clock, QObject *parent = nullptr);
    ~YtDlpRunner() override;
    YtDlpRunner(const YtDlpRunner &) = delete;
    YtDlpRunner &operator=(const YtDlpRunner &) = delete;
    YtDlpRunner(YtDlpRunner &&) = delete;
    YtDlpRunner &operator=(YtDlpRunner &&) = delete;

    // Finds yt-dlp (§12.1): the configured path, else the first candidate in searchDirectories that is a regular
    // executable file, not writable by group or others and owned by the user or root (symlinks resolved); runs
    // `--version` (no network, no permit) and looks for Deno the same way. Fails with ToolNotFound, ToolUnsafe,
    // ToolTooOld (older than minimumVersion()), ToolFailed (version unreadable) or Timeout.
    QFuture<Result<YtDlpInstall>> probe();

    // Searches YouTube without downloading (`ytsearch<N>:` with --flat-playlist -J): up to `count` (1…20)
    // candidates with id, title, channel and duration. InvalidArgument for empty text. Fails like probe(), plus the
    // §12.6 YouTube errors (RateLimited and BotCheck start the 60-minute pause).
    QFuture<Result<QList<YouTubeCandidate>>> search(const QString &text, int count = 10);

    // The search text for a release: main artist, base title, and the version words (remixer + "remix" for a remix).
    static QString searchText(const Release &release, const TextMatcher &matcher);

    // The candidate to fetch for a release (§12.3): length within ±3 s of the release, not live or upcoming, no DJ-mix
    // or derivative junk the release doesn't have, the release's title words present; then the "<main artist> -
    // Topic" channel, a channel named like the artist, most views. Nullopt when none fits (or the release has no
    // length: ±3 s can't be checked). Thread-safe (pure).
    static std::optional<YouTubePick> pick(const QList<YouTubeCandidate> &candidates, const Release &release,
                                           const TextMatcher &matcher);

    // Starts fetching the audio of one video into `previewCacheDir` (which must exist): the job waits for yt-dlp's
    // turn, then reports the growing file, progress, and the result. The caller owns the job (deleting it cancels).
    // Invalid IDs and folders fail the job right away (finished() on the next event-loop turn).
    YtDlpFetch *fetch(const QString &videoId, const QString &previewCacheDir, QObject *parent = nullptr);

    // When previews are paused after a YouTube rate limit or bot check, the time they resume.
    std::optional<base::Clock::TimePoint> pausedUntil() const;

Q_SIGNALS:
    // YouTube rate-limited Selecta or asked for a bot check: no yt-dlp until `until`.
    void paused(const selecta::meta::MetaError &reason);

private:
    friend class YtDlpFetch;
    struct Private;
    std::unique_ptr<Private> d;
};

// One audio fetch (YtDlpRunner::fetch). Lives on the runner's thread.
class YtDlpFetch : public QObject
{
    Q_OBJECT

public:
    ~YtDlpFetch() override; // cancels if not finished
    YtDlpFetch(const YtDlpFetch &) = delete;
    YtDlpFetch &operator=(const YtDlpFetch &) = delete;
    YtDlpFetch(YtDlpFetch &&) = delete;
    YtDlpFetch &operator=(YtDlpFetch &&) = delete;

    QString videoId() const { return m_videoId; }
    QString filePath() const { return m_filePath; } // known once started() was emitted
    bool isFinished() const { return m_finished; }

    // Kills the process (or leaves the queue), deletes the partial file and finishes with Cancelled. No effect once
    // finished.
    void cancel();

Q_SIGNALS:
    // yt-dlp is writing the audio to `filePath` (no .part file: a reader may open it as it grows).
    void started(const QString &filePath);
    // Bytes written so far; totalBytes is 0 while unknown.
    void progress(qint64 downloadedBytes, qint64 totalBytes);
    // Exactly once. On failure the partial file has been deleted.
    void finished(const selecta::meta::Result<selecta::meta::FetchedAudio> &result);

private:
    friend class YtDlpRunner;
    YtDlpFetch(YtDlpRunner *runner, QString videoId, QString directory, QObject *parent);
    void finish(Result<FetchedAudio> result);

    QPointer<YtDlpRunner> m_runner;
    QString m_videoId;
    QString m_directory;
    QString m_filePath;
    bool m_finished = false;
};

} // namespace selecta::meta

Q_DECLARE_METATYPE(selecta::meta::Result<selecta::meta::FetchedAudio>)
