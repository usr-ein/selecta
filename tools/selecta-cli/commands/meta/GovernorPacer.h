// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Governor.h"
#include "meta/RequestPacer.h"

#include <QObject>
#include <QPointer>

namespace selecta::cli {

// meta's RequestPacer over a real governor::Governor, for selecta-cli's meta commands (the app's core/ has its own).
// Each acquire() asks the governor once and hands the decision over on the next turn of `context`'s event loop, never
// inside acquire(); a refusal carries the governor's explanation and retry-after. With `verbose`, report() prints each
// request's activity-log line (targets arrive scrubbed: no token, secret or Client ID) to stderr.
//
// Threading: `context`'s thread only. The governor and context must outlive the pacer.
class GovernorPacer final : public meta::RequestPacer
{
public:
    GovernorPacer(governor::Governor &governor, QObject *context, bool verbose);

    void acquire(governor::Operation operation, Grant grant) override;
    void report(const meta::RequestRecord &record) override;

private:
    governor::Governor &m_governor;
    QPointer<QObject> m_context;
    bool m_verbose;
};

} // namespace selecta::cli
