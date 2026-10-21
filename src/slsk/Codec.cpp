// SPDX-License-Identifier: GPL-3.0-or-later
#include "slsk/Codec.h"

#include <QCryptographicHash>
#include <QStringDecoder>
#include <QtEndian>

#include <limits>
#include <utility>

namespace selecta::slsk {
namespace {

template <typename T>
void appendLittleEndian(QByteArray &data, T value)
{
    const qsizetype at = data.size();
    data.resize(at + qsizetype{sizeof(T)});
    qToLittleEndian(value, data.data() + at);
}

} // namespace

bool Reader::take(qsizetype count, const char **out) noexcept
{
    if (m_failed || count < 0 || count > m_data.size() - m_pos) {
        m_failed = true;
        *out = nullptr;
        return false;
    }
    *out = m_data.data() + m_pos;
    m_pos += count;
    return true;
}

quint8 Reader::u8() noexcept
{
    const char *p = nullptr;
    return take(1, &p) ? static_cast<quint8>(*p) : quint8{0};
}

quint16 Reader::u16() noexcept
{
    const char *p = nullptr;
    return take(2, &p) ? qFromLittleEndian<quint16>(p) : quint16{0};
}

quint32 Reader::u32() noexcept
{
    const char *p = nullptr;
    return take(4, &p) ? qFromLittleEndian<quint32>(p) : quint32{0};
}

qint32 Reader::i32() noexcept
{
    const char *p = nullptr;
    return take(4, &p) ? qFromLittleEndian<qint32>(p) : qint32{0};
}

quint64 Reader::u64() noexcept
{
    const char *p = nullptr;
    return take(8, &p) ? qFromLittleEndian<quint64>(p) : quint64{0};
}

bool Reader::boolean() noexcept
{
    return u8() != 0;
}

QByteArray Reader::string(qsizetype maxBytes)
{
    const quint32 length = u32();
    if (m_failed) {
        return {};
    }
    // Compared with std::cmp_* so a length near 2^32 can't wrap on any platform. A length past the end is a cut-short
    // message (or junk trailing bytes); only a string whose bytes are all there and over the cap is oversize.
    if (std::cmp_greater(length, remaining())) {
        m_failed = true;
        return {};
    }
    if (std::cmp_greater(length, maxBytes)) {
        m_failed = true;
        m_oversize = true;
        return {};
    }
    return bytes(static_cast<qsizetype>(length));
}

QByteArray Reader::bytes(qsizetype count)
{
    const char *p = nullptr;
    if (!take(count, &p)) {
        return {};
    }
    return {p, count};
}

qsizetype Reader::count(qsizetype minItemBytes) noexcept
{
    const quint32 n = u32();
    if (m_failed) {
        return 0;
    }
    const qsizetype perItem = minItemBytes > 0 ? minItemBytes : 1;
    if (std::cmp_greater(n, remaining() / perItem)) {
        m_failed = true;
        return 0;
    }
    return static_cast<qsizetype>(n);
}

QByteArrayView Reader::rest() noexcept
{
    if (m_failed) {
        return {};
    }
    const QByteArrayView tail = m_data.sliced(m_pos);
    m_pos = m_data.size();
    return tail;
}

void Reader::skip(qsizetype count) noexcept
{
    const char *p = nullptr;
    take(count, &p);
}

void Writer::u8(quint8 value)
{
    m_data.append(static_cast<char>(value));
}

void Writer::u16(quint16 value)
{
    appendLittleEndian(m_data, value);
}

void Writer::u32(quint32 value)
{
    appendLittleEndian(m_data, value);
}

void Writer::i32(qint32 value)
{
    appendLittleEndian(m_data, value);
}

void Writer::u64(quint64 value)
{
    appendLittleEndian(m_data, value);
}

void Writer::boolean(bool value)
{
    u8(value ? 1 : 0);
}

void Writer::string(QByteArrayView raw)
{
    // Every string Selecta writes comes from a bounded message; a 4 GiB string is a programming error.
    Q_ASSERT(std::cmp_less_equal(raw.size(), std::numeric_limits<quint32>::max()));
    u32(static_cast<quint32>(raw.size()));
    m_data.append(raw);
}

void Writer::string(const QString &text)
{
    string(QByteArrayView(text.toUtf8()));
}

void Writer::bytes(QByteArrayView raw)
{
    m_data.append(raw);
}

bool isValidUtf8(QByteArrayView raw)
{
    return raw.isValidUtf8();
}

QString decodeText(QByteArrayView raw)
{
    if (!raw.isValidUtf8()) {
        return QString::fromLatin1(raw);
    }
    // A leading byte-order mark is kept as a character: names must decode to exactly what was sent.
    QStringDecoder decoder(QStringDecoder::Utf8,
                           QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    const QString decoded = decoder.decode(raw);
    return decoder.hasError() ? QString::fromLatin1(raw) : decoded;
}

QByteArray md5Hex(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex();
}

const char *decodeStatusName(DecodeStatus status)
{
    switch (status) {
    case DecodeStatus::Decoded:
        return "decoded";
    case DecodeStatus::Malformed:
        return "malformed";
    case DecodeStatus::Ignored:
        return "ignored";
    case DecodeStatus::Unknown:
        return "unknown";
    }
    return "?";
}

} // namespace selecta::slsk
