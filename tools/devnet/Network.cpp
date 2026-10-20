// SPDX-License-Identifier: GPL-3.0-or-later
#include "Network.h"

#include "Describe.h"
#include "fixtures/FakePeer.h"
#include "fixtures/FakeServer.h"
#include "slsk/Frame.h"

#include <QTimer>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::devnet {
namespace {

using slsk::testing::FakePeer;
using slsk::testing::FakeServer;

// What real uploaders answer a queue request they won't serve (references/soulseek-protocol.md §6.5).
constexpr std::array kRefusals = {u"Too many files", u"Too many megabytes", u"Pending shutdown.", u"Banned"};
constexpr qint64 kUncappedSpeed = qint64{10} * 1024 * 1024; // what a peer without a cap advertises
constexpr qint64 kMinSearchDelayMs = 50;

// The words of `text`, case-folded: runs of letters and digits.
QStringList wordsOf(const QString &text)
{
    QStringList words;
    QString current;
    const QString folded = text.toCaseFolded();
    for (const QChar c : folded) {
        if (c.isLetterOrNumber()) {
            current += c;
        } else if (!current.isEmpty()) {
            words.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty()) {
        words.append(current);
    }
    return words;
}

struct Query
{
    QStringList include; // whole words that must appear
    QStringList partial; // must appear inside some word
    QStringList exclude; // whole words that must not appear

    bool matches(const QStringList &pathWords) const
    {
        const auto has = [&](const QString &word) { return pathWords.contains(word); };
        const auto inside = [&](const QString &part) {
            return std::ranges::any_of(pathWords, [&](const QString &word) { return word.contains(part); });
        };
        return std::ranges::all_of(include, has) && std::ranges::all_of(partial, inside) &&
               std::ranges::none_of(exclude, has);
    }
};

Query parseQuery(const QString &text)
{
    Query query;
    const QStringList terms = text.split(u' ', Qt::SkipEmptyParts);
    for (const QString &term : terms) {
        if (term.startsWith(u'-')) {
            query.exclude += wordsOf(term.mid(1));
        } else if (term.startsWith(u'*')) {
            query.partial += wordsOf(term.mid(1));
        } else {
            query.include += wordsOf(term);
        }
    }
    return query;
}

QString megabytes(quint64 bytes)
{
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1) + u" MiB"_s;
}

} // namespace

QList<slsk::FileEntry> matchSearch(const slsk::SharedFileList &shares, const QString &query, int limit)
{
    const Query parsed = parseQuery(query);
    QList<slsk::FileEntry> found;
    if (parsed.include.isEmpty() && parsed.partial.isEmpty()) {
        return found; // only exclusions, or nothing: real clients answer nothing
    }
    for (const slsk::SharedDirectory &directory : shares.directories) {
        for (const slsk::FileEntry &file : directory.files) {
            const QString path = directory.path.display() + u'\\' + file.name.display();
            if (!parsed.matches(wordsOf(path))) {
                continue;
            }
            slsk::FileEntry result = file;
            result.name = slsk::RemotePath::fromText(path);
            found.append(result);
            if (found.size() >= limit) {
                return found;
            }
        }
    }
    return found;
}

struct DevNetwork::Peer
{
    LibraryPeer library;
    PeerBehaviour behaviour;
    std::unique_ptr<FakePeer> fake;
    bool slotFree = true;
    qint64 rate = 0; // bytes per second per upload; 0: no cap
};

