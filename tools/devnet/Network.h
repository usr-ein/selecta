// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Behaviour.h"
#include "Library.h"
#include "slsk/PeerMessages.h"
#include "slsk/ServerMessages.h"

#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>

#include <chrono>
#include <memory>
#include <vector>

namespace selecta::slsk::testing {
class FakePeer;
class FakeServer;
} // namespace selecta::slsk::testing

namespace selecta::devnet {

struct NetworkOptions
{
    // The server's port on 127.0.0.1; 0 picks a free one.
    quint16 port = 2242;
    // With a password: the only account that may log in. Empty: any name and password.
    QString user;
    QString password;
    // Bytes per second per upload, before each peer's own factor (25 to 100 %); 0: no cap.
    qint64 uploadRate = qint64{1024} * 1024;
    // Longest a queued download waits for its upload to start (a random wait up to this).
    std::chrono::milliseconds queueWait{2000};
    // Share of queue requests refused with a realistic reason, in percent.
    int denyPercent = 0;
    // Longest a peer takes to answer a search (a random delay up to this).
    std::chrono::milliseconds searchDelay{1200};
    // Results per peer and search.
    int maxResults = 100;
    // Each peer's speed, free slot and timings, and the denials.
    quint32 seed = 1;
    // Peers that queue for long or refuse everything, by name.
    Behaviours behaviours;
};

// The files of `shares` a search for `query` finds, the way sharing clients match: every word of the query must be a
// word of the file's full virtual path (words are split on anything but letters and digits and compared
// case-insensitively), a word starting with '-' must not be, and a word starting with '*' may be part of a longer
// word. At most `limit` entries, in share order, each named by its full virtual path as search results are.
QList<slsk::FileEntry> matchSearch(const slsk::SharedFileList &shares, const QString &query, int limit);

// A local Soulseek network for manual runs: a FakeServer and one FakePeer per LibraryPeer, all on 127.0.0.1 and
// nowhere else, behaving like a small slice of the real network. Peers answer searches after a short delay with
// what they share (and their free slot, speed and queue), answer browses, user info and folder requests, queue
// download requests, let them wait a little (answering place-in-queue), refuse some with the reasons real clients
// send, and upload the real local files at their own speed. Every message and transfer is logged.
//
// It never resolves a host name and never connects anywhere but 127.0.0.1.
//
// Threading: lives on the thread that runs its event loop. Ownership: owns the server and the peers.
class DevNetwork : public QObject
{
    Q_OBJECT

public:
    explicit DevNetwork(NetworkOptions options, QObject *parent = nullptr);
    ~DevNetwork() override;
    DevNetwork(const DevNetwork &) = delete;
    DevNetwork &operator=(const DevNetwork &) = delete;

    bool start(QString *error);
    // A peer sharing `peer`'s files. Call after start().
    bool addPeer(const LibraryPeer &library, QString *error);

    quint16 port() const;
    QStringList peerNames() const;
    // Sets whether every message is logged (the default) or only logins and transfers.
    void setVerbose(bool verbose) { m_verbose = verbose; }

Q_SIGNALS:
    void logLine(const QString &line);

private:
    struct Peer;
    void answerSearch(const Peer &peer, const slsk::fromserver::SearchRequest &request);
    void answerMessage(const QString &from, const slsk::toserver::MessageUser &message);
    void answerRoomLine(const QString &from, const slsk::toserver::SayChatroom &line);
    void onPeerMessage(const Peer &peer, const QString &from, const slsk::PeerMessage &message);
    void connectLogging(const Peer &peer);
    void log(const QString &line);
    void trace(const QString &line);

    NetworkOptions m_options;
    std::unique_ptr<slsk::testing::FakeServer> m_server;
    std::vector<std::unique_ptr<Peer>> m_peers;
    QStringList m_roomPeers; // the peers in the network's room
    QRandomGenerator m_random;
    bool m_verbose = true;
};

} // namespace selecta::devnet
