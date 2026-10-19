// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/Clock.h"

namespace selecta::base {

Clock::~Clock() = default;

Clock::TimePoint SteadyClock::now() const
{
    return std::chrono::time_point_cast<Duration>(std::chrono::steady_clock::now());
}

} // namespace selecta::base
