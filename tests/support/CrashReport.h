// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <typeinfo>

#if defined(__APPLE__) || defined(__linux__)
#include <csignal>
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
    (void)std::fputs("\n*** std::terminate", stderr);
#if defined(__APPLE__) || defined(__linux__)
    if (const std::type_info *type = abi::__cxa_current_exception_type()) {
        (void)std::fputs(" (uncaught ", stderr);
        (void)std::fputs(type->name(), stderr);
        (void)std::fputs(")", stderr);
    }
    (void)std::fputs(", backtrace:\n", stderr);
    std::array<void *, 64> frames{};
    const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
    backtrace_symbols_fd(frames.data(), count, STDERR_FILENO);
#else
    (void)std::fputs("\n", stderr);
#endif
    std::abort();
}

#if defined(__APPLE__) || defined(__linux__)
// A crash (segmentation fault, bus error) prints its backtrace too, then dies of the same signal. backtrace() isn't
// formally async-signal-safe, but this only ever runs in a test binary that is about to die anyway.
inline void reportCrash(int signal)
{
    const char *name = "\n*** SIGILL";
    if (signal == SIGSEGV) {
        name = "\n*** SIGSEGV";
    } else if (signal == SIGBUS) {
        name = "\n*** SIGBUS";
    }
    (void)write(STDERR_FILENO, name, std::strlen(name));
    (void)write(STDERR_FILENO, ", backtrace:\n", 13);
    std::array<void *, 64> frames{};
    const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
    backtrace_symbols_fd(frames.data(), count, STDERR_FILENO);
    (void)std::signal(signal, SIG_DFL);
    (void)std::raise(signal);
}
#endif

inline void installCrashReport()
{
    std::set_terminate(&reportTermination);
#if defined(__APPLE__) || defined(__linux__)
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL}) {
        (void)std::signal(signal, &reportCrash);
    }
#endif
}

} // namespace selecta::testing
