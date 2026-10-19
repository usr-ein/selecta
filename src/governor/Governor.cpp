// SPDX-License-Identifier: GPL-3.0-or-later
#include "governor/Governor.h"

#include "governor/Limits.h"
#include "governor/QueryHygiene.h"
#include "governor/ServerGuard.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::governor {
namespace {

using Duration = base::Clock::Duration;

QString seconds(Duration wait)
{
    const auto value = std::chrono::ceil<std::chrono::seconds>(wait).count();
    if (value < 120) {
        return u"%1 s"_s.arg(std::max<qint64>(1, value));
    }
    return u"%1 min"_s.arg((value + 59) / 60);
}

QString subjectOf(Operation operation, const Request &request)
{
    switch (operation) {
    case Operation::Search:
        return request.query;
    case Operation::PrivateMessage:
    case Operation::RoomMessage:
        return request.recipient;
    case Operation::Login:
        return request.server.toString();
    default:
        return request.peer;
    }
}

QString messageKey(const Request &request)
{
    const QByteArray digest = QCryptographicHash::hash(request.text.trimmed().toUtf8(), QCryptographicHash::Sha256);
    return request.recipient.toCaseFolded() + u'\x1f' + QString::fromLatin1(digest.toHex());
}

} // namespace

Governor::Governor(const base::Clock &clock, Options options, QObject *parent)
    : QObject(parent)
    , m_clock(clock)
    , m_today(options.today ? std::move(options.today) : [] { return QDate::currentDate(); })
    , m_ledger(clock, m_today)
    , m_reconnect(
          clock,
          options.random01 ? std::move(options.random01) : [] { return QRandomGenerator::global()->generateDouble(); })
    , m_searches(clock, limits::kSearchBurst, limits::kSearchRefill)
    , m_peerLookups(clock, limits::kPeerLookupsInFlight, limits::kPeerLookupRefill)
    , m_previews(clock, limits::kPreviewsPer10Min, limits::kPreviewRefill)
    , m_browses(clock, limits::kBrowseBurst, limits::kBrowseRefill)
    , m_userInfos(clock, limits::kUserInfoBurst, limits::kUserInfoRefill)
    , m_folderContents(clock, limits::kFolderContentsBurst, limits::kFolderContentsRefill)
    , m_placeInQueue(clock, limits::kPlaceInQueueBurst, limits::kPlaceInQueueRefill)
    , m_privateMessages(clock, 1, limits::kMessageSpacing)
    , m_roomMessages(clock, 1, limits::kMessageSpacing)
    , m_spotify(clock, limits::kSpotifyBurst, limits::kSpotifyRefill)
    , m_musicBrainz(clock, limits::kMusicBrainzBurst, limits::kMusicBrainzRefill)
    , m_coverArt(clock, limits::kCoverArtBurst, limits::kCoverArtRefill)
    , m_deezer(clock, limits::kDeezerBurst, limits::kDeezerRefill)
    , m_ytDlp(clock, limits::kYtDlpBurst, limits::kYtDlpRefill)
{}

Governor::~Governor() = default;

bool Governor::isSoulseekOperation(Operation operation)
{
    switch (operation) {
    case Operation::SpotifyRequest:
    case Operation::MusicBrainzRequest:
    case Operation::CoverArtRequest:
    case Operation::DeezerRequest:
    case Operation::YtDlpRun:
        return false;
    default:
        return true;
    }
}

Decision Governor::request(Operation operation, const Request &request)
{
    Decision decision = decide(operation, request);
    Q_EMIT decided(operation, subjectOf(operation, request), decision.refusal, decision.explanation);
    if (operation == Operation::Search) {
        Q_EMIT searchBudgetChanged(searchesAvailable());
    }
    return decision;
}

Decision Governor::allow(Operation operation, QString explanation)
{
    Decision decision;
    decision.permit = Permit(operation);
    decision.explanation = std::move(explanation);
    return decision;
}

