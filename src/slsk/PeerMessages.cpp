// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/PeerMessages.h"

#include "slsk/Zlib.h"
#include "slsk/internal/CodecHelpers.h"

#include <QtEndian>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::slsk {

using internal::encodeWith;
using internal::parse;
using internal::readOptional;
using internal::readText;

namespace {

constexpr int kCompressionLevel = 4; // Nicotine+'s level: a good size/CPU trade-off for share lists

void writeEntries(Writer &w, const QList<FileEntry> &entries)
{
    w.u32(static_cast<quint32>(entries.size()));
    for (const FileEntry &entry : entries) {
        entry.encode(w);
    }
}

QList<FileEntry> readEntries(Reader &r)
{
    const qsizetype n = r.count(FileEntry::kMinBytes);
    QList<FileEntry> entries;
    entries.reserve(n);
    for (qsizetype i = 0; i < n; ++i) {
        std::optional<FileEntry> entry = FileEntry::decode(r);
        if (!entry) {
            r.fail();
            return {};
        }
        entries.append(std::move(*entry));
    }
    return entries;
}

void writeDirectories(Writer &w, const QList<SharedDirectory> &directories)
{
    w.u32(static_cast<quint32>(directories.size()));
    for (const SharedDirectory &directory : directories) {
        directory.encode(w);
    }
}

QList<SharedDirectory> readDirectories(Reader &r)
{
    const qsizetype n = r.count(SharedDirectory::kMinBytes);
    QList<SharedDirectory> directories;
    directories.reserve(n);
    for (qsizetype i = 0; i < n; ++i) {
        std::optional<SharedDirectory> directory = SharedDirectory::decode(r);
        if (!directory) {
            r.fail();
            return {};
        }
        directories.append(std::move(*directory));
    }
    return directories;
}

// Inflates a zlib payload, then parses it with `fill`.
template <typename T, typename Fill>
std::optional<T> parseCompressed(QByteArrayView payload, qsizetype maxInflated, Fill &&fill)
{
    const zlib::Inflated inflated = zlib::inflate(payload, maxInflated);
    if (!inflated.ok()) {
        return std::nullopt;
    }
    return parse<T>(QByteArrayView(inflated.data), std::forward<Fill>(fill));
}

} // namespace

// --- peer init -------------------------------------------------------------------------------------------------------
namespace peerinit {

QByteArray PierceFireWall::encode() const
{
    return encodeWith([this](Writer &w) { w.u32(token); });
}

std::optional<PierceFireWall> PierceFireWall::decode(QByteArrayView payload)
{
    return parse<PierceFireWall>(payload, [](Reader &r, PierceFireWall &m) { m.token = r.u32(); });
}

QByteArray PeerInit::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(username);
        w.string(QByteArrayView(connectionTypeCode(type)));
        w.u32(token);
    });
}

std::optional<PeerInit> PeerInit::decode(QByteArrayView payload)
{
    return parse<PeerInit>(payload, [](Reader &r, PeerInit &m) {
        m.username = readText(r);
        const std::optional<ConnectionType> type = parseConnectionType(r.string());
        if (!type) {
            r.fail();
            return;
        }
        m.type = *type;
        // Always 0 today; aioslsk sends 8 bytes. Missing in very old clients.
        if (r.remaining() >= 4) {
            m.token = r.u32();
        }
    });
}

} // namespace peerinit

