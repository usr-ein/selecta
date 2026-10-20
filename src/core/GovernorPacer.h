// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/RequestPacer.h"

#include <QPointer>

class QObject;

namespace selecta::governor {
class Governor;
}

namespace selecta::core {

class ActivityLog;

// meta's requests through the network governor (ADR-006): acquire() asks the Governor for the operation's permit and
// hands the answer back on a later event-loop turn; report() writes every finished request to the activity log
// (SAFE-4, META-7). Request targets arrive already scrubbed of tokens and secrets.
//
// Threading: engine thread, like the Governor and the meta objects. Grants are queued through `context`; if it is gone
// by then, the grant is dropped with its unused permit, which is harmless.
class GovernorPacer final : public meta::RequestPacer
{
public:
    GovernorPacer(governor::Governor &governor, ActivityLog &activity, QObject *context);

    void acquire(governor::Operation operation, Grant grant) override;
    void report(const meta::RequestRecord &record) override;

private:
    governor::Governor &m_governor;
    ActivityLog &m_activity;
    QPointer<QObject> m_context;
};

} // namespace selecta::core