DevNetwork::DevNetwork(NetworkOptions options, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_server(std::make_unique<FakeServer>())
    , m_random(m_options.seed)
{
    if (!m_options.user.isEmpty()) {
        m_server->setAccount(m_options.user, m_options.password);
    }
    m_server->setGreeting(u"selecta-devnet: a local test network on 127.0.0.1"_s);
    connect(m_server.get(), &FakeServer::clientLoggedIn, this,
            [this](const QString &user) { log(u"%1 logged in"_s.arg(user)); });
    connect(m_server.get(), &FakeServer::clientDisconnected, this,
            [this](const QString &user) { log(u"%1 disconnected"_s.arg(user)); });
    connect(m_server.get(), &FakeServer::messageReceived, this,
            [this](const QString &user, const slsk::ToServerMessage &message) {
                trace(u"server <- %1  %2"_s.arg(user, describe(message)));
            });
    connect(m_server.get(), &FakeServer::frameSent, this, [this](const QString &user, const QByteArray &frame) {
        trace(u"server -> %1  %2"_s.arg(user.isEmpty() ? u"(logging in)"_s : user, describeServerFrame(frame)));
    });
}

DevNetwork::~DevNetwork()
{
    m_peers.clear(); // before the server: each peer unregisters from it
}

bool DevNetwork::start(QString *error)
{
    if (!m_server->start(m_options.port)) {
        *error = u"can't listen on 127.0.0.1:%1: %2"_s.arg(m_options.port).arg(m_server->errorString());
        return false;
    }
    return true;
}

bool DevNetwork::addPeer(const LibraryPeer &library, QString *error)
{
    auto peer = std::make_unique<Peer>();
    peer->library = library;
    peer->behaviour = m_options.behaviours.value(library.username);
    // A peer that keeps everyone queued has no free slot to advertise.
    peer->slotFree = !peer->behaviour.queue && m_random.bounded(100) < 85;
    if (m_options.uploadRate > 0) {
        const double factor = 0.25 + (m_random.bounded(76) / 100.0); // 25 to 100 % of the base speed
        peer->rate = std::max<qint64>(1024, static_cast<qint64>(static_cast<double>(m_options.uploadRate) * factor));
    }
    auto fake = std::make_unique<FakePeer>(library.username, m_server.get());
    fake->shares = library.shares;
    fake->queueBehaviour = FakePeer::QueueBehaviour::Hold; // onPeerMessage() decides
    fake->uploadRate = peer->rate;
    fake->userInfo = slsk::peer::UserInfoResponse{
        u"selecta-devnet peer: local files, loopback only"_s, std::nullopt, 2, 0, peer->slotFree, std::nullopt};
    for (auto it = library.files.constBegin(); it != library.files.constEnd(); ++it) {
        fake->files.insert(it.key(), FakePeer::Upload{{}, -1, false, it.value()});
    }
    const Peer *self = peer.get(); // owned by m_peers from here on: the address stays valid
    fake->onSearch = [this, self](const slsk::fromserver::SearchRequest &request) { answerSearch(*self, request); };
    fake->onMessage = [this, self](quint64 /*connection*/, const QString &from, const slsk::PeerMessage &message) {
        onPeerMessage(*self, from, message);
    };
    if (!fake->start(true)) {
        *error = u"peer %1 can't listen on 127.0.0.1"_s.arg(library.username);
        return false;
    }
    peer->fake = std::move(fake);
    connectLogging(*peer);
    const qint64 advertised = peer->rate > 0 ? peer->rate : kUncappedSpeed;
    m_server->setUserStats(library.username, slsk::UserStats{static_cast<quint32>(advertised), 0, 0,
                                                             static_cast<quint32>(library.fileCount()),
                                                             static_cast<quint32>(library.shares.directories.size())});
    const QString speed = peer->rate > 0 ? u"%1 KiB/s"_s.arg(peer->rate / 1024) : u"full speed"_s;
    const QString habit = peer->behaviour.isDefault() ? QString() : u"; "_s + describeBehaviour(peer->behaviour);
    log(u"%1 shares %2 files (%3) at %4%5"_s.arg(library.username)
            .arg(library.fileCount())
            .arg(megabytes(library.totalBytes), speed, habit));
    m_peers.push_back(std::move(peer));
    return true;
}

quint16 DevNetwork::port() const
{
    return m_server->port();
}

QStringList DevNetwork::peerNames() const
{
    QStringList names;
    for (const auto &peer : m_peers) {
        names.append(peer->library.username);
    }
    return names;
}

void DevNetwork::answerSearch(const Peer &peer, const slsk::fromserver::SearchRequest &request)
{
    QList<slsk::FileEntry> results = matchSearch(peer.library.shares, request.query, m_options.maxResults);
    if (results.isEmpty()) {
        return;
    }
    slsk::peer::FileSearchResponse response;
    response.username = peer.library.username;
    response.token = request.token;
    response.results = std::move(results);
    response.slotFree = peer.slotFree;
    response.averageSpeed = static_cast<quint32>(peer.rate > 0 ? peer.rate : kUncappedSpeed);
    response.queueLength = static_cast<quint32>(peer.fake->heldCount());
    const qint64 longest = std::max(kMinSearchDelayMs, static_cast<qint64>(m_options.searchDelay.count()));
    const std::chrono::milliseconds delay{m_random.bounded(kMinSearchDelayMs, longest + 1)};
    log(u"%1 has %2 results for %3's search %4, answering in %5 ms"_s.arg(peer.library.username)
            .arg(response.results.size())
            .arg(request.username, u'"' + request.query + u'"')
            .arg(delay.count()));
    FakePeer *fake = peer.fake.get();
    QTimer::singleShot(delay, fake,
                       [fake, user = request.username, response] { fake->sendSearchResponse(user, response); });
}

void DevNetwork::onPeerMessage(const Peer &peer, const QString &from, const slsk::PeerMessage &message)
{
    FakePeer *fake = peer.fake.get();
    if (const auto *queued = std::get_if<slsk::peer::QueueUpload>(&message)) {
        if (!peer.library.files.contains(queued->path.raw)) {
            fake->denyHeld(from, queued->path, u"File not shared."_s);
            return;
        }
        if (!peer.behaviour.refuse.isEmpty()) {
            fake->denyHeld(from, queued->path, peer.behaviour.refuse);
            return;
        }
        if (m_random.bounded(100) < m_options.denyPercent) {
            const auto pick = m_random.bounded(static_cast<quint32>(kRefusals.size()));
            fake->denyHeld(from, queued->path, QString::fromUtf16(kRefusals.at(pick)));
            return;
        }
        std::chrono::milliseconds wait{0};
        if (peer.behaviour.queue) {
            wait = *peer.behaviour.queue;
            log(u"%1 keeps %2's download of %3 queued for %4 s"_s
                    .arg(peer.library.username, from, queued->path.display())
                    .arg(peer.behaviour.queue->count()));
        } else {
            const qint64 longest = std::max<qint64>(0, m_options.queueWait.count());
            wait = std::chrono::milliseconds(m_random.bounded(qint64{0}, longest + 1));
        }
        QTimer::singleShot(wait, fake, [fake, from, path = queued->path] { fake->startUpload(from, path); });
        return;
    }
    if (const auto *asked = std::get_if<slsk::peer::PlaceInQueueRequest>(&message)) {
        if (const int place = fake->heldPosition(from, asked->path); place > 0) {
            fake->sendTo(from, slsk::frame(slsk::peer::PlaceInQueueResponse{asked->path, static_cast<quint32>(place)}));
        }
    }
}

void DevNetwork::connectLogging(const Peer &peer)
{
    const QString name = peer.library.username;
    const FakePeer *fake = peer.fake.get();
    connect(fake, &FakePeer::messageReceived, this,
            [this, name](quint64 /*connection*/, const QString &from, const slsk::PeerMessage &message) {
                trace(u"%1 <- %2  %3"_s.arg(name, from, describe(message)));
            });
    connect(fake, &FakePeer::peerFrameSent, this,
            [this, name](quint64 /*connection*/, const QString &to, const QByteArray &frame) {
                trace(u"%1 -> %2  %3"_s.arg(name, to, describePeerFrame(frame)));
            });
    connect(fake, &FakePeer::uploadStarted, this,
            [this, name](quint64 /*connection*/, const QString &to, const slsk::RemotePath &path, quint64 offset,
                         quint64 size) {
                log(u"%1 -> %2  uploading %3 from byte %4 of %5"_s.arg(name, to, path.display()).arg(offset).arg(size));
            });
    connect(fake, &FakePeer::uploadFinished, this,
            [this, name](quint64 /*connection*/, const QString &to, const slsk::RemotePath &path, quint64 sent,
                         bool complete) {
                log(u"%1 -> %2  upload of %3 %4 (%5 bytes sent)"_s
                        .arg(name, to, path.display(), complete ? u"complete"_s : u"cut off"_s)
                        .arg(sent));
            });
}

void DevNetwork::log(const QString &line)
{
    Q_EMIT logLine(line);
}

void DevNetwork::trace(const QString &line)
{
    if (m_verbose) {
        Q_EMIT logLine(line);
    }
}

} // namespace selecta::devnet
