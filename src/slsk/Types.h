// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>
#include <utility>

// Value types shared by the codec and the sessions. All are copyable, cheap to copy (Qt implicit sharing) and safe to
// pass between threads in queued signals.
namespace selecta::slsk {

class Reader;
class Writer;

// A path on a remote peer, e.g. `Music\Artist - Album\01 Track.flac`. The bytes are kept exactly as the peer sent
// them and are always echoed back unchanged (protocol §2.2): re-encoding a decoded string would break paths from
// peers that don't use UTF-8. The separator is `\`. A remote path is display data and a lookup key, never a local
// path.
struct RemotePath
{
    QByteArray raw;

    // For paths typed or rebuilt locally: the UTF-8 bytes of `text`.
    static RemotePath fromText(const QString &text) { return RemotePath{text.toUtf8()}; }

    // UTF-8 when valid, Latin-1 otherwise.
    QString display() const;
    bool isEmpty() const { return raw.isEmpty(); }
    // The same path encoded as Latin-1, for the one retry Nicotine+ makes when a peer answers "File not shared."
    // (an old client that doesn't speak UTF-8). nullopt when the path isn't valid UTF-8, has characters outside
    // Latin-1, or would encode to the same bytes.
    std::optional<RemotePath> latin1Variant() const;

    friend bool operator==(const RemotePath &, const RemotePath &) = default;
};

inline size_t qHash(const RemotePath &path, size_t seed = 0) noexcept
{
    return qHash(path.raw, seed);
}

// The three kinds of peer connection (protocol §3.1).
enum class ConnectionType : quint8 {
    Peer,        // "P": messages
    File,        // "F": one file transfer
    Distributed, // "D": the search tree
};

// "P", "F" or "D".
QByteArray connectionTypeCode(ConnectionType type);
// nullopt for anything but exactly "P", "F" or "D".
std::optional<ConnectionType> parseConnectionType(QByteArrayView code);

// A user's presence. Values the protocol doesn't define are kept as they arrive: treat them as offline.
enum class UserStatus : quint32 {
    Offline = 0,
    Away = 1,
    Online = 2,
};

// What the server knows about a user's sharing (GetUserStats, WatchUser, room members).
struct UserStats
{
    quint32 averageSpeed = 0; // bytes per second, the server's running mean of SendUploadSpeed reports
    quint32 uploadCount = 0;  // rolls over
    quint32 unknown = 0;      // always sent; meaning unknown
    quint32 files = 0;
    quint32 folders = 0;

    friend bool operator==(const UserStats &, const UserStats &) = default;
};

// File attribute codes (protocol §6.2).
enum class AttributeCode : quint32 {
    Bitrate = 0,
    Duration = 1,
    Vbr = 2,
    Encoder = 3, // obsolete
    SampleRate = 4,
    BitDepth = 5,
};

// Audio attributes of a shared file (protocol §6.2). Every attribute may be missing; a missing VBR flag means
// "unknown", never CBR: SoulseekQt sends no VBR flag at all.
struct FileAttributes
{
    std::optional<quint32> bitrate;    // kbps
    std::optional<quint32> duration;   // seconds
    std::optional<bool> vbr;           // any non-zero value is true
    std::optional<quint32> sampleRate; // Hz
    std::optional<quint32> bitDepth;   // bits
    // Codes other than the five above (encoder and anything newer), and repeats of a known code after its first
    // occurrence, in the order received.
    QList<std::pair<quint32, quint32>> other;

    bool isEmpty() const;
    // The number of (code, value) pairs encode() writes.
    qsizetype pairCount() const;

    friend bool operator==(const FileAttributes &, const FileAttributes &) = default;
};

// One file in a share list, a folder listing or a search result.
struct FileEntry
{
    RemotePath name;   // search results: the full virtual path; share lists and folder contents: the bare name
    quint64 size = 0;  // see normalizeFileSize()
    QString extension; // often empty (SoulseekQt); derive it from the name instead
    FileAttributes attributes;

