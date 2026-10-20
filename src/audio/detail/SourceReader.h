// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/ByteSource.h"
#include "audio/detail/Bytes.h"

#include <QtGlobal>

#include <vector>

namespace selecta::audio::detail {

// Reads byte ranges from a ByteSource against one snapshot, so a parse sees a consistent prefix of the file.
class SourceReader
{
public:
    enum class Status { Ok, NotAvailable, Error };

    SourceReader(ByteSource &source, SourceSnapshot snapshot)
        : m_source(source)
        , m_snapshot(snapshot)
    {}

    qint64 available() const { return m_snapshot.available; }
    bool isComplete() const { return m_snapshot.isComplete(); }
    SourceState state() const { return m_snapshot.state; }
    qint64 totalSize() const { return m_source.totalSize(); }
    // The size the file will have: total for complete or growing sources.
    qint64 endOffset() const { return isComplete() ? m_snapshot.available : m_source.totalSize(); }

    // Reads exactly `count` bytes at `position` into `out`. NotAvailable when they extend beyond the snapshot's
    // available count (or the file end); Error on an I/O failure (see ByteSource::lastError()).
    Status read(qint64 position, qint64 count, std::vector<quint8> &out);

    ByteSource &source() { return m_source; }

private:
    ByteSource &m_source;
    SourceSnapshot m_snapshot;
};

} // namespace selecta::audio::detail
