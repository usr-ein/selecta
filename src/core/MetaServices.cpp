// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/MetaServices.h"

#include "meta/TokenStore.h"

#include <QFuture>
#include <QLoggingCategory>
#include <QNetworkAccessManager>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcMeta, "selecta.core.meta")

using StoredSecret = meta::Result<std::optional<QByteArray>>;

} // namespace

MetaServices::MetaServices(governor::Governor &governor, ActivityLog &activity, const base::Clock &clock,
                           Options options, QObject *parent)
    : QObject(parent)
    , m_network(std::make_unique<QNetworkAccessManager>())
    , m_pacer(governor, activity, this)
    , m_tokens(std::move(options.tokens))
    , m_cache(clock)
{
    Q_ASSERT(m_tokens);
    qRegisterMetaType<SpotifyStatus>();
    meta::SpotifyAuth::Config config;
    config.openUrl = std::move(options.openUrl);
    if (!options.endpoints.spotifyAccounts.isEmpty()) {
        config.accountsUrl = options.endpoints.spotifyAccounts;
    }
    m_spotifyAuth = std::make_unique<meta::SpotifyAuth>(std::move(config), m_network.get(), m_pacer, *m_tokens, clock);
    m_spotifyClient =
        options.endpoints.spotifyApi.isEmpty()
            ? std::make_unique<meta::SpotifyClient>(*m_spotifyAuth, m_network.get(), m_pacer, &m_cache, clock)
            : std::make_unique<meta::SpotifyClient>(*m_spotifyAuth, m_network.get(), m_pacer, &m_cache, clock, nullptr,
                                                    options.endpoints.spotifyApi);
    m_deezer = options.endpoints.deezerApi.isEmpty()
                   ? std::make_unique<meta::DeezerClient>(m_network.get(), m_pacer, &m_cache, clock)
                   : std::make_unique<meta::DeezerClient>(m_network.get(), m_pacer, &m_cache, clock, nullptr,
                                                          options.endpoints.deezerApi);
    m_resolver = std::make_unique<meta::ReleaseResolver>(m_spotifyClient.get(), m_deezer.get());
    m_clips = std::make_unique<meta::ClipSource>(*m_deezer);
    m_musicBrainz = options.endpoints.musicBrainzApi.isEmpty()
                        ? std::make_unique<meta::MusicBrainzClient>(m_network.get(), m_pacer, &m_cache, clock)
                        : std::make_unique<meta::MusicBrainzClient>(m_network.get(), m_pacer, &m_cache, clock, nullptr,
                                                                    options.endpoints.musicBrainzApi);
    m_ytDlp = std::make_unique<meta::YtDlpRunner>(
        options.endpoints.ytDlp ? meta::YtDlpRunner::Config{} : meta::YtDlpRunner::Config::disabled(), m_pacer, clock);
    if (options.endpoints.coverDownloads) {
        meta::CoverFetcher::Config covers;
        if (!options.endpoints.coverArtArchive.isEmpty()) {
            covers.coverArtArchiveUrl = options.endpoints.coverArtArchive;
        }
        m_covers = std::make_unique<meta::CoverFetcher>(std::move(covers), m_network.get(), m_pacer, clock);
    }
    // Spotify data cached for a sign-in goes with it (Developer Policy §I.2).
    connect(m_spotifyAuth.get(), &meta::SpotifyAuth::signedOut, this, [this] { m_cache.removePrefix(u"spotify"_s); });
    connect(m_spotifyAuth.get(), &meta::SpotifyAuth::stateChanged, this, &MetaServices::publish);
    connect(m_spotifyAuth.get(), &meta::SpotifyAuth::persistFailed, this, [this](const meta::MetaError &error) {
        m_error = tr("Signed in, but the sign-in couldn't be saved: %1").arg(error.message);
        publish();
    });
}

MetaServices::~MetaServices()
{
    // The resolver uses the clients, and the clients use the sign-in.
    m_ytDlp.reset(); // kills a running yt-dlp and its children
    m_covers.reset();
    m_musicBrainz.reset();
    m_resolver.reset();
    m_clips.reset();
    m_deezer.reset();
    m_spotifyClient.reset();
    m_spotifyAuth.reset();
}