    // Encoded as `uint8 1` · name · `uint64` size · extension · attribute list.
    void encode(Writer &writer) const;
    static std::optional<FileEntry> decode(Reader &reader);
    // The fewest bytes an encoded entry can take (for count checks).
    static constexpr qsizetype kMinBytes = 1 + 4 + 8 + 4 + 4;

    friend bool operator==(const FileEntry &, const FileEntry &) = default;
};

// Soulseek NS sends sizes above 2 GiB as a negative 32-bit number sign-extended to 64 bits. A size whose upper four
// bytes are all 0xFF is read as its lower four bytes, unsigned (protocol §2.5).
quint64 normalizeFileSize(quint64 wireSize);

// A folder and its files.
struct SharedDirectory
{
    RemotePath path; // full virtual path, e.g. `Music\Artist - Album`
    QList<FileEntry> files;

    void encode(Writer &writer) const;
    static std::optional<SharedDirectory> decode(Reader &reader);
    static constexpr qsizetype kMinBytes = 4 + 4;

    friend bool operator==(const SharedDirectory &, const SharedDirectory &) = default;
};

// A peer's whole share list (SharedFileListResponse). Private folders are visible but downloads from them are
// refused (buddy-only or locked shares).
struct SharedFileList
{
    QList<SharedDirectory> directories;
    quint32 unknown = 0; // official clients always send 0
    QList<SharedDirectory> privateDirectories;

    friend bool operator==(const SharedFileList &, const SharedFileList &) = default;
};

// --- Transfer rejections (protocol §6.5) -----------------------------------------------------------------------------

// The reason strings peers send in TransferResponse and UploadDenied. The texts are exact: case and the trailing dot
// matter.
enum class RejectionReason {
    Banned,              // "Banned", or "Banned (<message>)"
    Cancelled,           // "Cancelled"
    Complete,            // "Complete"
    FileNotShared,       // "File not shared." (also the deprecated form without the dot)
    FileReadError,       // "File read error."
    PendingShutdown,     // "Pending shutdown."
    Queued,              // "Queued": the answer to a legacy download request
    TooManyFiles,        // "Too many files" (also the deprecated "User limit of x files exceeded")
    TooManyMegabytes,    // "Too many megabytes" (also the deprecated "User limit of x megabytes exceeded")
    BlockedCountry,      // deprecated
    DisallowedExtension, // deprecated (Soulseek NS)
    RemoteFileError,     // deprecated (Soulseek NS)
    Other,               // any other text: a generic denial
};

// What core/ should do about a rejection.
enum class RejectionClass {
    Retryable, // try again later (queue limits, shutdown, read errors), through the governor
    Never,     // don't retry automatically
    Banned,    // the peer banned us
};

struct Rejection
{
    QString text; // exactly as received
    RejectionReason reason = RejectionReason::Other;
    RejectionClass rejectionClass = RejectionClass::Never;

    friend bool operator==(const Rejection &, const Rejection &) = default;
};

Rejection classifyRejection(const QString &text);
// The exact text to send for a reason that is still in use; empty for deprecated reasons and Other.
QString rejectionText(RejectionReason reason);

// An IPv4 address as the protocol carries it: a `uint32` whose value is the address in host order.
inline QHostAddress ipv4Address(quint32 wire)
{
    return QHostAddress(wire);
}

} // namespace selecta::slsk

Q_DECLARE_METATYPE(selecta::slsk::RemotePath)
Q_DECLARE_METATYPE(selecta::slsk::ConnectionType)
Q_DECLARE_METATYPE(selecta::slsk::UserStatus)
Q_DECLARE_METATYPE(selecta::slsk::UserStats)
Q_DECLARE_METATYPE(selecta::slsk::FileEntry)
Q_DECLARE_METATYPE(selecta::slsk::SharedDirectory)
Q_DECLARE_METATYPE(selecta::slsk::SharedFileList)
Q_DECLARE_METATYPE(selecta::slsk::Rejection)