Decision Governor::decide(Operation operation, const Request &request)
{
    if (m_paused && isSoulseekOperation(operation)) {
        Decision paused;
        paused.refusal = Refusal::NetworkPaused;
        paused.explanation = u"Network activity is paused."_s;
        return paused;
    }
    switch (operation) {
    case Operation::Login:
        return decideLogin(request);
    case Operation::Search:
        return decideSearch(request);
    case Operation::PeerLookup:
        return decideTimed(operation, m_peerLookups, u"Peer lookups"_s);
    case Operation::QueueDownload:
        return decideQueueDownload(request);
    case Operation::PlaceInQueue:
        return decidePerSubject(operation, m_lastPlaceQuery, request.peer, limits::kPlaceInQueuePoll, m_placeInQueue,
                                u"Queue position requests"_s);
    case Operation::Browse:
        return decidePerSubject(operation, m_lastBrowse, request.peer, limits::kBrowseSameUserQuiet, m_browses,
                                u"Browsing"_s);
    case Operation::UserInfo:
        return decidePerSubject(operation, m_lastUserInfo, request.peer, limits::kUserInfoPerUser, m_userInfos,
                                u"User info requests"_s);
    case Operation::FolderContents:
        return decideTimed(operation, m_folderContents, u"Folder requests"_s);
    case Operation::PrivateMessage:
        return decideMessage(operation, request, m_privateMessages);
    case Operation::RoomMessage:
        return decideMessage(operation, request, m_roomMessages);
    case Operation::SpotifyRequest:
        return decideTimed(operation, m_spotify, u"Spotify requests"_s);
    case Operation::MusicBrainzRequest:
        return decideTimed(operation, m_musicBrainz, u"MusicBrainz requests"_s);
    case Operation::CoverArtRequest:
        return decideTimed(operation, m_coverArt, u"Cover downloads"_s);
    case Operation::DeezerRequest:
        return decideTimed(operation, m_deezer, u"Deezer requests"_s);
    case Operation::YtDlpRun:
        return decideTimed(operation, m_ytDlp, u"yt-dlp runs"_s);
    }
    Decision unknown;
    unknown.refusal = Refusal::RateLimited;
    unknown.explanation = u"Unknown operation."_s;
    return unknown;
}

Decision Governor::decideTimed(Operation operation, TokenBucket &bucket, const QString &what)
{
    if (bucket.tryTake()) {
        return allow(operation, u"%1: allowed."_s.arg(what));
    }
    Decision decision;
    decision.refusal = Refusal::RateLimited;
    decision.retryAfter = bucket.timeUntilAvailable();
    decision.explanation = u"%1 are paced; next one in %2."_s.arg(what, seconds(decision.retryAfter));
    return decision;
}

void Governor::forget(std::map<QString, base::Clock::TimePoint> &last, Duration olderThan) const
{
    const auto now = m_clock.now();
    std::erase_if(last, [&](const auto &item) { return now - item.second >= olderThan; });
}

Decision Governor::decidePerSubject(Operation operation, std::map<QString, base::Clock::TimePoint> &last,
                                    const QString &subject, Duration quiet, TokenBucket &bucket, const QString &what)
{
    forget(last, quiet);
    const QString key = subject.toCaseFolded();
    if (const auto found = last.find(key); found != last.end()) {
        Decision decision;
        decision.refusal = Refusal::PeerTooSoon;
        decision.retryAfter = found->second + quiet - m_clock.now();
        decision.explanation =
            u"%1 for %2: asked recently; again in %3."_s.arg(what, subject, seconds(decision.retryAfter));
        return decision;
    }
    Decision decision = decideTimed(operation, bucket, what);
    if (decision.allowed()) {
        last[key] = m_clock.now();
    }
    return decision;
}

Decision Governor::decideMessage(Operation operation, const Request &request, TokenBucket &bucket)
{
    forget(m_lastMessage, limits::kSameMessageQuiet);
    const QString key = messageKey(request);
    if (const auto found = m_lastMessage.find(key); found != m_lastMessage.end()) {
        Decision decision;
        decision.refusal = Refusal::MessageRepeated;
        decision.retryAfter = found->second + limits::kSameMessageQuiet - m_clock.now();
        decision.explanation = u"The same message already went to %1 a moment ago."_s.arg(request.recipient);
        return decision;
    }
    Decision decision = decideTimed(operation, bucket, u"Messages"_s);
    if (decision.allowed()) {
        m_lastMessage[key] = m_clock.now();
    }
    return decision;
}

