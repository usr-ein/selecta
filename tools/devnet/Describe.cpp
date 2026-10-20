// SPDX-License-Identifier: GPL-3.0-or-later
#include "Describe.h"

#include <QHostAddress>
#include <QtEndian>

#include <optional>
#include <variant>

using namespace Qt::StringLiterals;

namespace selecta::devnet {
namespace {

using slsk::ConnectionType;
using slsk::Decoded;
using slsk::DecodeStatus;
using slsk::RemotePath;
using slsk::SharedDirectory;
using slsk::TransferDirection;
namespace fromserver = slsk::fromserver;
namespace peer = slsk::peer;
namespace toserver = slsk::toserver;

QString quoted(const QString &text)
{
    return u'"' + text + u'"';
}

QString quoted(const RemotePath &path)
{
    return quoted(path.display());
}

QString typeName(ConnectionType type)
{
    return QString::fromLatin1(slsk::connectionTypeCode(type));
}

QString address(quint32 ip, quint32 port)
{
    return QHostAddress(ip).toString() + u':' + QString::number(port);
}

int fileCount(const QList<SharedDirectory> &directories)
{
    int files = 0;
    for (const SharedDirectory &directory : directories) {
        files += static_cast<int>(directory.files.size());
    }
    return files;
}

// The fields worth a log line, per message; the name alone for the rest.
struct Describer
{
    // --- client → server ---
    QString operator()(const toserver::Login &m) const { return u"Login user=%1"_s.arg(m.username); }
    QString operator()(const toserver::SetWaitPort &m) const { return u"SetWaitPort port=%1"_s.arg(m.port); }
    QString operator()(const toserver::GetPeerAddress &m) const { return u"GetPeerAddress user=%1"_s.arg(m.username); }
    QString operator()(const toserver::ConnectToPeer &m) const
    {
        return u"ConnectToPeer token=%1 user=%2 type=%3"_s.arg(m.token).arg(m.username, typeName(m.type));
    }
    QString operator()(const toserver::FileSearch &m) const
    {
        return u"FileSearch token=%1 query=%2"_s.arg(m.token).arg(quoted(m.query));
    }
    QString operator()(const toserver::GetUserStats &m) const { return u"GetUserStats user=%1"_s.arg(m.username); }

    // --- server → client ---
    QString operator()(const fromserver::LoginReply &m) const
    {
        return m.success ? u"LoginReply ok"_s : u"LoginReply refused reason=%1"_s.arg(m.reason);
    }
    QString operator()(const fromserver::PeerAddress &m) const
    {
        return u"PeerAddress user=%1 address=%2"_s.arg(m.username, address(m.ip, m.port));
    }
    QString operator()(const fromserver::ConnectToPeerRequest &m) const
    {
        return u"ConnectToPeer user=%1 type=%2 address=%3 token=%4"_s
            .arg(m.username, typeName(m.type), address(m.ip, m.port))
            .arg(m.token);
    }
    QString operator()(const fromserver::SearchRequest &m) const
    {
        return u"SearchRequest user=%1 token=%2 query=%3"_s.arg(m.username).arg(m.token).arg(quoted(m.query));
    }

