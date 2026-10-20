// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/detail/PcmHash.h"

#include <QSysInfo>
#include <QtEndian>

#include <cstring>
#include <vector>

namespace selecta::convert::detail {

void addFloatSamples(QCryptographicHash &hash, const float *samples, qint64 count)
{
    if (count <= 0) {
        return;
    }
    const qint64 bytes = count * qint64{sizeof(float)};
    if constexpr (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(samples), bytes));
    } else {
        std::vector<char> little(static_cast<std::size_t>(bytes));
        for (qint64 i = 0; i < count; ++i) {
            quint32 bits = 0;
            std::memcpy(&bits, samples + i, sizeof(bits));
            qToLittleEndian(bits, little.data() + (i * qint64{sizeof(bits)}));
        }
        hash.addData(QByteArrayView(little.data(), bytes));
    }
}

} // namespace selecta::convert::detail
