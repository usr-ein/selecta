// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Frame.h"

#include <QtEndian>

#include <algorithm>
#include <limits>
#include <utility>

namespace selecta::slsk {
namespace {

constexpr qsizetype kLengthBytes = 4;
// Consumed bytes are dropped from the front of the buffer once they pass this size (or half the buffer).
constexpr qsizetype kCompactThreshold = qsizetype{64} * 1024;

} // namespace

QByteArray encodeFrame(CodeWidth width, quint32 code, QByteArrayView payload)
{
    const auto codeBytes = static_cast<qsizetype>(width);
    const qsizetype length = codeBytes + payload.size();
    Q_ASSERT(std::cmp_less_equal(length, std::numeric_limits<quint32>::max()));
    QByteArray frame(kLengthBytes + length, Qt::Uninitialized);
    char *p = frame.data();
    qToLittleEndian(static_cast<quint32>(length), p);
    if (width == CodeWidth::Byte) {
        Q_ASSERT(code <= 0xFF);
        p[kLengthBytes] = static_cast<char>(static_cast<quint8>(code));
    } else {
        qToLittleEndian(code, p + kLengthBytes);
    }
    if (!payload.isEmpty()) {
        std::ranges::copy(payload, p + kLengthBytes + codeBytes);
    }
    return frame;
}

FrameReader::FrameReader(CodeWidth width, qsizetype maxPayload)
    : m_width(width)
    , m_maxPayload(maxPayload)
{}

void FrameReader::append(QByteArrayView bytes)
{
    if (m_error != Error::None || bytes.isEmpty()) {
        return;
    }
    if (m_offset > 0 && (m_offset >= kCompactThreshold || m_offset * 2 >= m_buffer.size())) {
        m_buffer.remove(0, m_offset);
        m_offset = 0;
    }
    // Grow towards the end of the frame being read rather than doubling past it, so a large frame costs about its own
    // size in memory. Memory follows the bytes that actually arrive: nothing is reserved on the length field alone.
    const qsizetype needed = m_buffer.size() + bytes.size();
    if (m_headerChecked && needed > m_buffer.capacity()) {
        const qsizetype frameEnd = m_offset + kLengthBytes + static_cast<qsizetype>(m_width) + m_pendingPayload;
        if (needed <= frameEnd) {
            m_buffer.reserve(std::min(frameEnd, std::max(needed, m_buffer.capacity() * 2)));
        }
    }
    m_buffer.append(bytes);
    checkHeader();
}

bool FrameReader::checkHeader()
{
    if (m_error != Error::None) {
        return false;
    }
    if (m_headerChecked) {
        return true;
    }
    const auto codeBytes = static_cast<qsizetype>(m_width);
    if (buffered() < kLengthBytes + codeBytes) {
        return true;
    }
    const char *p = m_buffer.constData() + m_offset;
    const auto length = qFromLittleEndian<quint32>(p);
    if (std::cmp_less(length, codeBytes)) {
        m_error = Error::TooShort;
        return false;
    }
    const quint32 code = m_width == CodeWidth::Byte ? static_cast<quint8>(p[kLengthBytes])
                                                    : qFromLittleEndian<quint32>(p + kLengthBytes);
    const qsizetype payload = static_cast<qsizetype>(length) - codeBytes;
    const qsizetype cap = m_caps ? m_caps(code) : m_maxPayload;
    if (cap < 0) {
        m_error = Error::Unsolicited;
        return false;
    }
    if (payload > cap) {
        m_error = Error::Oversize;
        return false;
    }
    m_pendingCode = code;
    m_pendingPayload = payload;
    m_headerChecked = true;
    return true;
}

std::optional<Frame> FrameReader::next()
{
    if (!checkHeader() || !m_headerChecked) {
        return std::nullopt;
    }
    const qsizetype headerBytes = kLengthBytes + static_cast<qsizetype>(m_width);
    if (buffered() < headerBytes + m_pendingPayload) {
        return std::nullopt;
    }
    Frame frame;
    frame.code = m_pendingCode;
    frame.payload = QByteArray(m_buffer.constData() + m_offset + headerBytes, m_pendingPayload);
    m_offset += headerBytes + m_pendingPayload;
    m_headerChecked = false;
    m_pendingPayload = 0;
    m_pendingCode = 0;
    if (m_offset == m_buffer.size()) {
        m_buffer.clear();
        m_offset = 0;
    }
    checkHeader(); // judge the next frame's header as soon as it's there
    return frame;
}

QByteArray FrameReader::takeBuffered()
{
    QByteArray rest = m_buffer.mid(m_offset);
    m_buffer.clear();
    m_offset = 0;
    m_headerChecked = false;
    m_pendingPayload = 0;
    m_pendingCode = 0;
    return rest;
}

const char *frameErrorName(FrameReader::Error error)
{
    switch (error) {
    case FrameReader::Error::None:
        return "none";
    case FrameReader::Error::Oversize:
        return "frame over its size cap";
    case FrameReader::Error::TooShort:
        return "frame length shorter than its code";
    case FrameReader::Error::Unsolicited:
        return "unsolicited frame";
    }
    return "?";
}

} // namespace selecta::slsk
