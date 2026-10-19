// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QPromise>

#include <functional>
#include <memory>
#include <utility>

// Internal helpers that turn meta's callback plumbing into the QFuture<Result<T>> its public API returns.
namespace selecta::meta::detail {

// A QPromise that callbacks can share (QPromise is move-only, std::function needs copies). Finishing twice is
// harmless: the first value wins. If every copy is destroyed unfinished, QPromise's destructor cancels the future,
// so a dropped operation never leaves a future pending forever.
template <typename T>
class SharedPromise
{
public:
    SharedPromise()
        : m_state(std::make_shared<State>())
    {
        m_state->promise.start();
    }

    QFuture<T> future() const { return m_state->promise.future(); }
    bool isCanceled() const { return m_state->promise.isCanceled(); }
    bool isFinished() const { return m_state->finished; }

    // Finishes now with `value` (dropped if the caller already cancelled).
    void finish(T value) const
    {
        if (m_state->finished) {
            return;
        }
        m_state->finished = true;
        if (!m_state->promise.isCanceled()) {
            m_state->promise.addResult(std::move(value));
        }
        m_state->promise.finish();
    }

    // Ends the future canceled (no result), unless it already finished.
    void cancel() const
    {
        if (m_state->finished) {
            return;
        }
        m_state->finished = true;
        m_state->promise.future().cancel();
        m_state->promise.finish();
    }

    // Finishes on the next turn of `context`'s event loop, so the caller never sees a re-entrant continuation. If
    // `context` is gone by then, the future ends canceled.
    void finishLater(QObject *context, T value) const
    {
        const SharedPromise self = *this;
        QMetaObject::invokeMethod(
            context, [self, value = std::move(value)]() mutable { self.finish(std::move(value)); },
            Qt::QueuedConnection);
    }

    // Calls `onCancel` (on `context`'s thread) when the caller cancels the future before it finishes.
    void onCanceled(QObject *context, std::function<void()> onCancel) const
    {
        auto *watcher = new QFutureWatcher<T>(context);
        QObject::connect(watcher, &QFutureWatcherBase::canceled, context, std::move(onCancel));
        QObject::connect(watcher, &QFutureWatcherBase::finished, watcher, &QObject::deleteLater);
        watcher->setFuture(future());
    }

private:
    struct State
    {
        QPromise<T> promise;
        bool finished = false;
    };
    std::shared_ptr<State> m_state;
};

// A future that finishes with `value` on the next turn of `context`'s event loop.
template <typename T>
QFuture<T> deferred(QObject *context, T value)
{
    const SharedPromise<T> promise;
    promise.finishLater(context, std::move(value));
    return promise.future();
}

// Lets a multi-step operation cancel whatever step is running when its future is cancelled. Copies share state.
class CancelSlot
{
public:
    CancelSlot()
        : m_state(std::make_shared<State>())
    {}

    // Replaces the action that cancels the current step.
    void set(std::function<void()> cancelStep) const { m_state->cancelStep = std::move(cancelStep); }
    void clear() const { m_state->cancelStep = nullptr; }
    bool isCancelled() const { return m_state->cancelled; }

    void cancel() const
    {
        if (m_state->cancelled) {
            return;
        }
        m_state->cancelled = true;
        if (const auto step = std::exchange(m_state->cancelStep, nullptr)) {
            step();
        }
    }

private:
    struct State
    {
        std::function<void()> cancelStep;
        bool cancelled = false;
    };
    std::shared_ptr<State> m_state;
};

// Calls `then(result)` on `context`'s thread when `future` finishes with a result; does nothing if it was cancelled
// or `context` is gone.
template <typename T, typename F>
void whenReady(const QFuture<T> &future, QObject *context, F then)
{
    auto *watcher = new QFutureWatcher<T>(context); // a child of `context`; deletes itself once finished
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [future, then = std::move(then)]() mutable {
        if (future.isCanceled() || future.resultCount() == 0) {
            return;
        }
        then(future.result());
    });
    QObject::connect(watcher, &QFutureWatcherBase::finished, watcher, &QObject::deleteLater);
    watcher->setFuture(future);
}

} // namespace selecta::meta::detail
