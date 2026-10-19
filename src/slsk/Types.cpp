// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Types.h"

#include "slsk/Codec.h"

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::slsk {

QString RemotePath::display() const
{
    return decodeText(raw);
}

std::optional<RemotePath> RemotePath::latin1Variant() const
{
    if (!isValidUtf8(raw)) {
        return std::nullopt;
    }
    const QString text = QString::fromUtf8(raw);
    const bool fitsLatin1 = std::ranges::all_of(text, [](QChar c) { return c.unicode() <= 0xFF; });
    if (!fitsLatin1) {
        return std::nullopt;
    }
    QByteArray latin1 = text.toLatin1();
    if (latin1 == raw) {
        return std::nullopt;
    }
    return RemotePath{std::move(latin1)};
}

QByteArray connectionTypeCode(ConnectionType type)
{
    switch (type) {
    case ConnectionType::Peer:
        return QByteArrayLiteral("P");
    case ConnectionType::File:
        return QByteArrayLiteral("F");
    case ConnectionType::Distributed:
        return QByteArrayLiteral("D");
    }
    return QByteArrayLiteral("P");
}

std::optional<ConnectionType> parseConnectionType(QByteArrayView code)
{
    if (code == "P") {
        return ConnectionType::Peer;
    }
    if (code == "F") {
        return ConnectionType::File;
    }
    if (code == "D") {
        return ConnectionType::Distributed;
    }
    return std::nullopt;
}

bool FileAttributes::isEmpty() const
{
    return pairCount() == 0;
}

qsizetype FileAttributes::pairCount() const
{
    return (bitrate ? 1 : 0) + (duration ? 1 : 0) + (vbr ? 1 : 0) + (sampleRate ? 1 : 0) + (bitDepth ? 1 : 0) +
           other.size();
}

void FileEntry::encode(Writer &writer) const
{
    writer.u8(1);
    writer.string(QByteArrayView(name.raw));
    writer.u64(size);
    writer.string(extension);
    writer.u32(static_cast<quint32>(attributes.pairCount()));
    const auto pair = [&writer](AttributeCode code, quint32 value) {
        writer.u32(static_cast<quint32>(code));
        writer.u32(value);
    };
    if (attributes.bitrate) {
        pair(AttributeCode::Bitrate, *attributes.bitrate);
    }
    if (attributes.duration) {
        pair(AttributeCode::Duration, *attributes.duration);
    }
    if (attributes.vbr) {
        pair(AttributeCode::Vbr, *attributes.vbr ? 1 : 0);
    }
    if (attributes.sampleRate) {
        pair(AttributeCode::SampleRate, *attributes.sampleRate);
    }
    if (attributes.bitDepth) {
        pair(AttributeCode::BitDepth, *attributes.bitDepth);
    }
    for (const auto &[code, value] : attributes.other) {
        writer.u32(code);
        writer.u32(value);
    }
}

std::optional<FileEntry> FileEntry::decode(Reader &reader)
{
    FileEntry entry;
    reader.u8(); // always 1; its value carries no meaning
    entry.name.raw = reader.string();
    entry.size = normalizeFileSize(reader.u64());
    entry.extension = decodeText(reader.string());
    const qsizetype count = reader.count(8);
    for (qsizetype i = 0; i < count && reader.ok(); ++i) {
        const quint32 code = reader.u32();
        const quint32 value = reader.u32();
        FileAttributes &a = entry.attributes;
        const auto keepFirst = [&](std::optional<quint32> &slot) {
            if (slot) {
                a.other.append({code, value});
            } else {
                slot = value;
            }
        };
        switch (static_cast<AttributeCode>(code)) {
        case AttributeCode::Bitrate:
            keepFirst(a.bitrate);
            break;
        case AttributeCode::Duration:
            keepFirst(a.duration);
            break;
        case AttributeCode::Vbr:
            if (a.vbr) {
                a.other.append({code, value});
            } else {
                a.vbr = value != 0;
            }
            break;
        case AttributeCode::SampleRate:
            keepFirst(a.sampleRate);
            break;
        case AttributeCode::BitDepth:
            keepFirst(a.bitDepth);
            break;
        case AttributeCode::Encoder:
        default:
            a.other.append({code, value});
            break;
        }
    }
    if (!reader.ok()) {
        return std::nullopt;
    }
    return entry;
}

quint64 normalizeFileSize(quint64 wireSize)
{
    constexpr quint64 kHighMask = 0xFFFF'FFFF'0000'0000ULL;
    if ((wireSize & kHighMask) == kHighMask) {
        return wireSize & 0xFFFF'FFFFULL;
    }
    return wireSize;
}

void SharedDirectory::encode(Writer &writer) const
{
    writer.string(QByteArrayView(path.raw));
    writer.u32(static_cast<quint32>(files.size()));
    for (const FileEntry &file : files) {
        file.encode(writer);
    }
}

std::optional<SharedDirectory> SharedDirectory::decode(Reader &reader)
{
    SharedDirectory directory;
    directory.path.raw = reader.string();
    const qsizetype count = reader.count(FileEntry::kMinBytes);
    directory.files.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        std::optional<FileEntry> file = FileEntry::decode(reader);
        if (!file) {
            return std::nullopt;
        }
        directory.files.append(std::move(*file));
    }
    if (!reader.ok()) {
        return std::nullopt;
    }
    return directory;
}

