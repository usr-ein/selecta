// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/GovernorPacer.h"
#include "meta/CoverFetcher.h"
#include "meta/DeezerClient.h"
#include "meta/MetaCache.h"
#include "meta/MusicBrainzClient.h"
#include "meta/ReleaseResolver.h"
#include "meta/SpotifyAuth.h"
#include "meta/SpotifyClient.h"
#include "meta/YtDlpRunner.h"

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace selecta::base {
class Clock;
}
namespace selecta::governor {
class Governor;
}
namespace selecta::meta {
class TokenStore;
}

namespace selecta::core {

class ActivityLog;

// Where the metadata services are. Empty: the real services. Tests and isolated runs point them at a closed loopback
// port so nothing leaves the machine.
struct MetaEndpoints
{
    QUrl spotifyAccounts;
    QUrl spotifyApi;
    QUrl deezerApi;
    QUrl musicBrainzApi;
    QUrl coverArtArchive;
    // Covers also come straight from the CDNs a release links to (Spotify's, Deezer's), which no URL here redirects:
    // isolated runs turn cover downloads off instead.
    bool coverDownloads = true;
    // The user's own yt-dlp for previews (ADR-011). Isolated runs never find or run it.
    bool ytDlp = true;

    // Every service on 127.0.0.1:1, where nothing listens: requests fail at once. No cover downloads.
    static MetaEndpoints unreachable()
    {
        const QUrl closed(QStringLiteral("http://127.0.0.1:1"));
        return {.spotifyAccounts = closed,
                .spotifyApi = closed,
                .deezerApi = closed,
                .musicBrainzApi = closed,
                .coverArtArchive = closed,
                .coverDownloads = false,
                .ytDlp = false};
    }
};

// What Settings shows about Spotify. The Client ID is shown in its field; the secret never leaves the engine (the UI
// only learns whether one is saved).
struct SpotifyStatus
{
    meta::SpotifyAuth::State state = meta::SpotifyAuth::State::NotConfigured;
    meta::SpotifyAuth::Mode mode = meta::SpotifyAuth::Mode::None;
    QString clientId;
    bool hasSecret = false;
    std::optional<QDateTime> expiresAt; // when Spotify ends a browser sign-in
    QString error;                      // the last failure, in words, until the next success
};

// The metadata services on the engine thread (PRD META-1…8): one network manager, the governor pacer, the token
// store (the Keychain in the app) and Spotify's sign-in. The Spotify Client ID and the optional Client secret
// (META-1: the secret enables the client-credentials flow) are kept in the token store, never in the settings file.
//
// Threading: engine thread. Ownership: owns everything it creates; the Governor and the activity log outlive it.
class MetaServices : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        std::unique_ptr<meta::TokenStore> tokens; // required
        // Opens the browser for a Spotify sign-in; called on the engine thread (the app forwards it to the UI thread).
        std::function<bool(const QUrl &)> openUrl;
        MetaEndpoints endpoints;
    };

    // Token-store accounts for the credentials. Spotify Client IDs are 32 hex digits, so these can't collide with the
    // refresh tokens SpotifyAuth keeps under "spotify/<clientId>".
    static constexpr QLatin1StringView kClientIdAccount{"spotify/client-id"};
    static constexpr QLatin1StringView kClientSecretAccount{"spotify/client-secret"};

    MetaServices(governor::Governor &governor, ActivityLog &activity, const base::Clock &clock, Options options,
                 QObject *parent = nullptr);
    ~MetaServices() override;
    MetaServices(const MetaServices &) = delete;
    MetaServices &operator=(const MetaServices &) = delete;
    MetaServices(MetaServices &&) = delete;
    MetaServices &operator=(MetaServices &&) = delete;

    // Loads the saved Spotify credentials and signs in with them: the client-credentials flow when a secret is saved,
    // otherwise the stored browser sign-in, if any.
    void restore();
    // Saves new credentials and signs in with them. An empty Client ID forgets the credentials and the sign-in. An
    // empty `secret` keeps the saved one when `keepSavedSecret` is set, and deletes it otherwise.
    void setSpotifyCredentials(const QString &clientId, const QByteArray &secret, bool keepSavedSecret);
    // Opens the browser to sign in to Spotify (for playlists and Liked Songs; tags need only the catalogue).
    void signInToSpotify();
    void signOutOfSpotify();

    SpotifyStatus spotifyStatus() const;
    meta::SpotifyAuth &spotifyAuth() { return *m_spotifyAuth; }
    meta::SpotifyClient &spotify() { return *m_spotifyClient; }
    meta::DeezerClient &deezer() { return *m_deezer; }
    // Line → release candidates, from Spotify when signed in and from Deezer (keyless) otherwise.
    meta::ReleaseResolver &resolver() { return *m_resolver; }
    // Label and genre by ISRC (D30), for the arrival pipeline.
    meta::MusicBrainzClient &musicBrainz() { return *m_musicBrainz; }
    // Cover images; null when cover downloads are off (isolated runs).
    meta::CoverFetcher *covers() { return m_covers.get(); }
    // Previews through the user's own yt-dlp (never finds or runs it in isolated runs).
    meta::YtDlpRunner &ytDlp() { return *m_ytDlp; }
    meta::MetaCache &cache() { return m_cache; }
    QNetworkAccessManager &network() { return *m_network; }
    meta::RequestPacer &pacer() { return m_pacer; }
    meta::TokenStore &tokens() { return *m_tokens; }

Q_SIGNALS:
    void spotifyStatusChanged(const selecta::core::SpotifyStatus &status);

private:
    void applyCredentials(const QString &clientId);
    void finished(const meta::Result<void> &result);
    void publish();

    std::unique_ptr<QNetworkAccessManager> m_network;
    GovernorPacer m_pacer;
    std::unique_ptr<meta::TokenStore> m_tokens;
    meta::MemoryMetaCache m_cache;
    std::unique_ptr<meta::SpotifyAuth> m_spotifyAuth;
    std::unique_ptr<meta::SpotifyClient> m_spotifyClient;
    std::unique_ptr<meta::DeezerClient> m_deezer;
    std::unique_ptr<meta::ReleaseResolver> m_resolver;
    std::unique_ptr<meta::MusicBrainzClient> m_musicBrainz;
    std::unique_ptr<meta::CoverFetcher> m_covers;
    std::unique_ptr<meta::YtDlpRunner> m_ytDlp;
    QByteArray m_secret; // in memory only while set; never logged
    QString m_error;
};

} // namespace selecta::core

Q_DECLARE_METATYPE(selecta::core::SpotifyStatus)
