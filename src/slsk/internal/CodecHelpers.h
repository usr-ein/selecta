// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Codec.h"

#include <QStringList>

#include <optional>
#include <type_traits>
#include <utility>

// Shared plumbing for the message codecs. Private to slsk.
namespace selecta::slsk::internal {

// Decodes a T from `payload` with `fill(reader, message)`. Malformed (nullopt) when any read failed or `fill` called
// reader.fail(). Trailing bytes are ignored.
template <typename T, typename Fill>
std::optional<T> parse(QByteArrayView payload, Fill &&fill)
{
    Reader reader(payload);
    T message;
    std::forward<Fill>(fill)(reader, message);
    if (!reader.ok()) {
        return std::nullopt;
    }
    return message;
}

template <typename Write>
QByteArray encodeWith(Write &&write)
{
    Writer writer;
    std::forward<Write>(write)(writer);
    return writer.take();
}

inline QString readText(Reader &reader)
{
    return decodeText(reader.string());
}

// An optional trailing field: read only if bytes remain, and only kept if it parses. Bytes that don't parse as the
// field are extra trailing bytes, which are ignored rather than failing the message (protocol §6.2).
template <typename T, typename Read>
std::optional<T> readOptional(Reader &reader, Read &&read)
{
    if (!reader.ok() || reader.atEnd()) {
        return std::nullopt;
    }
    Reader probe = reader; // NOLINT(misc-const-correctness): read() advances it
    T value = std::forward<Read>(read)(probe);
    if (!probe.ok()) {
        return std::nullopt;
    }
    reader = probe;
    return value;
}

inline QStringList readStringList(Reader &reader)
{
    const qsizetype n = reader.count(4);
    QStringList list;
    list.reserve(n);
    for (qsizetype i = 0; i < n && reader.ok(); ++i) {
        list.append(readText(reader));
    }
    return list;
}

inline void writeStringList(Writer &writer, const QStringList &list)
{
    writer.u32(static_cast<quint32>(list.size()));
    for (const QString &item : list) {
        writer.string(item);
    }
}

inline void writeValue(Writer &writer, const QString &value)
{
    writer.string(value);
}

inline void writeValue(Writer &writer, quint32 value)
{
    writer.u32(value);
}

inline void writeValue(Writer &writer, bool value)
{
    writer.boolean(value);
}

template <typename V>
V readValue(Reader &reader)
{
    if constexpr (std::is_same_v<V, QString>) {
        return readText(reader);
    } else if constexpr (std::is_same_v<V, quint32>) {
        return reader.u32();
    } else {
        static_assert(std::is_same_v<V, bool>, "readValue: unsupported field type");
        return reader.boolean();
    }
}

// Messages made of a single string, uint32 or bool field: `Field` is a pointer to that member.
template <auto Field, typename T>
QByteArray encodeSingle(const T &message)
{
    return encodeWith([&message](Writer &writer) { writeValue(writer, message.*Field); });
}

template <typename T, auto Field>
std::optional<T> decodeSingle(QByteArrayView payload)
{
    using V = std::remove_cvref_t<decltype(std::declval<T &>().*Field)>;
    return parse<T>(payload, [](Reader &reader, T &message) { message.*Field = readValue<V>(reader); });
}

// One alternative of a decoded-message variant.
template <typename Variant, typename T>
Decoded<Variant> decodeAs(QByteArrayView payload)
{
    std::optional<T> message = T::decode(payload);
    if (!message) {
        return {DecodeStatus::Malformed, std::nullopt};
    }
    return {DecodeStatus::Decoded, Variant(std::in_place_type<T>, std::move(*message))};
}

} // namespace selecta::slsk::internal
