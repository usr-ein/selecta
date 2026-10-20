// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/detail/SourceReader.h"

namespace selecta::audio::detail {

SourceReader::Status SourceReader::read(qint64 position, qint64 count, std::vector<quint8> &out)
{
    if (position < 0 || count < 0 || position > m_snapshot.available || count > m_snapshot.available - position) {
        return Status::NotAvailable;
    }
    out.resize(static_cast<std::size_t>(count));
    if (count == 0) {
        return Status::Ok;
    }
    const qint64 got = m_source.read(position, reinterpret_cast<char *>(out.data()), count);
    if (got < 0) {
        return Status::Error;
    }
    if (got < count) {
        return Status::NotAvailable;
    }
    return Status::Ok;
}

} // namespace selecta::audio::detail
