// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <execinfo.h>
#include <unistd.h>
#endif

// Test binaries print where an uncaught exception came from before they abort: the debugger can't always attach in
// automated runs, and "terminating due to uncaught exception" alone says nothing about the throw site. libc++abi
// calls the terminate handler before unwinding when no handler exists, so the stack still shows the throw.
namespace selecta::testing {

[[noreturn]] inline void reportTermination()
{
    std::fputs("\n*** std::terminate", stderr);
#if defined(__APPLE__) || defined(__linux__)
    if (const std::type_info *type = abi::__cxa_current_exception_type()) {
        std::fputs(" (uncaught ", stderr);
        std::fputs(type->name(), stderr);
        std::fputs(")", stderr);
    }
    std::fputs(", backtrace:\n", stderr);
    void *frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#else
    std::fputs("\n", stderr);
#endif
    std::abort();
}

inline void installCrashReport()
{
    std::set_terminate(&reportTermination);
}

} // namespace selecta::testing
