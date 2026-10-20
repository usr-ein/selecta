// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QIODevice>

namespace selecta::testing {

// Writes through to `target` until `budget` bytes have been written, then fails every write the way a full disk does
// (Qt's ENOSPC message): the disk-full simulation of the C2 tests.
class FailingDevice final : public QIODevice
{
    Q_OBJECT

public:
    FailingDevice(QIODevice &target, qint64 budget);

    bool isSequential() const override { return false; }
    bool seek(qint64 position) override;
    qint64 size() const override { return m_target.size(); }
    qint64 failedWrites() const { return m_failedWrites; }

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 size) override;

private:
    QIODevice &m_target;
    qint64 m_budget = 0;
    qint64 m_written = 0;
    qint64 m_failedWrites = 0;
};

} // namespace selecta::testing
