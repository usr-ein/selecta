// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Protocol.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

// Declares the members every wire message struct has (ServerMessages.h, PeerMessages.h, DistributedMessages.h):
//
//   static constexpr kCode               its message code
//   static constexpr kName               the struct's name, for logs
//   QByteArray encode() const            the payload: no length and no code (Frame.h adds them)
//   static std::optional<T> decode(...)  the message, or nullopt when the payload is malformed. Never throws and never
//                                        reads out of bounds. Optional trailing fields are read only if bytes remain;
//                                        extra trailing bytes are ignored.
//   operator==                           field-wise, for tests
//
// Strings that name things (users, rooms, messages) are decoded to QString (UTF-8, Latin-1 fallback); paths stay raw
// bytes (RemotePath).
#define SELECTA_SLSK_MESSAGE(Type, codeValue)                                                                          \
    static constexpr auto kCode = codeValue;                                                                           \
    static constexpr const char *kName = #Type;                                                                        \
    QByteArray encode() const;                                                                                         \
    static std::optional<Type> decode(QByteArrayView payload);                                                         \
    friend bool operator==(const Type &, const Type &) = default

// Primitive encoding of the Soulseek protocol (references/soulseek-protocol.md §2.1–2.2): little-endian integers,
// one-byte booleans and `uint32`-length-prefixed byte strings.
//
// Everything here is pure and reentrant: no shared state, safe to use from any thread at the same time.
namespace selecta::slsk {

// Bounds-checked little-endian reader over a byte view. It never throws and never reads past the end: the first
// failed read sets a sticky failure flag, and every later read returns a zero value. Decoders read their fields and
// check ok() once at the end.
//
// The view is not copied: the bytes must outlive the Reader.
class Reader
{
public:
    explicit Reader(QByteArrayView data) noexcept
        : m_data(data)
    {}

    quint8 u8() noexcept;
    quint16 u16() noexcept;
    quint32 u32() noexcept;
    qint32 i32() noexcept;
    quint64 u64() noexcept;
    bool boolean() noexcept; // one byte; any non-zero value is true
    // A `uint32` length and that many raw bytes, kept as they are (no text decoding). Fails if the length exceeds
    // the bytes that remain, so a hostile length never allocates more than the message holds. Also fails if the bytes
    // are there but exceed `maxBytes` (protocol::kStringMaxBytes unless a field says otherwise): that sets oversize().
    QByteArray string(qsizetype maxBytes = kDefaultStringMaxBytes);
    // `count` raw bytes.
    QByteArray bytes(qsizetype count);
    // A `uint32` list count, checked against the bytes that remain: each item takes at least `minItemBytes`, so a
    // count that can't fit fails here and callers may reserve() the returned count safely.
    qsizetype count(qsizetype minItemBytes) noexcept;
    // Everything that remains, without copying. Consumes it.
    QByteArrayView rest() noexcept;
    void skip(qsizetype count) noexcept;

    qsizetype remaining() const noexcept { return m_failed ? 0 : m_data.size() - m_pos; }
    bool atEnd() const noexcept { return remaining() == 0; }
    bool ok() const noexcept { return !m_failed; }
    // True when the failure was a string longer than its cap: the message is hostile, not merely cut short or followed
    // by junk, so even an optional trailing field that holds one fails the whole message (internal::readOptional).
    bool oversize() const noexcept { return m_oversize; }
    void fail() noexcept { m_failed = true; }

    static constexpr qsizetype kDefaultStringMaxBytes = protocol::kStringMaxBytes;

private:
    bool take(qsizetype count, const char **out) noexcept;

    QByteArrayView m_data;
    qsizetype m_pos = 0;
    bool m_failed = false;
    bool m_oversize = false;
};

// Little-endian writer into a growing QByteArray.
class Writer
{
public:
    Writer() = default;
    explicit Writer(qsizetype reserve) { m_data.reserve(reserve); }

    void u8(quint8 value);
    void u16(quint16 value);
    void u32(quint32 value);
    void i32(qint32 value);
    void u64(quint64 value);
    void boolean(bool value);
    // `uint32` length and the raw bytes.
    void string(QByteArrayView raw);
    void string(const QByteArray &raw) { string(QByteArrayView(raw)); }
    // `uint32` length and the UTF-8 bytes.
    void string(const QString &text);
    // Raw bytes, no length.
    void bytes(QByteArrayView raw);

    qsizetype size() const { return m_data.size(); }
    const QByteArray &data() const { return m_data; }
    QByteArray take() { return std::move(m_data); }

private:
    QByteArray m_data;
};

// Text from the wire: UTF-8 when the bytes are valid UTF-8, Latin-1 otherwise (protocol §2.2). Never fails.
QString decodeText(QByteArrayView raw);
// True when `raw` is valid UTF-8.
bool isValidUtf8(QByteArrayView raw);
// Lower-case hexadecimal MD5 digest, as the login message uses it.
QByteArray md5Hex(QByteArrayView data);

// The outcome of decoding one framed message.
enum class DecodeStatus {
    Decoded,   // the message was understood; `message` holds it
    Malformed, // a known code whose payload didn't parse: drop the message, keep the connection
    Ignored,   // a known code Selecta deliberately doesn't decode (obsolete, out of scope): skip it
    Unknown,   // a code this version doesn't know: skip it
};

const char *decodeStatusName(DecodeStatus status);

template <typename Message>
struct Decoded
{
    DecodeStatus status = DecodeStatus::Unknown;
    std::optional<Message> message;

    bool ok() const { return status == DecodeStatus::Decoded && message.has_value(); }
};

} // namespace selecta::slsk
