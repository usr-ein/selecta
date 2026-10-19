// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "governor/Permit.h"

// Test-only source of permits, so library tests can call budgeted operations without a Governor. Never include this
// from src/: the app must get every permit from the Governor.
namespace selecta::governor::testing {

class PermitFactory
{
public:
    static Permit make(Operation operation) { return Permit(operation); }
};

} // namespace selecta::governor::testing