void MetaServices::restore()
{
    m_tokens->read(QString(kClientIdAccount)).then(this, [this](const StoredSecret &id) {
        if (!id.ok()) {
            m_error = tr("Couldn't read the Spotify Client ID: %1").arg(id.error().message);
            publish();
            return;
        }
        const QString clientId = id.value() ? QString::fromUtf8(*id.value()).trimmed() : QString();
        if (clientId.isEmpty()) {
            m_spotifyAuth->setClientId({});
            publish();
            return;
        }
        m_tokens->read(QString(kClientSecretAccount)).then(this, [this, clientId](const StoredSecret &secret) {
            m_secret = secret.ok() && secret.value() ? *secret.value() : QByteArray();
            applyCredentials(clientId);
        });
    });
}

void MetaServices::setSpotifyCredentials(const QString &clientId, const QByteArray &secret, bool keepSavedSecret)
{
    const auto reportStoreFailure = [this](const meta::Result<void> &result) {
        if (!result.ok()) {
            m_error = tr("Couldn't save the Spotify credentials: %1").arg(result.error().message);
            publish();
        }
    };
    const QString id = clientId.trimmed();
    if (id.isEmpty()) {
        m_secret.clear();
        if (m_spotifyAuth->state() != meta::SpotifyAuth::State::NotConfigured) {
            m_spotifyAuth->signOut().then(this, [](const meta::Result<void> &) {});
        }
        m_tokens->remove(QString(kClientIdAccount)).then(this, reportStoreFailure);
        m_tokens->remove(QString(kClientSecretAccount)).then(this, reportStoreFailure);
        m_spotifyAuth->setClientId({});
        m_error.clear();
        publish();
        return;
    }
    m_tokens->write(QString(kClientIdAccount), id.toUtf8()).then(this, reportStoreFailure);
    if (!secret.isEmpty()) {
        m_secret = secret;
        m_tokens->write(QString(kClientSecretAccount), secret).then(this, reportStoreFailure);
    } else if (!keepSavedSecret) {
        m_secret.clear();
        m_tokens->remove(QString(kClientSecretAccount)).then(this, reportStoreFailure);
    }
    applyCredentials(id);
}

void MetaServices::applyCredentials(const QString &clientId)
{
    m_error.clear();
    m_spotifyAuth->setClientId(clientId);
    if (!m_secret.isEmpty()) {
        m_spotifyAuth->signInWithClientSecret(m_secret).then(this,
                                                             [this](const meta::Result<void> &r) { finished(r); });
    } else {
        m_spotifyAuth->restore().then(this, [this](const meta::Result<void> &r) { finished(r); });
    }
    publish();
}

void MetaServices::signInToSpotify()
{
    m_error.clear();
    m_spotifyAuth->signIn(meta::SpotifyAuth::playlistScopes()).then(this, [this](const meta::Result<void> &r) {
        finished(r);
    });
    publish();
}

void MetaServices::signOutOfSpotify()
{
    m_spotifyAuth->signOut().then(this, [this](const meta::Result<void> &r) { finished(r); });
}

void MetaServices::finished(const meta::Result<void> &result)
{
    if (result.ok()) {
        m_error.clear();
    } else {
        m_error = result.error().message;
        qCInfo(lcMeta).noquote() << "Spotify:" << result.error().toString();
    }
    publish();
}

SpotifyStatus MetaServices::spotifyStatus() const
{
    return SpotifyStatus{.state = m_spotifyAuth->state(),
                         .mode = m_spotifyAuth->mode(),
                         .clientId = m_spotifyAuth->clientId(),
                         .hasSecret = !m_secret.isEmpty(),
                         .expiresAt = m_spotifyAuth->signInExpiresAt(),
                         .error = m_error};
}

void MetaServices::publish()
{
    Q_EMIT spotifyStatusChanged(spotifyStatus());
}

} // namespace selecta::core