Rejection classifyRejection(const QString &text)
{
    struct Known
    {
        QStringView text;
        RejectionReason reason = RejectionReason::Other;
        RejectionClass rejectionClass = RejectionClass::Never;
    };
    static constexpr std::array kExact = {
        Known{u"Banned", RejectionReason::Banned, RejectionClass::Banned},
        Known{u"Cancelled", RejectionReason::Cancelled, RejectionClass::Never},
        Known{u"Complete", RejectionReason::Complete, RejectionClass::Never},
        Known{u"File not shared.", RejectionReason::FileNotShared, RejectionClass::Never},
        Known{u"File not shared", RejectionReason::FileNotShared, RejectionClass::Never},
        Known{u"File read error.", RejectionReason::FileReadError, RejectionClass::Retryable},
        Known{u"Pending shutdown.", RejectionReason::PendingShutdown, RejectionClass::Retryable},
        Known{u"Queued", RejectionReason::Queued, RejectionClass::Retryable},
        Known{u"Too many files", RejectionReason::TooManyFiles, RejectionClass::Retryable},
        Known{u"Too many megabytes", RejectionReason::TooManyMegabytes, RejectionClass::Retryable},
        Known{u"Blocked country", RejectionReason::BlockedCountry, RejectionClass::Never},
        Known{u"Disallowed extension", RejectionReason::DisallowedExtension, RejectionClass::Never},
        Known{u"Remote file error", RejectionReason::RemoteFileError, RejectionClass::Never},
    };
    for (const Known &known : kExact) {
        if (text == known.text) {
            return Rejection{text, known.reason, known.rejectionClass};
        }
    }
    // Nicotine+ may add a custom message: "Banned (Banned, don't bother retrying)".
    if (text.startsWith(u"Banned ("_s) && text.endsWith(u')')) {
        return Rejection{text, RejectionReason::Banned, RejectionClass::Banned};
    }
    // Old Nicotine+: "User limit of 100 files exceeded", "User limit of 500 megabytes exceeded".
    if (text.startsWith(u"User limit of "_s)) {
        if (text.endsWith(u" files exceeded"_s)) {
            return Rejection{text, RejectionReason::TooManyFiles, RejectionClass::Retryable};
        }
        if (text.endsWith(u" megabytes exceeded"_s)) {
            return Rejection{text, RejectionReason::TooManyMegabytes, RejectionClass::Retryable};
        }
    }
    return Rejection{text, RejectionReason::Other, RejectionClass::Never};
}

QString rejectionText(RejectionReason reason)
{
    switch (reason) {
    case RejectionReason::Banned:
        return u"Banned"_s;
    case RejectionReason::Cancelled:
        return u"Cancelled"_s;
    case RejectionReason::Complete:
        return u"Complete"_s;
    case RejectionReason::FileNotShared:
        return u"File not shared."_s;
    case RejectionReason::FileReadError:
        return u"File read error."_s;
    case RejectionReason::PendingShutdown:
        return u"Pending shutdown."_s;
    case RejectionReason::Queued:
        return u"Queued"_s;
    case RejectionReason::TooManyFiles:
        return u"Too many files"_s;
    case RejectionReason::TooManyMegabytes:
        return u"Too many megabytes"_s;
    case RejectionReason::BlockedCountry:
    case RejectionReason::DisallowedExtension:
    case RejectionReason::RemoteFileError:
    case RejectionReason::Other:
        return {};
    }
    return {};
}

} // namespace selecta::slsk