    // --- peer ↔ peer ---
    QString operator()(const peer::FileSearchResponse &m) const
    {
        return u"FileSearchResponse token=%1 results=%2 slotFree=%3 speed=%4 queue=%5"_s.arg(m.token)
            .arg(m.results.size())
            .arg(m.slotFree ? u"yes"_s : u"no"_s)
            .arg(m.averageSpeed)
            .arg(m.queueLength);
    }
    QString operator()(const peer::SharedFileListResponse &m) const
    {
        return u"SharedFileListResponse folders=%1 files=%2"_s.arg(m.list.directories.size())
            .arg(fileCount(m.list.directories));
    }
    QString operator()(const peer::UserInfoResponse &m) const
    {
        return u"UserInfoResponse slots=%1 queue=%2 slotsFree=%3"_s.arg(m.totalUploads)
            .arg(m.queueSize)
            .arg(m.slotsFree ? u"yes"_s : u"no"_s);
    }
    QString operator()(const peer::FolderContentsRequest &m) const
    {
        return u"FolderContentsRequest token=%1 folder=%2"_s.arg(m.token).arg(quoted(m.folder));
    }
    QString operator()(const peer::FolderContentsResponse &m) const
    {
        return u"FolderContentsResponse token=%1 folder=%2 files=%3"_s.arg(m.token)
            .arg(quoted(m.folder))
            .arg(fileCount(m.directories));
    }
    QString operator()(const peer::TransferRequest &m) const
    {
        const QString size = m.size ? QString::number(*m.size) : u"-"_s;
        return u"TransferRequest %1 token=%2 path=%3 size=%4"_s
            .arg(m.direction == TransferDirection::Upload ? u"upload"_s : u"download"_s)
            .arg(m.token)
            .arg(quoted(m.path), size);
    }
    QString operator()(const peer::TransferResponse &m) const
    {
        return m.allowed ? u"TransferResponse token=%1 allowed"_s.arg(m.token)
                         : u"TransferResponse token=%1 refused reason=%2"_s.arg(m.token).arg(quoted(m.reason));
    }
    QString operator()(const peer::QueueUpload &m) const { return u"QueueUpload path=%1"_s.arg(quoted(m.path)); }
    QString operator()(const peer::PlaceInQueueRequest &m) const
    {
        return u"PlaceInQueueRequest path=%1"_s.arg(quoted(m.path));
    }
    QString operator()(const peer::PlaceInQueueResponse &m) const
    {
        return u"PlaceInQueueResponse path=%1 place=%2"_s.arg(quoted(m.path)).arg(m.place);
    }
    QString operator()(const peer::UploadFailed &m) const { return u"UploadFailed path=%1"_s.arg(quoted(m.path)); }
    QString operator()(const peer::UploadDenied &m) const
    {
        return u"UploadDenied path=%1 reason=%2"_s.arg(quoted(m.path), quoted(m.reason));
    }

    template <typename M>
    QString operator()(const M & /*m*/) const
    {
        return QString::fromLatin1(M::kName);
    }
};

// A frame's code and payload, when it holds a whole one.
std::optional<std::pair<quint32, QByteArrayView>> split(QByteArrayView frame)
{
    if (frame.size() < 8) {
        return std::nullopt;
    }
    const auto length = qFromLittleEndian<quint32>(frame.data());
    if (length < 4 || static_cast<qsizetype>(length) > frame.size() - 4) {
        return std::nullopt;
    }
    return std::make_pair(qFromLittleEndian<quint32>(frame.data() + 4), frame.sliced(8, length - 4));
}

template <typename Message>
QString describeDecoded(const Decoded<Message> &decoded, quint32 code, qsizetype payloadSize)
{
    if (decoded.status == slsk::DecodeStatus::Decoded && decoded.message) {
        return std::visit(Describer{}, *decoded.message);
    }
    return u"code %1 (%2 bytes, %3)"_s.arg(code)
        .arg(payloadSize)
        .arg(decoded.status == DecodeStatus::Unknown ? u"unknown"_s : u"malformed"_s);
}

} // namespace

QString describe(const slsk::ToServerMessage &message)
{
    return std::visit(Describer{}, message);
}

QString describe(const slsk::FromServerMessage &message)
{
    return std::visit(Describer{}, message);
}

QString describe(const slsk::PeerMessage &message)
{
    return std::visit(Describer{}, message);
}

QString describeServerFrame(QByteArrayView frame)
{
    const auto parts = split(frame);
    if (!parts) {
        return u"(incomplete frame, %1 bytes)"_s.arg(frame.size());
    }
    return describeDecoded(slsk::decodeFromServer(parts->first, parts->second), parts->first, parts->second.size());
}

QString describePeerFrame(QByteArrayView frame)
{
    const auto parts = split(frame);
    if (!parts) {
        return u"(incomplete frame, %1 bytes)"_s.arg(frame.size());
    }
    return describeDecoded(slsk::decodePeer(parts->first, parts->second), parts->first, parts->second.size());
}

} // namespace selecta::devnet