Decision Governor::decideSearch(const Request &request)
{
    const CleanQuery clean = cleanQuery(request.query, m_excludedPhrases);
    Decision decision;
    switch (clean.refusal) {
    case Refusal::None:
        break;
    case Refusal::QueryTooShort:
        decision.refusal = clean.refusal;
        decision.explanation = u"Searches need at least %1 characters."_s.arg(limits::kQueryMinChars);
        return decision;
    case Refusal::QueryTooLong:
        decision.refusal = clean.refusal;
        decision.explanation = u"Searches can't be longer than %1 characters."_s.arg(limits::kQueryMaxChars);
        return decision;
    default:
        decision.refusal = clean.refusal;
        decision.explanation =
            u"The server excludes \"%1\": peers would return nothing for this search."_s.arg(clean.excludedPhrase);
        return decision;
    }

    forget(m_lastQuery, limits::kSameQueryQuiet);
    const QString key = queryKey(clean.query);
    if (const auto found = m_lastQuery.find(key); found != m_lastQuery.end()) {
        decision.refusal = Refusal::SameQueryRecently;
        decision.retryAfter = found->second + limits::kSameQueryQuiet - m_clock.now();
        decision.explanation = u"This search ran less than %1 min ago; showing its results instead."_s.arg(
            std::chrono::duration_cast<std::chrono::minutes>(limits::kSameQueryQuiet).count());
        return decision;
    }

    // Get tracks leaves part of the bucket for searches typed by hand.
    const int reserve = request.source == SearchSource::GetTracks ? limits::kGetTracksReservedSearches : 0;
    if (m_searches.available() <= reserve) {
        decision.refusal = Refusal::RateLimited;
        decision.retryAfter = m_searches.timeUntilAvailable(reserve + 1);
        decision.explanation =
            u"Searches are paced at 2 per minute; next one in %1."_s.arg(seconds(decision.retryAfter));
        return decision;
    }
    if (!m_searches.tryTake()) {
        decision.refusal = Refusal::RateLimited;
        decision.retryAfter = m_searches.timeUntilAvailable();
        decision.explanation =
            u"Searches are paced at 2 per minute; next one in %1."_s.arg(seconds(decision.retryAfter));
        return decision;
    }
    m_lastQuery[key] = m_clock.now();
    Decision allowed = allow(Operation::Search, u"Search allowed."_s);
    allowed.cleanedQuery = clean.query;
    return allowed;
}

Decision Governor::decideQueueDownload(const Request &request)
{
    Decision decision;
    const QString &peer = request.peer;
    if (const auto backoff = m_ledger.backoffRemaining(peer)) {
        decision.refusal = Refusal::PeerBackoff;
        decision.retryAfter = *backoff;
        decision.explanation = u"%1 refused recent requests; leaving them alone for %2."_s.arg(peer, seconds(*backoff));
        return decision;
    }
    const PeerDay today = m_ledger.today(peer);
    if (today.files >= limits::kPeerDailyStopFiles || today.bytes >= limits::kPeerDailyStopBytes) {
        decision.refusal = Refusal::PeerDailyLimit;
        decision.explanation =
            u"You took %1 files from %2 today, the daily limit. Try again tomorrow."_s.arg(today.files).arg(peer);
        return decision;
    }
    if (m_ledger.filesLastSevenDays(peer) >= limits::kPeerWeeklyStopFiles) {
        decision.refusal = Refusal::PeerWeeklyLimit;
        decision.explanation =
            u"You took %1 files from %2 this week, the weekly limit."_s.arg(m_ledger.filesLastSevenDays(peer))
                .arg(peer);
        return decision;
    }
    if (m_ledger.queuedAt(peer) >= limits::kPeerQueuedFiles) {
        decision.refusal = Refusal::PeerQueueLimit;
        decision.explanation = u"%1 files are already queued at %2."_s.arg(limits::kPeerQueuedFiles).arg(peer);
        return decision;
    }
    if (m_ledger.queuedTotal() >= limits::kQueuedDownloads) {
        decision.refusal = Refusal::GlobalLimit;
        decision.explanation = u"%1 downloads are already queued."_s.arg(limits::kQueuedDownloads);
        return decision;
    }
    if (request.preview) {
        if (m_ledger.activeAt(peer) >= limits::kPeerActiveDownloads) {
            decision.refusal = Refusal::PeerActiveLimit;
            decision.explanation = u"Already downloading from %1; previews from them wait."_s.arg(peer);
            return decision;
        }
        if (!m_previews.tryTake()) {
            decision.refusal = Refusal::RateLimited;
            decision.retryAfter = m_previews.timeUntilAvailable();
            decision.explanation = u"Previews are paced; next one in %1."_s.arg(seconds(decision.retryAfter));
            return decision;
        }
    }
    Decision allowed =
        allow(Operation::QueueDownload, request.preview ? u"Preview allowed."_s : u"Download allowed."_s);
    if (today.files + 1 >= limits::kPeerDailyWarnFiles || today.bytes >= limits::kPeerDailyWarnBytes) {
        allowed.warning = u"That's %1 files from %2 today; Selecta stops at %3."_s.arg(today.files + 1)
                              .arg(peer)
                              .arg(limits::kPeerDailyStopFiles);
    }
    return allowed;
}

Decision Governor::decideLogin(const Request &request)
{
    if (!addressAllowed(request.server, request.liveEnabledByUser)) {
        Decision refused;
        refused.refusal = Refusal::LiveServerRefused;
        refused.explanation =
            liveServerCompiledIn()
                ? u"Connecting to the live Soulseek network is turned off."_s
                : u"This build can only connect to local test servers, not to %1."_s.arg(request.server.toString());
        return refused;
    }
    Decision decision = m_reconnect.mayLogin();
    if (decision.refusal != Refusal::None) {
        return decision;
    }
    m_reconnect.attemptStarted();
    return allow(Operation::Login, u"Login allowed."_s);
}

