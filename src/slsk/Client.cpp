// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Client.h"

#include "slsk/Logging.h"
#include "slsk/MetaTypes.h"

namespace selecta::slsk {

Client::Client(PermitSource &permits, QObject *parent)
    : QObject(parent)
    , m_server(new ServerSession(this))
    , m_peers(new PeerNetwork(*m_server, permits, this))
    , m_transfers(new TransferManager(*m_peers, *m_server, this))
    , m_distributed(new DistributedNode(*m_server, *m_peers, this))
{
    registerMetaTypes();
    connect(m_server, &ServerSession::searchRequestReceived, this, [this](const fromserver::SearchRequest &request) {
        Q_EMIT searchRequestReceived(
            IncomingSearch{request.username, request.token, request.query, IncomingSearch::Route::Server});
    });
    connect(m_distributed, &DistributedNode::searchRequestReceived, this, [this](const distrib::Search &search) {
        Q_EMIT searchRequestReceived(
            IncomingSearch{search.username, search.token, search.query, IncomingSearch::Route::Distributed});
    });
}

Client::~Client()
{
    // Each component uses the ones made before it, also while being destroyed: undo them in reverse. (QObject would
    // delete the children in the order they were made.)
    delete m_distributed;
    delete m_transfers;
    delete m_peers;
    delete m_server;
}

quint32 Client::nextSearchToken()
{
    return m_searchTokens.next([this](quint32 token) { return m_peers->isSearchOpen(token); });
}

template <typename Send>
quint32 Client::startSearch(const governor::Permit &permit, Send send)
{
    if (!m_server->isLoggedIn() || !permit.isValid() || permit.operation() != governor::Operation::Search) {
        return 0;
    }
    const quint32 token = nextSearchToken();
    m_peers->openSearch(token); // before the request: no result can be missed
    if (!send(token)) {
        m_peers->closeSearch(token);
        return 0;
    }
    return token;
}

quint32 Client::search(const QString &query, governor::Permit search)
{
    return startSearch(search, [&](quint32 token) { return m_server->fileSearch(token, query, std::move(search)); });
}

quint32 Client::searchUser(const QString &username, const QString &query, governor::Permit search)
{
    return startSearch(search,
                       [&](quint32 token) { return m_server->userSearch(username, token, query, std::move(search)); });
}

quint32 Client::searchWishlist(const QString &query, governor::Permit search)
{
    return startSearch(search,
                       [&](quint32 token) { return m_server->wishlistSearch(token, query, std::move(search)); });
}

void Client::stopSearch(quint32 token)
{
    m_peers->closeSearch(token);
}

bool Client::answerSearch(const IncomingSearch &search, const QList<FileEntry> &results, UploadAvailability uploads,
                          PeerNetwork::SendCallback done)
{
    if (results.isEmpty() || search.username.isEmpty() || !m_server->isLoggedIn()) {
        return false;
    }
    peer::FileSearchResponse response;
    response.username = m_server->username();
    response.token = search.token;
    response.results = results;
    response.slotFree = uploads.slotFree;
    response.averageSpeed = uploads.averageSpeed;
    response.queueLength = uploads.queueLength;
    m_peers->sendSearchResponse(search.username, response, std::move(done));
    return true;
}

void Client::registerMetaTypes()
{
    slsk::registerMetaTypes();
}

} // namespace selecta::slsk
