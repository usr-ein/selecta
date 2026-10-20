// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/Worker.h"

#include <QMetaObject>

#include <pthread.h>

namespace selecta::meta::detail {

Worker::Worker(const QString &name, QObject *parent)
    : QObject(parent)
{
    setObjectName(name);
    m_thread = std::thread([this, threadName = name.toUtf8()] {
#ifdef Q_OS_MACOS
        pthread_setname_np(threadName.constData());
#endif
        loop();
    });
}

Worker::~Worker()
{
    {
        const std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_tasks.clear();
        m_completions.clear();
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Worker::post(std::function<void()> task)
{
    {
        const std::scoped_lock lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_tasks.push_back(std::move(task));
    }
    m_wake.notify_one();
}

void Worker::loop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
            if (m_stopping) {
                return;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        task();
    }
}

void Worker::complete(std::function<void()> completion)
{
    bool ring = false;
    {
        const std::scoped_lock lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_completions.push_back(std::move(completion));
        ring = !m_doorbellPending;
        m_doorbellPending = true;
    }
    if (ring) {
        // By name: Qt allocates the call event itself, so no memory of ours travels through Qt's queue.
        QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection);
    }
}

void Worker::drain()
{
    std::deque<std::function<void()>> completions;
    {
        const std::scoped_lock lock(m_mutex);
        completions.swap(m_completions);
        m_doorbellPending = false;
    }
    for (const auto &completion : completions) {
        completion();
    }
}

} // namespace selecta::meta::detail
