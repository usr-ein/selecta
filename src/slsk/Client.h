// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"
#include "slsk/DistributedNode.h"
#include "slsk/PeerNetwork.h"
#include "slsk/ServerSession.h"
#include "slsk/TokenGenerator.h"
#include "slsk/TransferManager.h"

#include <QObject>
#include <QString>

namespace selecta::slsk {

class PermitSource;

// A search that reached us and that core/ may answer (SHARE-2), from either route: the server (a user or room search,
// code 26; Soulfind sends every search this way) or the distributed network.
struct IncomingSearch
{
    enum class Route {
        Server,
        Distributed,
    };

    QString username; // the searcher (may be ourselves: Soulfind echoes our own searches)
    quint32 token = 0;
    QString query;
    Route route = Route::Server;

    friend bool operator==(const IncomingSearch &, const IncomingSearch &) = default;
};

// The whole protocol stack, wired together: one ServerSession, PeerNetwork, TransferManager and DistributedNode.
// core/ uses the components through the accessors; the Client adds search bookkeeping (tokens and which ones still
// accept results) and merges incoming searches from both routes.
//
// Threading: lives on the engine thread and is used only from it. Ownership: owns the four components (QObject
// children). Not owned: the PermitSource, which must outlive the Client.
class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(PermitSource &permits, QObject *parent = nullptr);
    ~Client() override;
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    ServerSession &server() { return *m_server; }
    PeerNetwork &peers() { return *m_peers; }
    TransferManager &transfers() { return *m_transfers; }
    DistributedNode &distributed() { return *m_distributed; }

    // Starts a search and opens its token for results; returns the token, or 0 (permit dropped) when not logged in.
    // Results arrive as PeerNetwork::searchResponseReceived() until stopSearch().
    quint32 search(const QString &query, governor::Permit search);
    quint32 searchUser(const QString &username, const QString &query, governor::Permit search);
    quint32 searchWishlist(const QString &query, governor::Permit search);
    // Stops accepting results for `token` (SRCH-16: core/ decides when collecting ends).
    void stopSearch(quint32 token);

    // Registers the metatypes of every value type slsk signals carry, for queued connections across threads. Safe to
    // call more than once and from any thread.
    static void registerMetaTypes();

Q_SIGNALS:
    void searchRequestReceived(const selecta::slsk::IncomingSearch &search);

private:
    quint32 nextSearchToken();

    ServerSession *m_server;
    PeerNetwork *m_peers;
    TransferManager *m_transfers;
    DistributedNode *m_distributed;
    TokenGenerator m_searchTokens;
};

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::IncomingSearch)
