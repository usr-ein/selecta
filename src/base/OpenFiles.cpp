// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/OpenFiles.h"

#include <QtGlobal>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <climits> // OPEN_MAX on Darwin
#include <sys/resource.h>
#endif

namespace selecta::base {
namespace {

#if defined(Q_OS_UNIX)
// macOS's default for an app: what's assumed when the limit can't be read.
constexpr qint64 kAssumedOpenFiles = 256;

qint64 fromRlimit(rlim_t value)
{
    if (value == RLIM_INFINITY || value > static_cast<rlim_t>(kNoOpenFileLimit)) {
        return kNoOpenFileLimit;
    }
    return static_cast<qint64>(value);
}

rlim_t toRlimit(qint64 value)
{
    return value == kNoOpenFileLimit ? RLIM_INFINITY : static_cast<rlim_t>(value);
}

// The most a soft RLIMIT_NOFILE may be, whatever the hard limit says.
qint64 systemCeiling()
{
#if defined(Q_OS_DARWIN)
    return OPEN_MAX; // setrlimit(2): "use rlim_cur = min(OPEN_MAX, rlim_max)"
#else
    return kNoOpenFileLimit; // the hard limit is the bound
#endif
}
#endif

} // namespace

std::optional<qint64> detail::raisedOpenFileLimit(qint64 soft, qint64 hard, qint64 ceiling, qint64 wanted)
{
    const qint64 target = std::min({wanted, hard, ceiling});
    if (target <= soft) {
        return std::nullopt;
    }
    return target;
}

qint64 openFileLimit()
{
#if defined(Q_OS_UNIX)
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return kAssumedOpenFiles;
    }
    return fromRlimit(limit.rlim_cur);
#else
    return kNoOpenFileLimit;
#endif
}

qint64 raiseOpenFileLimit(qint64 wanted)
{
#if defined(Q_OS_UNIX)
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        return kAssumedOpenFiles;
    }
    const std::optional<qint64> raised =
        detail::raisedOpenFileLimit(fromRlimit(limit.rlim_cur), fromRlimit(limit.rlim_max), systemCeiling(), wanted);
    if (raised) {
        rlimit next = limit;
        next.rlim_cur = toRlimit(*raised);
        if (setrlimit(RLIMIT_NOFILE, &next) != 0) {
            // Refused (a sandbox, say): the old limit stays in force, and the caller sizes itself to it.
        }
    }
    return openFileLimit();
#else
    Q_UNUSED(wanted)
    return kNoOpenFileLimit;
#endif
}

} // namespace selecta::base
