// SPDX-License-Identifier: GPL-3.0-or-later
#include "MetaSession.h"

#include "Command.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <cstdio>

using namespace Qt::StringLiterals;

namespace selecta::cli {

namespace {

constexpr int kMaxParentLevels = 12;
constexpr qsizetype kMaxSecretsFileBytes = qsizetype{64} * 1024;
constexpr int kShownReasons = 4;

QString unquoted(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 &&
        ((value.startsWith(u'"') && value.endsWith(u'"')) || (value.startsWith(u'\'') && value.endsWith(u'\'')))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

// KEY=value lines (an optional "export ", quotes, # comments). Only the two Spotify keys are read.
void readSecretsFile(const QString &path, SpotifySecrets &secrets)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QList<QByteArray> lines = file.read(kMaxSecretsFileBytes).split('\n');
    for (const QByteArray &raw : lines) {
        QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        if (line.startsWith(u"export "_s)) {
            line = line.mid(7).trimmed();
        }
        const qsizetype equals = line.indexOf(u'=');
        if (equals <= 0) {
            continue;
        }
        const QString key = line.left(equals).trimmed();
        const QString value = unquoted(line.mid(equals + 1));
        if (key == u"SPOTIFY_CLIENT_ID"_s && secrets.clientId.isEmpty()) {
            secrets.clientId = value;
        } else if (key == u"SPOTIFY_CLIENT_SECRET"_s && secrets.clientSecret.isEmpty()) {
            secrets.clientSecret = value.toUtf8();
        }
    }
    secrets.origin = QFileInfo(path).absoluteFilePath();
}

QString findSecretsFile()
{
    QDir directory(QDir::currentPath());
    for (int level = 0; level < kMaxParentLevels; ++level) {
        const QString candidate = directory.filePath(u"local/secrets.env"_s);
        if (QFileInfo(candidate).isFile()) {
            return candidate;
        }
        if (!directory.cdUp()) {
            break;
        }
    }
    return {};
}

QString artistsOf(const meta::Release &release)
{
    return release.artists.isEmpty() ? u"?"_s : release.artists.join(u", "_s);
}

} // namespace

SpotifySecrets loadSpotifySecrets(const QString &file)
{
    SpotifySecrets secrets;
    secrets.clientId = qEnvironmentVariable("SPOTIFY_CLIENT_ID").trimmed();
    secrets.clientSecret = qEnvironmentVariable("SPOTIFY_CLIENT_SECRET").trimmed().toUtf8();
    if (secrets.complete()) {
        secrets.origin = u"the environment"_s;
        return secrets;
    }
    const QString path = file.isEmpty() ? findSecretsFile() : file;
    if (!path.isEmpty()) {
        readSecretsFile(path, secrets);
    }
    return secrets;
}

QString formatLength(std::optional<qint64> durationMs)
{
    if (!durationMs.has_value() || *durationMs <= 0) {
        return u"?"_s;
    }
    const qint64 seconds = (*durationMs + 500) / 1000;
    return u"%1:%2"_s.arg(seconds / 60).arg(seconds % 60, 2, 10, u'0');
}

std::optional<qint64> parseLength(const QString &text)
{
    static const QRegularExpression pattern(uR"(^\s*(?:(\d{1,3}):)?(\d{1,5})\s*$)"_s);
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    const qint64 minutes = match.captured(1).isEmpty() ? 0 : match.captured(1).toLongLong();
    const qint64 seconds = match.captured(2).toLongLong();
    if (!match.captured(1).isEmpty() && seconds >= 60) {
        return std::nullopt;
    }
    const qint64 total = (minutes * 60) + seconds;
    return total > 0 ? std::optional<qint64>(total * 1000) : std::nullopt;
}

QString describeCandidate(int rank, const meta::Candidate &candidate)
{
    const meta::Release &release = candidate.release;
    QString text =
        u"%1. %2 %3  %4 — %5  (%6)  [%7]\n"_s.arg(rank, 2)
            .arg(meta::confidenceName(candidate.confidence), -9)
            .arg(candidate.score, 0, 'f', 2)
            .arg(artistsOf(release), release.title, formatLength(release.durationMs), meta::sourceName(release.source));
    QStringList facts;
    if (!release.album.isEmpty()) {
        QStringList about;
        for (const QString &part : {release.albumType, release.releaseDate}) {
            if (!part.isEmpty()) {
                about << part;
            }
        }
        facts << (about.isEmpty() ? u"album \"%1\""_s.arg(release.album)
                                  : u"album \"%1\" (%2)"_s.arg(release.album, about.join(u", "_s)));
    }
    if (!release.isrc.isEmpty()) {
        facts << u"ISRC "_s + release.isrc;
    }
    if (!release.label.isEmpty()) {
        facts << u"label "_s + release.label;
    }
    if (!release.spotifyId.isEmpty()) {
        facts << u"spotify:track:"_s + release.spotifyId;
    }
    if (!release.deezerId.isEmpty()) {
        facts << u"deezer:"_s + release.deezerId;
    }
    if (!facts.isEmpty()) {
        text += u"      "_s + facts.join(u" · "_s) + u'\n';
    }
    if (!candidate.reasons.isEmpty()) {
        text += u"      "_s + candidate.reasons.mid(0, kShownReasons).join(u"; "_s) + u'\n';
    }
    return text;
}

QString describeResolution(const meta::Resolution &resolution)
{
    QStringList tried;
    for (const meta::Source source : resolution.sourcesTried) {
        tried << meta::sourceName(source);
    }
    QString text = u"resolution: %1 (asked %2)\n"_s.arg(meta::confidenceName(resolution.confidence),
                                                        tried.isEmpty() ? u"nobody"_s : tried.join(u", "_s));
    for (const meta::MetaError &error : resolution.sourceErrors) {
        text += u"  failed: %1\n"_s.arg(error.toString());
    }
    return text;
}

bool printEnrichment(const meta::Release &release, meta::MusicBrainzClient &musicBrainz)
{
    printLine(stdout, u"MusicBrainz (label and genres, D30) for \"%1\":"_s.arg(release.title));
    const auto result = MetaSession::wait(musicBrainz.enrich(release, meta::Priority::Interactive));
    if (!result.has_value()) {
        printLine(stderr, u"  no answer in time"_s);
        return false;
    }
    if (!result->ok()) {
        printLine(stdout, u"  "_s + result->error().toString());
        return result->error().code == meta::ErrorCode::NotFound;
    }
    const meta::musicbrainz::Enrichment &found = result->value();
    printLine(stdout, u"  label     %1%2"_s.arg(
                          found.label.isEmpty() ? u"(none)"_s : found.label,
                          found.catalogNumber.isEmpty() ? QString() : u" (catalogue %1)"_s.arg(found.catalogNumber)));
    printLine(stdout, u"  genres    %1"_s.arg(found.genres.isEmpty() ? u"(none)"_s : found.genres.join(u", "_s)));
    printLine(stdout, u"  recording %1, release %2"_s.arg(found.recordingId.isEmpty() ? u"?"_s : found.recordingId,
                                                          found.releaseId.isEmpty() ? u"?"_s : found.releaseId));
    for (const QString &reason : found.reasons) {
        printLine(stdout, u"  · "_s + reason);
    }
    return true;
}

void MetaSession::addCommonOptions(QCommandLineParser &parser)
{
    parser.addOption({u"verbose"_s, u"Print every request Selecta makes (scrubbed)."_s});
    parser.addOption({u"no-spotify"_s, u"Don't use Spotify even when a Client ID and secret are known."_s});
    parser.addOption({u"secrets"_s, u"The secrets file (default: local/secrets.env, searched upwards)."_s, u"file"_s});
}

MetaSession::Options MetaSession::commonOptions(const QCommandLineParser &parser)
{
    Options options;
    options.verbose = parser.isSet(u"verbose"_s);
    options.spotify = !parser.isSet(u"no-spotify"_s);
    options.secretsFile = parser.value(u"secrets"_s);
    return options;
}

MetaSession::MetaSession(const Options &options)
    : m_options(options)
    , m_governor(m_clock)
    , m_pacer(m_governor, &m_context, options.verbose)
    , m_cache(m_clock)
{
    // meta's own request log would repeat the --verbose lines; keep the output to what the command prints.
    QLoggingCategory::setFilterRules(u"selecta.meta.*.debug=false\nselecta.meta.*.info=false"_s);
    if (m_options.spotify) {
        m_secrets = loadSpotifySecrets(m_options.secretsFile);
    }
    meta::SpotifyAuth::Config config;
    config.clientId = m_secrets.clientId;
    m_auth = std::make_unique<meta::SpotifyAuth>(config, &m_network, m_pacer, m_tokens, m_clock);
    m_spotify = std::make_unique<meta::SpotifyClient>(*m_auth, &m_network, m_pacer, &m_cache, m_clock);
    m_deezer = std::make_unique<meta::DeezerClient>(&m_network, m_pacer, &m_cache, m_clock);
    m_musicBrainz = std::make_unique<meta::MusicBrainzClient>(&m_network, m_pacer, &m_cache, m_clock);
    m_resolver = std::make_unique<meta::ReleaseResolver>(m_spotify.get(), m_deezer.get());
}

MetaSession::~MetaSession() = default;

void MetaSession::start()
{
    if (!m_options.spotify) {
        printLine(stdout, u"sources: Deezer (Spotify off: --no-spotify)"_s);
        return;
    }
    if (!m_secrets.complete()) {
        printLine(stdout, u"sources: Deezer (Spotify off: no SPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET)"_s);
        return;
    }
    const auto signedIn = wait(m_auth->signInWithClientSecret(m_secrets.clientSecret), std::chrono::seconds(60));
    if (!signedIn.has_value() || !signedIn->ok()) {
        printLine(stdout, u"sources: Deezer (Spotify sign-in failed: %1)"_s.arg(
                              signedIn.has_value() ? signedIn->error().toString() : u"no answer"_s));
        return;
    }
    m_spotifyOn = true;
    printLine(stdout, u"sources: Spotify (client credentials from %1), Deezer"_s.arg(m_secrets.origin));
}

meta::YtDlpRunner &MetaSession::ytDlp(const QString &executable)
{
    if (!m_ytDlp) {
        meta::YtDlpRunner::Config config;
        config.executable = executable;
        m_ytDlp = std::make_unique<meta::YtDlpRunner>(config, m_pacer, m_clock);
    }
    return *m_ytDlp;
}

} // namespace selecta::cli
