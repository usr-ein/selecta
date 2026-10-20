// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace selecta::core {
struct AlbumDownload;
struct AlbumReport;
struct ChatMessage;
struct GetLine;
struct RoomLine;
struct WishlistItem;
} // namespace selecta::core

namespace selecta::app {

// What clicking a system notification opens: a view (NavModel ids: "messages", "rooms", "inbox", "wishlist", "get")
// and, for some, what to show in it (the conversation's peer, the room).
struct NotificationTarget
{
    QString view;
    QString argument;

    friend bool operator==(const NotificationTarget &, const NotificationTarget &) = default;
};

// One notification for the system's notification centre.
struct SystemNotification
{
    QString id;     // a later notification with the same id replaces this one
    QString thread; // notifications of one thread are grouped (one conversation, one room)
    QString title;
    QString body;
    bool sound = false; // someone is waiting for an answer; finished work stays silent
    NotificationTarget target;

    friend bool operator==(const SystemNotification &, const SystemNotification &) = default;
};

// Where system notifications go: macOS's Notification Center inside the app bundle (createSystemNotificationSink()), a
// list for the debug socket in headless runs (RecordingNotificationSink), a fake in tests. Main thread only; the
// activation handler is called on the main thread too.
class NotificationSink
{
public:
    using ActivationHandler = std::function<void(const NotificationTarget &)>;

    NotificationSink() = default;
    virtual ~NotificationSink() = default;
    NotificationSink(const NotificationSink &) = delete;
    NotificationSink &operator=(const NotificationSink &) = delete;
    NotificationSink(NotificationSink &&) = delete;
    NotificationSink &operator=(NotificationSink &&) = delete;

    // Hands a notification to the system. Never blocks; a refusal or a failure is the sink's to handle (it logs it).
    virtual void post(const SystemNotification &notification) = 0;
    // What the sink is and what it knows of its permission, for the debug socket and the log.
    virtual QString describe() const = 0;

    // Who hears about clicks (the Notifier); an empty handler stops that.
    void setActivationHandler(ActivationHandler handler) { m_handler = std::move(handler); }

protected:
    // The user clicked a notification that asked to open `target`.
    void activate(const NotificationTarget &target) const
    {
        if (m_handler) {
            m_handler(target);
        }
    }

private:
    ActivationHandler m_handler;
};

// macOS's Notification Center (UNUserNotificationCenter), when Selecta runs from its app bundle; nullptr elsewhere
// (tests, command-line tools, other systems), where the framework would throw: the window's own notifications are then
// all there is. Create it on the main thread before the event loop starts, so a click is never missed.
std::unique_ptr<NotificationSink> createSystemNotificationSink();
// Whether this process runs from an app bundle with a bundle identifier (what macOS's notifications need).
bool runsInAppBundle();

// Keeps what is posted instead of showing it: headless runs (the debug socket lists and clicks them), never the
// screen. Holds the latest kKept notifications. Main thread only.
class RecordingNotificationSink final : public NotificationSink
{
public:
    static constexpr std::size_t kKept = 50;

    void post(const SystemNotification &notification) override;
    QString describe() const override;

    // Oldest first.
    QList<SystemNotification> posted() const { return {m_posted.cbegin(), m_posted.cend()}; }
    // Acts as a click on the notification posted with this id. Returns false if none is kept.
    bool click(const QString &id) const;

private:
    std::deque<SystemNotification> m_posted;
};

// A Get tracks batch that finished: every line the user asked to get has arrived, failed for good or been cancelled.
struct FinishedBatch
{
    int arrived = 0;
    int failed = 0;
    QString track; // "Artist – Title" of the one track it settled, when it settled one
};

// Follows the Get tracks downloads as batches (PRD UI-6): a batch is every line the user sends on its way (Get, K, a
// retry) until none is on its way any more; lines sent while others are still going join the same batch. A line that
// arrives or fails for good counts; a cancelled, interrupted or removed one just leaves. Plain logic: no thread of its
// own, use it from one thread.
class GetTracksBatch
{
public:
    struct Step
    {
        int arrived = 0; // lines that have just arrived
        std::optional<FinishedBatch> finished;
    };

