// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GovernorPacer.h"
#include "base/Clock.h"
#include "governor/Governor.h"
#include "meta/DeezerClient.h"
#include "meta/MetaCache.h"
#include "meta/MusicBrainzClient.h"
#include "meta/Release.h"
#include "meta/ReleaseResolver.h"
#include "meta/SpotifyAuth.h"
#include "meta/SpotifyClient.h"
#include "meta/TokenStore.h"
#include "meta/YtDlpRunner.h"

#include <QByteArray>
#include <QCommandLineParser>
#include <QEventLoop>
#include <QFuture>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>

// Shared plumbing for selecta-cli's meta commands (meta-search, meta-isrc, meta-ytdlp-search). They are smoke tools
// run by hand against the real services, sparingly (docs/contracts/meta.md M6); tests never run them.
namespace selecta::cli {

// The Spotify Client ID and secret for client-credentials smoke runs: SPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET from
// the environment, else from a secrets file (`file`, or the first local/secrets.env found walking up from the working
// directory). The values are never printed or logged; `origin` says where they came from.
struct SpotifySecrets
{
    QString clientId;
    QByteArray clientSecret;
    QString origin;

    bool complete() const { return !clientId.isEmpty() && !clientSecret.isEmpty(); }
};
SpotifySecrets loadSpotifySecrets(const QString &file);

// "6:31" for 391 000 ms; "?" when unknown.
QString formatLength(std::optional<qint64> durationMs);
// "6:31" or "391" → milliseconds; nullopt when unreadable.
std::optional<qint64> parseLength(const QString &text);

// One candidate as two or three lines (score, credit, title, length, source; album, ISRC, IDs; reasons).
QString describeCandidate(int rank, const meta::Candidate &candidate);
QString describeResolution(const meta::Resolution &resolution);

class MetaSession;

// Asks MusicBrainz for the release's label, catalogue number and genres (D30) and prints them. False on a failure
// other than "MusicBrainz has nothing".
bool printEnrichment(const meta::Release &release, meta::MusicBrainzClient &musicBrainz);

// The meta services, wired to a real Governor on the steady clock and a memory cache. Spotify is used only with a
// Client ID and secret (client credentials: catalogue only); Deezer and MusicBrainz need no key; yt-dlp is the user's
// own, found as the app would find it.
class MetaSession
{
public:
    struct Options
    {
        bool verbose = false;
        bool spotify = true;
        QString secretsFile;
    };

    // The options every meta command takes: --verbose, --no-spotify, --secrets <file>.
    static void addCommonOptions(QCommandLineParser &parser);
    static Options commonOptions(const QCommandLineParser &parser);

    explicit MetaSession(const Options &options);
    ~MetaSession();
    MetaSession(const MetaSession &) = delete;
    MetaSession &operator=(const MetaSession &) = delete;
    MetaSession(MetaSession &&) = delete;
    MetaSession &operator=(MetaSession &&) = delete;

    // Signs in to Spotify when it can, and prints which sources are on (never the Client ID or secret).
    void start();

    bool spotifyOn() const { return m_spotifyOn; }
    meta::ReleaseResolver &resolver() { return *m_resolver; }
    meta::MusicBrainzClient &musicBrainz() { return *m_musicBrainz; }
    meta::YtDlpRunner &ytDlp(const QString &executable);
    meta::RequestPacer &pacer() { return m_pacer; }

    // Runs the event loop until `future` finishes, at most `timeout`; nullopt when it didn't or ended canceled.
    template <typename T>
    static std::optional<T> wait(const QFuture<T> &future,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(120))
    {
        if (!future.isFinished()) {
            QEventLoop loop;
            QFutureWatcher<T> watcher;
            QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
            watcher.setFuture(future);
            loop.exec();
        }
        if (!future.isFinished() || future.isCanceled() || future.resultCount() == 0) {
            return std::nullopt;
        }
        return future.result();
    }

private:
    Options m_options;
    base::SteadyClock m_clock;
    QObject m_context;
    governor::Governor m_governor;
    GovernorPacer m_pacer;
    QNetworkAccessManager m_network;
    meta::MemoryMetaCache m_cache;
    meta::InMemoryTokenStore m_tokens;
    SpotifySecrets m_secrets;
    std::unique_ptr<meta::SpotifyAuth> m_auth;
    std::unique_ptr<meta::SpotifyClient> m_spotify;
    std::unique_ptr<meta::DeezerClient> m_deezer;
    std::unique_ptr<meta::MusicBrainzClient> m_musicBrainz;
    std::unique_ptr<meta::ReleaseResolver> m_resolver;
    std::unique_ptr<meta::YtDlpRunner> m_ytDlp;
    bool m_spotifyOn = false;
};

} // namespace selecta::cli