// --- peer ------------------------------------------------------------------------------------------------------------
namespace peer {

QByteArray GetShareFileList::encode() const
{
    return {};
}

std::optional<GetShareFileList> GetShareFileList::decode(QByteArrayView /*payload*/)
{
    // Some Windows clients add a 4-byte ticket (aioslsk notes); it carries nothing we use.
    return GetShareFileList{};
}

QByteArray SharedFileListResponse::encode() const
{
    Writer w;
    writeDirectories(w, list.directories);
    w.u32(list.unknown);
    writeDirectories(w, list.privateDirectories);
    return zlib::deflate(QByteArrayView(w.data()), kCompressionLevel);
}

std::optional<SharedFileListResponse> SharedFileListResponse::decode(QByteArrayView payload)
{
    return decode(payload, protocol::kShareListInflatedMax);
}

std::optional<SharedFileListResponse> SharedFileListResponse::decode(QByteArrayView payload, qsizetype maxInflated)
{
    return parseCompressed<SharedFileListResponse>(payload, maxInflated, [](Reader &r, SharedFileListResponse &m) {
        m.list.directories = readDirectories(r);
        if (r.remaining() >= 4) {
            m.list.unknown = r.u32();
        }
        m.list.privateDirectories =
            readOptional<QList<SharedDirectory>>(r, readDirectories).value_or(QList<SharedDirectory>());
    });
}

QByteArray FileSearchResponse::encode() const
{
    Writer w;
    w.string(username);
    w.u32(token);
    writeEntries(w, results);
    w.boolean(slotFree);
    w.u32(averageSpeed);
    w.u32(queueLength);
    w.u32(unknown);
    writeEntries(w, privateResults);
    return zlib::deflate(QByteArrayView(w.data()), kCompressionLevel);
}

std::optional<FileSearchResponse> FileSearchResponse::decode(QByteArrayView payload)
{
    return decode(payload, protocol::kPeerLargeInflatedMax);
}

std::optional<FileSearchResponse> FileSearchResponse::decode(QByteArrayView payload, qsizetype maxInflated)
{
    return parseCompressed<FileSearchResponse>(payload, maxInflated, [](Reader &r, FileSearchResponse &m) {
        m.username = readText(r);
        m.token = r.u32();
        m.results = readEntries(r);
        // Every trailing field is optional in practice (protocol §6.2).
        if (r.ok() && !r.atEnd()) {
            m.slotFree = r.boolean();
        }
        if (r.remaining() >= 4) {
            m.averageSpeed = r.u32();
        }
        if (r.remaining() >= 4) {
            m.queueLength = r.u32();
        }
        if (r.remaining() >= 4) {
            m.unknown = r.u32();
        }
        m.privateResults = readOptional<QList<FileEntry>>(r, readEntries).value_or(QList<FileEntry>());
    });
}

std::optional<FileSearchResponse::Header> FileSearchResponse::peek(QByteArrayView payload)
{
    // Enough for the longest username we accept and the token.
    constexpr qsizetype kPrefix = 4 + protocol::kUsernameMaxBytes + 4;
    const zlib::Inflated prefix = zlib::inflatePrefix(payload, kPrefix);
    if (!prefix.ok()) {
        return std::nullopt;
    }
    Reader r(prefix.data);
    Header header;
    header.username = readText(r);
    header.token = r.u32();
    if (!r.ok()) {
        return std::nullopt;
    }
    return header;
}

QByteArray UserInfoRequest::encode() const
{
    return {};
}

std::optional<UserInfoRequest> UserInfoRequest::decode(QByteArrayView /*payload*/)
{
    return UserInfoRequest{};
}

QByteArray UserInfoResponse::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(description);
        w.boolean(picture.has_value());
        if (picture) {
            w.string(QByteArrayView(*picture));
        }
        w.u32(totalUploads);
        w.u32(queueSize);
        w.boolean(slotsFree);
        if (uploadPermitted) {
            w.u32(*uploadPermitted);
        }
    });
}

std::optional<UserInfoResponse> UserInfoResponse::decode(QByteArrayView payload)
{
    return parse<UserInfoResponse>(payload, [](Reader &r, UserInfoResponse &m) {
        m.description = readText(r);
        if (r.boolean()) {
            m.picture = r.string();
        }
        m.totalUploads = r.u32();
        m.queueSize = r.u32();
        m.slotsFree = r.boolean();
        // Optional; Museek+ sends slotsFree as a 32-bit value, which leaves a 3-byte tail that isn't this field.
        if (r.remaining() >= 4) {
            m.uploadPermitted = r.u32();
        }
    });
}

QByteArray FolderContentsRequest::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(token);
        w.string(QByteArrayView(folder.raw));
    });
}

std::optional<FolderContentsRequest> FolderContentsRequest::decode(QByteArrayView payload)
{
    return parse<FolderContentsRequest>(payload, [](Reader &r, FolderContentsRequest &m) {
        m.token = r.u32();
        m.folder.raw = r.string();
    });
}

QByteArray FolderContentsResponse::encode() const
{
    Writer w;
    w.u32(token);
    w.string(QByteArrayView(folder.raw));
    writeDirectories(w, directories);
    return zlib::deflate(QByteArrayView(w.data()), kCompressionLevel);
}

std::optional<FolderContentsResponse> FolderContentsResponse::decode(QByteArrayView payload)
{
    return decode(payload, protocol::kPeerLargeInflatedMax);
}

std::optional<FolderContentsResponse> FolderContentsResponse::decode(QByteArrayView payload, qsizetype maxInflated)
{
    return parseCompressed<FolderContentsResponse>(payload, maxInflated, [](Reader &r, FolderContentsResponse &m) {
        m.token = r.u32();
        m.folder.raw = r.string();
        m.directories = readDirectories(r);
    });
}

