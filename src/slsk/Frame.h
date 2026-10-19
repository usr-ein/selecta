// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Protocol.h"

#include <QByteArray>
#include <QByteArrayView>

#include <functional>
#include <optional>
#include <type_traits>

// Message framing (references/soulseek-protocol.md §2.3): `uint32 length` · code · payload, where `length` counts the
// code and the payload. Server and peer codes are `uint32`; peer-init and distributed codes are `uint8`.
namespace selecta::slsk {

enum class CodeWidth : quint8 {
    Byte = 1, // peer init, distributed
    Word = 4, // server, peer
};

template <typename Code>
constexpr CodeWidth codeWidthOf()
{
    return sizeof(std::underlying_type_t<Code>) == 1 ? CodeWidth::Byte : CodeWidth::Word;
}

struct Frame
{
    quint32 code = 0;
    QByteArray payload;
};

// `length` · `code` · `payload`. Pure.
QByteArray encodeFrame(CodeWidth width, quint32 code, QByteArrayView payload);

// The framed bytes of a message struct (SELECTA_SLSK_MESSAGE).
template <typename Message>
QByteArray frame(const Message &message)
{
    using Code = std::remove_cvref_t<decltype(Message::kCode)>;
    return encodeFrame(codeWidthOf<Code>(), static_cast<quint32>(Message::kCode), message.encode());
}

// Splits a byte stream into frames, enforcing a size cap per frame before its payload is buffered (NFR-4). A frame
// longer than its cap, or a length too short to hold the code, is a framing violation: the reader stops for good and
// the owner closes the connection.
//
// Not thread-safe: one reader per connection, used on that connection's thread. Holds at most one incomplete frame
// plus the bytes appended in one call.
class FrameReader
{
public:
    enum class Error {
        None,
        Oversize,    // the length field exceeds the cap for the frame's code
        TooShort,    // the length field can't even hold the code
        Unsolicited, // the cap function refused the code outright (cap 0)
    };

    // Maximum payload bytes (without length and code) for a frame with this code; 0 refuses the frame outright.
    using CapFunction = std::function<qsizetype(quint32 code)>;

    FrameReader(CodeWidth width, qsizetype maxPayload);

    // Replaces the fixed cap with one that depends on the code (peer messages: big responses only when solicited).
    void setCapFunction(CapFunction caps) { m_caps = std::move(caps); }

    void append(QByteArrayView bytes);
    // The next complete frame, or nullopt when more bytes are needed or after a violation (see error()).
    std::optional<Frame> next();

    Error error() const { return m_error; }
    bool hasError() const { return m_error != Error::None; }
    // Payload bytes the frame being read announced (0 before its header is complete). For progress and logging.
    qsizetype pendingFrameSize() const { return m_pendingPayload; }
    qsizetype buffered() const { return m_buffer.size() - m_offset; }
    // Removes and returns every byte not yet consumed, e.g. what follows a peer-init frame on an `F` connection.
    QByteArray takeBuffered();

private:
    bool checkHeader();

    CodeWidth m_width;
    qsizetype m_maxPayload;
    CapFunction m_caps;
    QByteArray m_buffer;
    qsizetype m_offset = 0; // start of the unconsumed bytes in m_buffer
    qsizetype m_pendingPayload = 0;
    bool m_headerChecked = false;
    Error m_error = Error::None;
};

const char *frameErrorName(FrameReader::Error error);

} // namespace selecta::slsk
