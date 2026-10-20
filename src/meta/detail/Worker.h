// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace selecta::meta::detail {

// One background thread for work that may block (the cover cache folder, the Keychain): tasks run in order on the
// worker thread; their completions run on the thread the Worker lives on, from its event loop.
//
// Every piece of data crosses threads under one std::mutex: the task queue on the way in, the completion queue on the
// way out. Qt only rings a data-free doorbell (a queued call by name), so the hand-offs are visible to
// ThreadSanitizer, which can't see the locks inside an uninstrumented Qt.
//
// Threading: post() may be called from any thread; completions run on the Worker's thread. Destruction (on the
// Worker's thread) drops queued tasks and completions, waits for the task that is running (a Keychain prompt can make
// that wait long), and joins the thread.
class Worker final : public QObject
{
    Q_OBJECT

public:
    explicit Worker(const QString &name, QObject *parent = nullptr);
    ~Worker() override;
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    Worker(Worker &&) = delete;
    Worker &operator=(Worker &&) = delete;

    // Runs `work` on the worker thread, then `done(result)` on the Worker's thread. Neither runs once the Worker is
    // being destroyed.
    template <typename T>
    void run(std::function<T()> work, std::function<void(T)> done)
    {
        const auto job = std::make_shared<Job<T>>(Job<T>{std::move(work), std::move(done)});
        post([this, job] {
            const auto result = std::make_shared<T>(job->work());
            complete([job, result] { job->done(std::move(*result)); });
        });
    }

    // Runs `task` on the worker thread (no completion).
    void post(std::function<void()> task);

private Q_SLOTS:
    void drain();

private:
    template <typename T>
    struct Job
    {
        std::function<T()> work;
        std::function<void(T)> done;
    };

    void loop();
    void complete(std::function<void()> completion); // from the worker thread

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_tasks;       // guarded by m_mutex
    std::deque<std::function<void()>> m_completions; // guarded by m_mutex
    bool m_doorbellPending = false;                  // guarded by m_mutex
    bool m_stopping = false;                         // guarded by m_mutex
    std::thread m_thread;
};

} // namespace selecta::meta::detail