QByteArray TransferRequest::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(static_cast<quint32>(direction));
        w.u32(token);
        w.string(QByteArrayView(path.raw));
        if (size) {
            w.u64(*size);
        }
    });
}

std::optional<TransferRequest> TransferRequest::decode(QByteArrayView payload)
{
    return parse<TransferRequest>(payload, [](Reader &r, TransferRequest &m) {
        m.direction = static_cast<TransferDirection>(r.u32());
        m.token = r.u32();
        m.path.raw = r.string();
        // Sent with direction 1; Soulseek.NET-based clients send it with direction 0 too.
        if (r.remaining() >= 8) {
            m.size = normalizeFileSize(r.u64());
        }
    });
}

QByteArray TransferResponse::encode() const
{
    return encodeWith([this](Writer &w) {
        w.u32(token);
        w.boolean(allowed);
        if (allowed) {
            if (size) {
                w.u64(*size);
            }
        } else {
            w.string(reason);
        }
    });
}

std::optional<TransferResponse> TransferResponse::decode(QByteArrayView payload)
{
    return parse<TransferResponse>(payload, [](Reader &r, TransferResponse &m) {
        m.token = r.u32();
        m.allowed = r.boolean();
        if (m.allowed) {
            if (r.remaining() >= 8) {
                m.size = normalizeFileSize(r.u64());
            }
        } else {
            m.reason = readOptional<QString>(r, readText).value_or(QString());
        }
    });
}

QByteArray QueueUpload::encode() const
{
    return encodeWith([this](Writer &w) { w.string(path.raw); });
}

std::optional<QueueUpload> QueueUpload::decode(QByteArrayView payload)
{
    return parse<QueueUpload>(payload, [](Reader &r, QueueUpload &m) { m.path.raw = r.string(); });
}

QByteArray UploadFailed::encode() const
{
    return encodeWith([this](Writer &w) { w.string(path.raw); });
}

std::optional<UploadFailed> UploadFailed::decode(QByteArrayView payload)
{
    return parse<UploadFailed>(payload, [](Reader &r, UploadFailed &m) { m.path.raw = r.string(); });
}

QByteArray PlaceInQueueRequest::encode() const
{
    return encodeWith([this](Writer &w) { w.string(path.raw); });
}

std::optional<PlaceInQueueRequest> PlaceInQueueRequest::decode(QByteArrayView payload)
{
    return parse<PlaceInQueueRequest>(payload, [](Reader &r, PlaceInQueueRequest &m) { m.path.raw = r.string(); });
}

QByteArray PlaceInQueueResponse::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(QByteArrayView(path.raw));
        w.u32(place);
    });
}

std::optional<PlaceInQueueResponse> PlaceInQueueResponse::decode(QByteArrayView payload)
{
    return parse<PlaceInQueueResponse>(payload, [](Reader &r, PlaceInQueueResponse &m) {
        m.path.raw = r.string();
        m.place = r.u32();
    });
}

QByteArray UploadDenied::encode() const
{
    return encodeWith([this](Writer &w) {
        w.string(QByteArrayView(path.raw));
        w.string(reason);
    });
}

std::optional<UploadDenied> UploadDenied::decode(QByteArrayView payload)
{
    return parse<UploadDenied>(payload, [](Reader &r, UploadDenied &m) {
        m.path.raw = r.string();
        m.reason = readText(r);
    });
}

} // namespace peer

// --- file ------------------------------------------------------------------------------------------------------------
namespace file {

QByteArray encodeTransferInit(quint32 token)
{
    QByteArray bytes(kTransferInitBytes, Qt::Uninitialized);
    qToLittleEndian(token, bytes.data());
    return bytes;
}

std::optional<quint32> decodeTransferInit(QByteArrayView bytes)
{
    if (bytes.size() != kTransferInitBytes) {
        return std::nullopt;
    }
    return qFromLittleEndian<quint32>(bytes.data());
}

QByteArray encodeOffset(quint64 offset)
{
    QByteArray bytes(kOffsetBytes, Qt::Uninitialized);
    qToLittleEndian(offset, bytes.data());
    return bytes;
}

std::optional<quint64> decodeOffset(QByteArrayView bytes)
{
    if (bytes.size() != kOffsetBytes) {
        return std::nullopt;
    }
    return qFromLittleEndian<quint64>(bytes.data());
}

} // namespace file