void Governor::setPaused(bool paused)
{
    if (m_paused == paused) {
        return;
    }
    m_paused = paused;
    Q_EMIT pausedChanged(paused);
}

void Governor::setExcludedPhrases(const QStringList &phrases)
{
    m_excludedPhrases = phrases;
}

void Governor::serverEvent(ReconnectPolicy::Event event)
{
    m_reconnect.handle(event);
    if (event == ReconnectPolicy::Event::BanNotice) {
        setPaused(true);
    }
}

void Governor::downloadQueued(const QString &peer)
{
    m_ledger.queued(peer);
}

void Governor::downloadStarted(const QString &peer)
{
    m_ledger.started(peer);
}

void Governor::downloadFinished(const QString &peer, qint64 bytes, bool completed)
{
    m_ledger.finished(peer);
    if (completed) {
        const auto counted = m_ledger.recordCompleted(peer, bytes);
        Q_EMIT peerDayChanged(peer, counted.day, counted.totals.files, counted.totals.bytes);
    }
}

void Governor::downloadWithdrawn(const QString &peer)
{
    m_ledger.withdrawn(peer);
}

void Governor::peerDenied(const QString &peer, const QString &reason)
{
    if (const auto backoff = m_ledger.denied(peer, reason)) {
        Q_EMIT peerBackoffStarted(peer, backoff->count(), reason);
    }
}

bool Governor::downloadSlotFree() const
{
    return m_ledger.activeTotal() < limits::kActiveDownloads;
}

void Governor::restorePeerDay(const QString &peer, QDate day, PeerDay totals)
{
    m_ledger.restoreDay(peer, day, totals);
}

void Governor::restorePeerBackoff(const QString &peer, Duration remaining)
{
    m_ledger.restoreBackoff(peer, remaining);
}

int Governor::searchesAvailable() const
{
    return m_searches.available();
}

Duration Governor::nextSearchIn() const
{
    return m_searches.timeUntilAvailable();
}

QString refusalName(Refusal refusal)
{
    switch (refusal) {
    case Refusal::None:
        return u"none"_s;
    case Refusal::NetworkPaused:
        return u"network-paused"_s;
    case Refusal::RateLimited:
        return u"rate-limited"_s;
    case Refusal::SameQueryRecently:
        return u"same-query-recently"_s;
    case Refusal::QueryTooShort:
        return u"query-too-short"_s;
    case Refusal::QueryTooLong:
        return u"query-too-long"_s;
    case Refusal::QueryExcludedPhrase:
        return u"query-excluded-phrase"_s;
    case Refusal::PeerActiveLimit:
        return u"peer-active-limit"_s;
    case Refusal::PeerQueueLimit:
        return u"peer-queue-limit"_s;
    case Refusal::PeerDailyLimit:
        return u"peer-daily-limit"_s;
    case Refusal::PeerWeeklyLimit:
        return u"peer-weekly-limit"_s;
    case Refusal::PeerBackoff:
        return u"peer-backoff"_s;
    case Refusal::PeerTooSoon:
        return u"peer-too-soon"_s;
    case Refusal::GlobalLimit:
        return u"global-limit"_s;
    case Refusal::MessageRepeated:
        return u"message-repeated"_s;
    case Refusal::LoginNeedsUser:
        return u"login-needs-user"_s;
    case Refusal::LoginBackoff:
        return u"login-backoff"_s;
    case Refusal::LiveServerRefused:
        return u"live-server-refused"_s;
    }
    return u"unknown"_s;
}

QString operationName(Operation operation)
{
    switch (operation) {
    case Operation::Login:
        return u"login"_s;
    case Operation::Search:
        return u"search"_s;
    case Operation::PeerLookup:
        return u"peer-lookup"_s;
    case Operation::QueueDownload:
        return u"queue-download"_s;
    case Operation::PlaceInQueue:
        return u"place-in-queue"_s;
    case Operation::Browse:
        return u"browse"_s;
    case Operation::UserInfo:
        return u"user-info"_s;
    case Operation::FolderContents:
        return u"folder-contents"_s;
    case Operation::PrivateMessage:
        return u"private-message"_s;
    case Operation::RoomMessage:
        return u"room-message"_s;
    case Operation::SpotifyRequest:
        return u"spotify"_s;
    case Operation::MusicBrainzRequest:
        return u"musicbrainz"_s;
    case Operation::CoverArtRequest:
        return u"cover-art"_s;
    case Operation::DeezerRequest:
        return u"deezer"_s;
    case Operation::YtDlpRun:
        return u"yt-dlp"_s;
    }
    return u"unknown"_s;
}

} // namespace selecta::governor
