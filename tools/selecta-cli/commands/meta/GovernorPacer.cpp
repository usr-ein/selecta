// SPDX-License-Identifier: GPL-3.0-or-later
#include "GovernorPacer.h"

#include "Command.h"

#include <QMetaObject>

#include <cstdio>
#include <memory>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::cli {

GovernorPacer::GovernorPacer(governor::Governor &governor, QObject *context, bool verbose)
    : m_governor(governor)
    , m_context(context)
    , m_verbose(verbose)
{}

void GovernorPacer::acquire(governor::Operation operation, Grant grant)
{
    governor::Decision decision = m_governor.request(operation, {});
    const auto answer = std::make_shared<meta::PermitDecision>();
    answer->permit = std::move(decision.permit);
    if (!answer->permit.has_value()) {
        answer->refusal = decision.explanation;
        answer->retryAfter = std::chrono::duration_cast<std::chrono::milliseconds>(decision.retryAfter);
    }
    if (m_context.isNull()) {
        return; // shutting down: the request is dropped with its permit
    }
    QMetaObject::invokeMethod(
        m_context.data(), [grant = std::move(grant), answer]() mutable { grant(std::move(*answer)); },
        Qt::QueuedConnection);
}

void GovernorPacer::report(const meta::RequestRecord &record)
{
    if (!m_verbose) {
        return;
    }
    QString line =
        u"  · %1 %2 %3"_s.arg(meta::serviceName(record.service), QString::fromLatin1(record.method), record.target);
    if (record.httpStatus != 0) {
        line += u" → HTTP %1"_s.arg(record.httpStatus);
    }
    line += u", %1 bytes, %2 ms: %3"_s.arg(record.bytes).arg(record.elapsed.count()).arg(record.outcome);
    printLine(stderr, line);
}

} // namespace selecta::cli
