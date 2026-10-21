// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtTypes>

#include <limits>
#include <optional>

// The process's limit on open files (P-022). Every socket, file and pipe takes one descriptor; when none is left,
// sockets fail with "Out of resources" and Qt aborts as it starts a thread. A macOS app gets 256 unless it asks for
// more, so Selecta raises the limit at start-up and sizes its peer socket cap to the limit it got.
//
// Threading: any thread (getrlimit and setrlimit are thread-safe). Failure modes: a limit that can't be raised stays
// as it was, and the functions return the limit in force; a limit that can't be read is taken as 256 (macOS's
// default) so that callers stay conservative. Nothing throws.
namespace selecta::base {

// No limit that sockets count against: Windows (sockets are handles there), or an unlimited soft limit.
inline constexpr qint64 kNoOpenFileLimit = std::numeric_limits<qint64>::max();

// The most descriptors this process may have open at once: the soft RLIMIT_NOFILE.
qint64 openFileLimit();

// Raises that limit to `wanted`, or as far as the hard limit and the system allow; never lowers it. Returns the limit
// in force afterwards. Call it at start-up, before anything sizes itself to the limit.
qint64 raiseOpenFileLimit(qint64 wanted);

namespace detail {

// The soft limit that raises `soft` towards `wanted` without passing `hard` or the system's `ceiling` (either may be
// kNoOpenFileLimit); nullopt when it wouldn't go up.
std::optional<qint64> raisedOpenFileLimit(qint64 soft, qint64 hard, qint64 ceiling, qint64 wanted);

} // namespace detail

} // namespace selecta::base
