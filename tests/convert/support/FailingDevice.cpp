// SPDX-License-Identifier: GPL-3.0-or-later
#include "FailingDevice.h"

#include "convert/detail/OutputFile.h"

namespace selecta::testing {

FailingDevice::FailingDevice(QIODevice &target, qint64 budget)
    : m_target(target)
    , m_budget(budget)
{
    QIODevice::open(QIODevice::WriteOnly | QIODevice::Unbuffered);
}

bool FailingDevice::seek(qint64 position)
{
    return QIODevice::seek(position) && m_target.seek(position);
}

qint64 FailingDevice::readData(char * /*data*/, qint64 /*maxSize*/)
{
    return -1;
}

qint64 FailingDevice::writeData(const char *data, qint64 size)
{
    if (m_written + size > m_budget) {
        ++m_failedWrites;
        setErrorString(selecta::convert::detail::noSpaceMessage());
        return -1;
    }
    const qint64 written = m_target.write(data, size);
    if (written > 0) {
        m_written += written;
    }
    return written;
}

} // namespace selecta::testing
