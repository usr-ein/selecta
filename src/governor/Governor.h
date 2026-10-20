// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "base/Clock.h"
#include "governor/Decision.h"
#include "governor/PeerLedger.h"
#include "governor/ReconnectPolicy.h"
#include "governor/TokenBucket.h"

#include <QDate>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QStringList>

#include <functional>
#include <map>

namespace selecta::governor {

// Where a search comes from. Get tracks leaves part of the budget for searches typed by hand.
enum class SearchSource { Manual, GetTracks, Wishlist, Refresh };

// What an operation is about. Fields an operation doesn't use are ignored.
struct Request
{
    QString peer;  // the Soulseek user, for per-peer operations
    QString query; // Search: as typed; the Governor cleans it (Decision::cleanedQuery)
    SearchSource source = SearchSource::Manual;
    QString recipient;              // PrivateMessage: the user; RoomMessage: the room
    QString text;                   // PrivateMessage, RoomMessage
    bool preview = false;           // QueueDownload: a preview rather than a download
    QHostAddress server;            // Login: the resolved server address
    bool liveEnabledByUser = false; // Login: the user's run-time choice (PRD SAFE-3)
};

// The network governor (ADR-006): every budgeted outbound operation asks here first and gets a single-use Permit or a
// refusal with its reason. Rates are token buckets; the numbers are in Limits.h. Every decision is emitted for the
// activity log (PRD SAFE-4).
//
// Threading: lives on the engine thread and is used only there. Not thread-safe.
class Governor : public QObject
{
    Q_OBJECT

public:
    struct Options
    {
        std::function<QDate()> today;     // local calendar day; default QDate::currentDate
        std::function<double()> random01; // uniform [0, 1); default QRandomGenerator
    };

    explicit Governor(const base::Clock &clock, Options options = {}, QObject *parent = nullptr);
    ~Governor() override;

    // Asks for one operation. Allowed: Decision::permit is set; for searches Decision::cleanedQuery is what to send.
    Decision request(Operation operation, const Request &request = {});

    // --- state the limits depend on ---
    // "Pause network activity" (SAFE-5): refuses new Soulseek operations; metadata services aren't affected.
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    // Phrases the server says it excludes (code 160). Searches containing one are refused.
    void setExcludedPhrases(const QStringList &phrases);
    const QStringList &excludedPhrases() const { return m_excludedPhrases; }
    // Server session events. A ban notice also pauses the network.
    void serverEvent(ReconnectPolicy::Event event);

    // --- transfer bookkeeping, from the transfer service ---
    void downloadQueued(const QString &peer);
    void downloadStarted(const QString &peer);
    // An active download ended. Completed ones count toward the peer's daily and weekly totals (previews too).
    void downloadFinished(const QString &peer, qint64 bytes, bool completed);
    // A queued download left the queue without starting (cancelled, denied, withdrawn).
    void downloadWithdrawn(const QString &peer);
    void peerDenied(const QString &peer, const QString &reason);
    // Global cap on simultaneous downloads; the transfer service checks it before accepting a transfer.
    bool downloadSlotFree() const;

    // --- restoring persisted state at start-up ---
    void restorePeerDay(const QString &peer, QDate day, PeerDay totals);
    void restorePeerBackoff(const QString &peer, base::Clock::Duration remaining);

    // --- for the UI ---
    int searchesAvailable() const;
    // Until a search may go: zero while one is available.
    base::Clock::Duration nextSearchIn() const;
    // Until the budget grows by one search: zero only when it is full. (nextSearchIn() is zero as long as one search
    // is left, so it can't time a refill.)
    base::Clock::Duration nextSearchRefillIn() const;
    PeerDay peerToday(const QString &peer) const { return m_ledger.today(peer); }
    int peerQueued(const QString &peer) const { return m_ledger.queuedAt(peer); }
    int peerActive(const QString &peer) const { return m_ledger.activeAt(peer); }
    // How long Selecta still holds off this peer after a denial (FAIR-4); nullopt when it doesn't.
    std::optional<base::Clock::Duration> peerBackoff(const QString &peer) const
    {
        return m_ledger.backoffRemaining(peer);
    }
    bool loginNeedsUser() const { return m_reconnect.needsUser(); }

Q_SIGNALS:
    // Every decision, allowed or refused, for the activity log.
    void decided(selecta::governor::Operation operation, const QString &subject, selecta::governor::Refusal refusal,
                 const QString &explanation);
    void pausedChanged(bool paused);
    void searchBudgetChanged(int available);
    // State that must survive a restart (GovernorStore saves it): a peer's totals for `day` after a completed
    // download, and a back-off a peer's denial started.
    void peerDayChanged(const QString &peer, QDate day, int files, qint64 bytes);
    void peerBackoffStarted(const QString &peer, qint64 remainingMs, const QString &reason);

private:
    Decision decide(Operation operation, const Request &request);
    Decision decideSearch(const Request &request);
    Decision decideQueueDownload(const Request &request);
    Decision decideLogin(const Request &request);
    Decision decideMessage(Operation operation, const Request &request, TokenBucket &bucket);
    Decision decideTimed(Operation operation, TokenBucket &bucket, const QString &what);
    Decision decidePerSubject(Operation operation, std::map<QString, base::Clock::TimePoint> &last,
                              const QString &subject, base::Clock::Duration quiet, TokenBucket &bucket,
                              const QString &what);
    Decision allow(Operation operation, QString explanation);
    static bool isSoulseekOperation(Operation operation);
    void forget(std::map<QString, base::Clock::TimePoint> &last, base::Clock::Duration olderThan) const;

    const base::Clock &m_clock;
    std::function<QDate()> m_today;
    PeerLedger m_ledger;
    ReconnectPolicy m_reconnect;
    bool m_paused = false;
    QStringList m_excludedPhrases;

    TokenBucket m_searches;
    TokenBucket m_peerLookups;
    TokenBucket m_previews;
    TokenBucket m_browses;
    TokenBucket m_userInfos;
    TokenBucket m_folderContents;
    TokenBucket m_placeInQueue;
    TokenBucket m_privateMessages;
    TokenBucket m_roomMessages;
    TokenBucket m_spotify;
    TokenBucket m_musicBrainz;
    TokenBucket m_coverArt;
    TokenBucket m_deezer;
    TokenBucket m_ytDlp;

    std::map<QString, base::Clock::TimePoint> m_lastQuery;      // query key → sent
    std::map<QString, base::Clock::TimePoint> m_lastBrowse;     // user → browsed
    std::map<QString, base::Clock::TimePoint> m_lastUserInfo;   // user → asked
    std::map<QString, base::Clock::TimePoint> m_lastPlaceQuery; // user → asked
    std::map<QString, base::Clock::TimePoint> m_lastMessage;    // recipient + text → sent
};

} // namespace selecta::governor