    // The whole list, after lines came or went (or at start-up, with downloads that resume).
    Step reset(const QList<core::GetLine> &lines);
    // One line changed (progress updates included: they cost a lookup).
    Step update(const core::GetLine &line);
    // Lines on their way now.
    int pending() const { return static_cast<int>(m_pending.size()); }

private:
    void follow(const core::GetLine &line, Step &step);
    std::optional<FinishedBatch> finishIfDone();

    QSet<qint64> m_pending; // lines on their way, in this batch
    FinishedBatch m_batch;  // what this batch has settled so far
    QString m_lastSettled;  // the name of the last line it settled
};

// Decides what reaches the system's notifications and the Dock badge (PRD UI-6, NET-7), apart from the window's own
// notifications (Toasts), which MainWindow shows whatever happens.
//
// A system notification goes out only while the user is away from Selecta (the AwayCheck says so: its window isn't
// the active one), only with "Show notifications" on, and only for: a private message, someone naming you in a room, a
// Get tracks batch that finished, an album that arrived (complete or not) or failed, and the Wishlist finding new
// files. Messages and mentions coalesce per conversation and per room until the user is back: one notification, its
// count and the latest line. The Dock badge counts unread messages plus what arrived while the user was away (albums
// and Get tracks lines); the arrivals are cleared once the user is back (userReturned()).
//
// Main thread only. The sink, if any, must outlive the Notifier; a null sink means no system notifications here.
class Notifier : public QObject
{
    Q_OBJECT

public:
    using AwayCheck = std::function<bool()>;

    Notifier(NotificationSink *sink, AwayCheck away, QObject *parent = nullptr);
    ~Notifier() override;
    Notifier(const Notifier &) = delete;
    Notifier &operator=(const Notifier &) = delete;
    Notifier(Notifier &&) = delete;
    Notifier &operator=(Notifier &&) = delete;

    // Settings → "Show notifications" (on by default). Off, nothing reaches the system; the badge still counts.
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    NotificationSink *sink() const { return m_sink; }
    bool isAway() const;

    // What happened (EngineHost's signals of the same names).
    void messageAdded(const core::ChatMessage &message);
    void roomLineAdded(const QString &room, const core::RoomLine &line);
    void albumReported(const core::AlbumReport &report);
    void albumChanged(const core::AlbumDownload &album);
    void wishlistFound(const core::WishlistItem &item, int newFiles);
    void getLinesReset(const QList<core::GetLine> &lines);
    void getLineChanged(const core::GetLine &line);

    // The Dock badge (UI-6): unread messages plus arrivals while away.
    void setUnreadMessages(int unread);
    int badge() const { return m_unread + m_arrivals; }
    int arrivalsWhileAway() const { return m_arrivals; }
    // The user is back in Selecta (its window became active): the arrivals have been seen, and what comes next starts
    // new notifications.
    void userReturned();

    // The words of a finished batch's notification, for the window's notification too.
    static SystemNotification batchNotification(const FinishedBatch &batch);

    // The longest title and body handed to the system: names and lines come from the network.
    static constexpr qsizetype kMaxTitle = 120;
    static constexpr qsizetype kMaxBody = 300;

Q_SIGNALS:
    void badgeChanged(int count);
    // A Get tracks batch finished (whether the user is away or not).
    void getTracksFinished(const selecta::app::FinishedBatch &batch);
    // The user clicked a system notification: Selecta should come forward and show this.
    void activated(const selecta::app::NotificationTarget &target);

private:
    void post(SystemNotification notification);
    void countArrivals(int arrived);
    void finishBatch(const GetTracksBatch::Step &step);

    NotificationSink *m_sink;
    AwayCheck m_away;
    qint64 m_run = 0; // when this run started: ids of one-off news don't replace an earlier run's
    bool m_enabled = true;
    int m_unread = 0;
    int m_arrivals = 0;
    quint64 m_serial = 0;
    GetTracksBatch m_batch;
    QHash<QString, int> m_messagesAway; // by peer: messages since the user left
    QHash<QString, int> m_mentionsAway; // by room: mentions since the user left
    QHash<qint64, int> m_wishlistAway;  // by wishlist entry: files found since the user left
    QSet<qint64> m_albumsArrived;       // albums told of, so each once
    QSet<qint64> m_albumsFailed;
};

} // namespace selecta::app
