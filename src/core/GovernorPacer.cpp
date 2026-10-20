// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/GovernorPacer.h"

#include "core/ActivityLog.h"
#include "governor/Governor.h"

#include <QLocale>
#include <QObject>

#include <memory>

using namespace Qt::StringLiterals;

namespace selecta::core {

GovernorPacer::GovernorPacer(governor::Governor &governor, ActivityLog &activity, QObject *context)
    : m_governor(governor)
    , m_activity(activity)
    , m_context(context)
{}

void GovernorPacer::acquire(governor::Operation operation, Grant grant)
{
    governor::Decision decision = m_governor.request(operation, {});
    // A Permit is move-only; the shared holder lets the queued call stay copyable.
    const auto answer = std::make_shared<meta::PermitDecision>();
    answer->permit = std::move(decision.permit);
    if (!answer->permit) {
        answer->refusal = decision.explanation;
        answer->retryAfter = decision.retryAfter;
    }
    if (!m_context) {
        return;
    }
    QMetaObject::invokeMethod(
        m_context.data(), [grant = std::move(grant), answer] { grant(std::move(*answer)); }, Qt::QueuedConnection);
}

void GovernorPacer::report(const meta::RequestRecord &record)
{
    QStringList details;
    if (record.httpStatus > 0) {
        details.append(u"HTTP %1"_s.arg(record.httpStatus));
    }
    if (record.bytes > 0) {
        details.append(QLocale().formattedDataSize(record.bytes));
    }
    details.append(u"%1 ms"_s.arg(record.elapsed.count()));
    const QString subject =
        record.method.isEmpty() ? record.target : QString::fromLatin1(record.method) + u' ' + record.target;
    m_activity.record(meta::serviceName(record.service).toLower(), subject,
                      record.outcome.isEmpty() ? u"ok"_s : record.outcome, details.join(u" · "_s));
}

} // namespace selecta::core
