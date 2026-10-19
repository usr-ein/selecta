// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/RequestPacer.h"
#include "support/PermitFactory.h"

#include <QList>
#include <QTimer>

#include <chrono>
#include <deque>
#include <utility>

namespace selecta::meta::testing {

// A RequestPacer for tests: grants permits from the test-only PermitFactory on the next event-loop turn (never inside
// acquire()), or refuses as told, and records every acquire() and report().
class FakePacer final : public RequestPacer
{
public:
    void acquire(governor::Operation operation, Grant grant) override
    {
        m_acquired.append(operation);
        if (m_manual) {
            m_pending.emplace_back(operation, std::move(grant));
            return;
        }
        PermitDecision decision = decide(operation);
        const auto shared = std::make_shared<PermitDecision>(std::move(decision));
        QTimer::singleShot(0, &m_context, [grant = std::move(grant), shared]() mutable { grant(std::move(*shared)); });
    }

    void report(const RequestRecord &record) override { m_records.append(record); }

    // The next `times` requests are refused (all of them when negative), with an optional retry-after.
    void refuse(QString reason, std::chrono::milliseconds retryAfter = std::chrono::milliseconds(0), int times = -1)
    {
        m_refusal = std::move(reason);
        m_refusalRetryAfter = retryAfter;
        m_refusalsLeft = times;
    }
    void stopRefusing() { m_refusalsLeft = 0; }

    // Manual mode: acquire() parks the grant until grantPending().
    void setManual(bool manual) { m_manual = manual; }
    int pendingCount() const { return static_cast<int>(m_pending.size()); }
    void grantPending()
    {
        auto pending = std::exchange(m_pending, {});
        for (auto &[operation, grant] : pending) {
            grant(decide(operation));
        }
    }

    const QList<governor::Operation> &acquired() const { return m_acquired; }
    int acquiredCount() const { return static_cast<int>(m_acquired.size()); }
    const QList<RequestRecord> &records() const { return m_records; }

private:
    PermitDecision decide(governor::Operation operation)
    {
        PermitDecision decision;
        if (m_refusalsLeft != 0) {
            if (m_refusalsLeft > 0) {
                --m_refusalsLeft;
            }
            decision.refusal = m_refusal;
            decision.retryAfter = m_refusalRetryAfter;
            return decision;
        }
        decision.permit = governor::testing::PermitFactory::make(operation);
        return decision;
    }

    QObject m_context; // owns the queued grants: none fires once the pacer is gone
    QList<governor::Operation> m_acquired;
    QList<RequestRecord> m_records;
    QString m_refusal;
    std::chrono::milliseconds m_refusalRetryAfter{0};
    int m_refusalsLeft = 0;
    bool m_manual = false;
    std::deque<std::pair<governor::Operation, Grant>> m_pending;
};

} // namespace selecta::meta::testing