// --- dispatch --------------------------------------------------------------------------------------------------------

bool isKnownPeerCode(quint32 code)
{
    switch (static_cast<PeerCode>(code)) {
    case PeerCode::PrivateMessage:
    case PeerCode::GetShareFileList:
    case PeerCode::SharedFileListResponse:
    case PeerCode::FileSearchRequest:
    case PeerCode::FileSearchResponse:
    case PeerCode::RoomInvitation:
    case PeerCode::CancelledQueuedTransfer:
    case PeerCode::UserInfoRequest:
    case PeerCode::UserInfoResponse:
    case PeerCode::SendConnectToken:
    case PeerCode::MoveDownloadToTop:
    case PeerCode::FolderContentsRequest:
    case PeerCode::FolderContentsResponse:
    case PeerCode::TransferRequest:
    case PeerCode::TransferResponse:
    case PeerCode::PlaceholdUpload:
    case PeerCode::QueueUpload:
    case PeerCode::PlaceInQueueResponse:
    case PeerCode::UploadFailed:
    case PeerCode::ExactFileSearchRequest:
    case PeerCode::QueuedDownloads:
    case PeerCode::IndirectFileSearchRequest:
    case PeerCode::UploadDenied:
    case PeerCode::PlaceInQueueRequest:
    case PeerCode::UploadQueueNotification:
        return true;
    }
    return false;
}

Decoded<PeerInitMessage> decodePeerInit(quint32 code, QByteArrayView payload)
{
    using internal::decodeAs;
    if (code == static_cast<quint32>(PeerInitCode::PierceFireWall)) {
        return decodeAs<PeerInitMessage, peerinit::PierceFireWall>(payload);
    }
    if (code == static_cast<quint32>(PeerInitCode::PeerInit)) {
        return decodeAs<PeerInitMessage, peerinit::PeerInit>(payload);
    }
    return {DecodeStatus::Unknown, std::nullopt};
}

Decoded<PeerMessage> decodePeer(quint32 code, QByteArrayView payload)
{
    using internal::decodeAs;
    using V = PeerMessage;
    switch (static_cast<PeerCode>(code)) {
    case PeerCode::GetShareFileList:
        return decodeAs<V, peer::GetShareFileList>(payload);
    case PeerCode::SharedFileListResponse:
        return decodeAs<V, peer::SharedFileListResponse>(payload);
    case PeerCode::FileSearchResponse:
        return decodeAs<V, peer::FileSearchResponse>(payload);
    case PeerCode::UserInfoRequest:
        return decodeAs<V, peer::UserInfoRequest>(payload);
    case PeerCode::UserInfoResponse:
        return decodeAs<V, peer::UserInfoResponse>(payload);
    case PeerCode::FolderContentsRequest:
        return decodeAs<V, peer::FolderContentsRequest>(payload);
    case PeerCode::FolderContentsResponse:
        return decodeAs<V, peer::FolderContentsResponse>(payload);
    case PeerCode::TransferRequest:
        return decodeAs<V, peer::TransferRequest>(payload);
    case PeerCode::TransferResponse:
        return decodeAs<V, peer::TransferResponse>(payload);
    case PeerCode::QueueUpload:
        return decodeAs<V, peer::QueueUpload>(payload);
    case PeerCode::PlaceInQueueResponse:
        return decodeAs<V, peer::PlaceInQueueResponse>(payload);
    case PeerCode::UploadFailed:
        return decodeAs<V, peer::UploadFailed>(payload);
    case PeerCode::UploadDenied:
        return decodeAs<V, peer::UploadDenied>(payload);
    case PeerCode::PlaceInQueueRequest:
        return decodeAs<V, peer::PlaceInQueueRequest>(payload);
    default:
        break;
    }
    return {isKnownPeerCode(code) ? DecodeStatus::Ignored : DecodeStatus::Unknown, std::nullopt};
}

bool isAcceptablePeerUsername(const QString &username)
{
    if (username.isEmpty() || username.toUtf8().size() > protocol::kUsernameMaxBytes ||
        username == protocol::kServerUsername) {
        return false;
    }
    // By code point, so characters outside the BMP are judged whole rather than as surrogate halves.
    const QList<uint> codePoints = username.toUcs4();
    return std::ranges::all_of(codePoints, [](uint c) {
        return QChar::isPrint(static_cast<char32_t>(c)) && c != QChar::LineSeparator && c != QChar::ParagraphSeparator;
    });
}

} // namespace selecta::slsk
