// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <dirent.h>
#include <sys/resource.h>
#endif

// Test helpers for the process's open-file limit (P-022): count the descriptors open now, and lower the limit for one
// test. POSIX only; on Windows nothing counts and nothing is lowered.
namespace selecta::testing {

// Descriptors open in this process (from /dev/fd), or -1 where that can't be read.
inline int openDescriptorCount()
{
#if defined(Q_OS_UNIX)
    DIR *dir = opendir("/dev/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (const dirent *entry = readdir(dir)) { // NOLINT(concurrency-mt-unsafe): its own stream, one thread
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    (void)closedir(dir);
    return count - 1; // the listing's own descriptor
#else
    return -1;
#endif
}

// Sets the soft RLIMIT_NOFILE for its lifetime, then puts the old one back. ok() is false where it couldn't (Windows).
class ScopedOpenFileLimit
{
public:
    explicit ScopedOpenFileLimit(qint64 soft)
    {
#if defined(Q_OS_UNIX)
        if (getrlimit(RLIMIT_NOFILE, &m_saved) == 0) {
            rlimit lowered = m_saved;
            lowered.rlim_cur = static_cast<rlim_t>(soft);
            m_ok = setrlimit(RLIMIT_NOFILE, &lowered) == 0;
        }
#else
        Q_UNUSED(soft)
#endif
    }
    ~ScopedOpenFileLimit()
    {
#if defined(Q_OS_UNIX)
        if (m_ok) {
            (void)setrlimit(RLIMIT_NOFILE, &m_saved);
        }
#endif
    }
    ScopedOpenFileLimit(const ScopedOpenFileLimit &) = delete;
    ScopedOpenFileLimit &operator=(const ScopedOpenFileLimit &) = delete;
    ScopedOpenFileLimit(ScopedOpenFileLimit &&) = delete;
    ScopedOpenFileLimit &operator=(ScopedOpenFileLimit &&) = delete;

    bool ok() const { return m_ok; }

private:
#if defined(Q_OS_UNIX)
    rlimit m_saved{};
#endif
    bool m_ok = false;
};

} // namespace selecta::testing
