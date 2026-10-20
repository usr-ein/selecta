// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/Crc.h"

#include <array>

namespace selecta::audio::detail {
namespace {

constexpr std::array<quint8, 256> makeCrc8Table()
{
    std::array<quint8, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0 ? ((crc << 1U) ^ 0x07U) : (crc << 1U);
        }
        table.at(i) = static_cast<quint8>(crc & 0xFFU);
    }
    return table;
}

constexpr std::array<quint16, 256> makeCrc16FlacTable()
{
    std::array<quint16, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned crc = i << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0 ? ((crc << 1U) ^ 0x8005U) : (crc << 1U);
        }
        table.at(i) = static_cast<quint16>(crc & 0xFFFFU);
    }
    return table;
}

constexpr std::array<quint16, 256> makeCrc16ArcTable()
{
    std::array<quint16, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? ((crc >> 1U) ^ 0xA001U) : (crc >> 1U);
        }
        table.at(i) = static_cast<quint16>(crc & 0xFFFFU);
    }
    return table;
}

constexpr std::array<quint8, 256> kCrc8 = makeCrc8Table();
constexpr std::array<quint16, 256> kCrc16Flac = makeCrc16FlacTable();
constexpr std::array<quint16, 256> kCrc16Arc = makeCrc16ArcTable();

} // namespace

quint8 crc8Flac(ByteSpan bytes)
{
    unsigned crc = 0;
    for (const quint8 byte : bytes) {
        crc = kCrc8[(crc ^ byte) & 0xFFU];
    }
    return static_cast<quint8>(crc);
}

quint16 crc16Flac(ByteSpan bytes, quint16 initial)
{
    unsigned crc = initial;
    for (const quint8 byte : bytes) {
        crc = ((crc << 8U) ^ kCrc16Flac[((crc >> 8U) ^ byte) & 0xFFU]) & 0xFFFFU;
    }
    return static_cast<quint16>(crc);
}

quint16 crc16Lame(ByteSpan bytes, quint16 initial)
{
    unsigned crc = initial;
    for (const quint8 byte : bytes) {
        crc = (crc >> 8U) ^ kCrc16Arc[(crc ^ byte) & 0xFFU];
    }
    return static_cast<quint16>(crc);
}

} // namespace selecta::audio::detail
