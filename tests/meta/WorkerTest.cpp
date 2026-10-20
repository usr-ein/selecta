// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/Worker.h"

#include "meta/support/Await.h"

#include <QThread>

#include <atomic>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using selecta::meta::detail::Worker;
using selecta::meta::testing::spinUntil;

TEST(Worker, RunsWorkOffThreadAndCompletesOnTheOwnersThread)
{
    Worker worker(QStringLiteral("test-worker"));
    std::atomic<bool> offThread = false;
    QThread *owner = QThread::currentThread();
    bool completedOnOwner = false;
    int result = 0;
    worker.run<int>(
        [&] {
            offThread = QThread::currentThread() != owner;
            return 42;
        },
        [&](int value) {
            completedOnOwner = QThread::currentThread() == owner;
            result = value;
        });
    ASSERT_TRUE(spinUntil([&] { return result == 42; }));
    EXPECT_TRUE(offThread);
    EXPECT_TRUE(completedOnOwner);
}

TEST(Worker, KeepsTheOrderOfTasksAndCompletions)
{
    Worker worker(QStringLiteral("test-worker"));
    std::vector<int> ran; // touched by the worker only, read after the completions
    std::vector<int> completed;
    for (int i = 0; i < 100; ++i) {
        worker.run<int>(
            [&ran, i] {
                ran.push_back(i);
                return i;
            },
            [&completed](int value) { completed.push_back(value); });
    }
    ASSERT_TRUE(spinUntil([&] { return completed.size() == 100; }));
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(completed.at(static_cast<std::size_t>(i)), i);
    }
    EXPECT_EQ(ran.size(), 100U);
}

TEST(Worker, DestructionDropsQueuedWorkAndCompletions)
{
    const auto completions = std::make_shared<std::atomic<int>>(0);
    const auto started = std::make_shared<std::atomic<int>>(0);
    {
        Worker worker(QStringLiteral("test-worker"));
        for (int i = 0; i < 50; ++i) {
            worker.run<int>(
                [started] {
                    ++*started;
                    QThread::msleep(2);
                    return 0;
                },
                [completions](int) { ++*completions; });
        }
        QThread::msleep(5);
    } // destroyed with most tasks still queued: no crash, no hang
    QTest::qWait(20);
    EXPECT_LT(started->load(), 50);
    EXPECT_EQ(completions->load(), 0); // completions never ran: nobody spun the event loop before destruction
}

TEST(Worker, PostWithoutCompletion)
{
    Worker worker(QStringLiteral("test-worker"));
    auto done = std::make_shared<std::atomic<bool>>(false);
    worker.post([done] { *done = true; });
    ASSERT_TRUE(spinUntil([&] { return done->load(); }));
}
