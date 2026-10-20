// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRunnable>
#include <QThreadPool>

#include <memory>
#include <utility>

// ThreadSanitizer support. Qt is a static library built without ThreadSanitizer, so the synchronisation inside
// QThreadPool (handing a task to a pool thread) is invisible to it, and it would report the task's captured state as a
// data race. startTask() marks the hand-over explicitly in TSan builds; in other builds it is a plain
// QThreadPool::start. The media libraries use std::mutex and std::condition_variable, which ThreadSanitizer
// understands, for everything else.
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define SELECTA_THREAD_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(SELECTA_THREAD_SANITIZER)
#define SELECTA_THREAD_SANITIZER 1
#endif

#ifdef SELECTA_THREAD_SANITIZER
extern "C" void __tsan_acquire(void *address); // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" void __tsan_release(void *address); // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#endif

namespace selecta::audio::detail {

inline void handOverRelease([[maybe_unused]] const void *token)
{
#ifdef SELECTA_THREAD_SANITIZER
    __tsan_release(const_cast<void *>(token)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
#endif
}

inline void handOverAcquire([[maybe_unused]] const void *token)
{
#ifdef SELECTA_THREAD_SANITIZER
    __tsan_acquire(const_cast<void *>(token)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
#endif
}

// The address both sides of every hand-over use. It must be known without reading the task (whose captured state is
// what the hand-over publishes), so it is one fixed object: an acquire then orders the task after every earlier
// release, slightly more than needed, which can only hide races, never invent them.
inline void *handOverToken()
{
    static char token = 0;
    return &token;
}

// Starts `task` on `pool`, with everything the task captured visible to ThreadSanitizer as happening before it runs.
template <typename Task>
void startTask(QThreadPool *pool, Task task)
{
    QRunnable *runnable = QRunnable::create([task = std::move(task)]() mutable {
        handOverAcquire(handOverToken());
        task();
    });
    handOverRelease(handOverToken());
    pool->start(runnable);
}

} // namespace selecta::audio::detail
