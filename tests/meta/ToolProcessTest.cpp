// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/ToolProcess.h"

#include "meta/support/Await.h"

#include <cerrno>
#include <csignal>
#include <gtest/gtest.h>
#include <memory>
#include <sys/types.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using selecta::meta::detail::ToolOutcome;
using selecta::meta::detail::ToolProcess;
using selecta::meta::testing::spinUntil;

namespace {

// Generous ceilings for waits (they return as soon as their condition holds), and timeouts far above a shell's
// start-up even on a loaded machine.
constexpr int kCeilingMs = 30000;

ToolProcess::Spec shell(const QString &script)
{
    ToolProcess::Spec spec;
    spec.program = u"/bin/sh"_s;
    spec.arguments = {u"-c"_s, script};
    spec.workingDirectory = u"/"_s;
    spec.environment.insert(u"PATH"_s, u"/usr/bin:/bin"_s);
    spec.totalTimeout = 30s; // generous: only the timeout tests set short ones
    return spec;
}

struct Run
{
    std::unique_ptr<ToolProcess> process;
    QList<QByteArray> out;
    QList<QByteArray> err;
    int finished = 0;
};

std::unique_ptr<Run> start(ToolProcess::Spec spec)
{
    auto run = std::make_unique<Run>();
    run->process = std::make_unique<ToolProcess>(std::move(spec));
    Run *raw = run.get();
    QObject::connect(raw->process.get(), &ToolProcess::lineReady, raw->process.get(),
                     [raw](QProcess::ProcessChannel channel, const QByteArray &line) {
                         (channel == QProcess::StandardOutput ? raw->out : raw->err).append(line);
                     });
    QObject::connect(raw->process.get(), &ToolProcess::finished, raw->process.get(), [raw] { ++raw->finished; });
    raw->process->start();
    return run;
}

bool finish(Run &run)
{
    return spinUntil([&] { return run.finished > 0; }, kCeilingMs);
}

bool processGone(pid_t pid)
{
    return spinUntil([pid] { return ::kill(pid, 0) != 0 && errno == ESRCH; }, kCeilingMs);
}

} // namespace

TEST(ToolProcess, LinesAcrossReadsAndAnUnterminatedTail)
{
    ToolProcess::Spec spec = shell(u"printf 'a\\nb'; sleep 0.05; printf 'c\\nd'; printf 'e1\\r\\ne2\\n' >&2"_s);
    spec.streamLines = true;
    const auto run = start(std::move(spec));
    ASSERT_TRUE(finish(*run));
    EXPECT_EQ(run->finished, 1);
    EXPECT_EQ(run->out, (QList<QByteArray>{"a", "bc", "d"}));
    EXPECT_EQ(run->err, (QList<QByteArray>{"e1", "e2"}));
    const ToolOutcome &outcome = run->process->outcome();
    EXPECT_EQ(outcome.end, ToolOutcome::End::Exited);
    EXPECT_EQ(outcome.exitCode, 0);
    EXPECT_EQ(outcome.standardOutput, QByteArray("a\nbc\nd"));
    EXPECT_EQ(outcome.errorLines, (QList<QByteArray>{"e1", "e2"}));
}

TEST(ToolProcess, ExitCodesCrashesAndStartFailures)
{
    const auto exited = start(shell(u"exit 3"_s));
    ASSERT_TRUE(finish(*exited));
    EXPECT_EQ(exited->process->outcome().end, ToolOutcome::End::Exited);
    EXPECT_EQ(exited->process->outcome().exitCode, 3);

    const auto crashed = start(shell(u"kill -SEGV $$"_s));
    ASSERT_TRUE(finish(*crashed));
    EXPECT_EQ(crashed->process->outcome().end, ToolOutcome::End::Crashed);

    ToolProcess::Spec missing = shell({});
    missing.program = u"/nonexistent/selecta-tool"_s;
    const auto failed = start(std::move(missing));
    EXPECT_EQ(failed->finished, 0); // never inside start()
    ASSERT_TRUE(finish(*failed));
    EXPECT_EQ(failed->process->outcome().end, ToolOutcome::End::FailedToStart);
    EXPECT_FALSE(failed->process->outcome().startError.isEmpty());
    QTest::qWait(50);
    EXPECT_EQ(failed->finished, 1);
}

TEST(ToolProcess, OutputIsBounded)
{
    ToolProcess::Spec chatty = shell(u"dd if=/dev/zero bs=1000 count=50 2>/dev/null | tr '\\000' x; sleep 5"_s);
    chatty.maxStdoutBytes = 1000;
    const auto tooMuch = start(std::move(chatty));
    ASSERT_TRUE(finish(*tooMuch));
    EXPECT_EQ(tooMuch->process->outcome().end, ToolOutcome::End::OutputTooLarge);
    EXPECT_LE(tooMuch->process->outcome().standardOutput.size(), 1000);

    const auto lines = start(shell(
        u"i=1; while [ $i -le 300 ]; do echo \"line $i\" >&2; i=$((i+1)); done; head -c 10000 /dev/zero | tr '\\000' y >&2"_s));
    ASSERT_TRUE(finish(*lines));
    const QList<QByteArray> &kept = lines->process->outcome().errorLines;
    ASSERT_EQ(kept.size(), ToolProcess::kMaxErrorLines);
    EXPECT_EQ(kept.at(kept.size() - 2), QByteArray("line 300"));
    EXPECT_EQ(kept.last().size(), ToolProcess::kMaxLineBytes); // a 10 000-byte line, cut
}

TEST(ToolProcess, TheEnvironmentIsOnlyWhatWasGiven)
{
    qputenv("SELECTA_TOOL_TEST_LEAK", "leaked");
    ToolProcess::Spec spec = shell(u"echo \"[$HOME][$SELECTA_TOOL_TEST_LEAK]\""_s);
    spec.environment.insert(u"HOME"_s, u"/somewhere"_s);
    const auto run = start(std::move(spec));
    qunsetenv("SELECTA_TOOL_TEST_LEAK");
    ASSERT_TRUE(finish(*run));
    EXPECT_EQ(run->process->outcome().standardOutput, QByteArray("[/somewhere][]\n"));
}

TEST(ToolProcess, KillAndTimeoutsEndTheWholeProcessGroup)
{
    ToolProcess::Spec spec = shell(u"sleep 30 & echo $!; wait"_s);
    spec.streamLines = true;
    const auto killed = start(spec);
    ASSERT_TRUE(spinUntil([&] { return !killed->out.isEmpty(); }, kCeilingMs));
    const auto child = static_cast<pid_t>(killed->out.first().toInt());
    ASSERT_GT(child, 0);
    killed->process->kill();
    ASSERT_TRUE(finish(*killed));
    EXPECT_EQ(killed->process->outcome().end, ToolOutcome::End::Killed);
    EXPECT_TRUE(processGone(child));

    ToolProcess::Spec idle = spec;
    idle.idleTimeout = 2s; // counted from the child's pid line, the last output
    const auto stalled = start(idle);
    ASSERT_TRUE(finish(*stalled));
    EXPECT_EQ(stalled->process->outcome().end, ToolOutcome::End::IdleTimeout);
    ASSERT_FALSE(stalled->out.isEmpty());
    EXPECT_TRUE(processGone(static_cast<pid_t>(stalled->out.first().toInt())));

    ToolProcess::Spec busy = shell(u"while :; do echo x; sleep 0.05; done"_s);
    busy.idleTimeout = 0ms;
    busy.totalTimeout = 1s; // ends a run that never stops printing, however slowly it started
    const auto endless = start(busy);
    ASSERT_TRUE(finish(*endless));
    EXPECT_EQ(endless->process->outcome().end, ToolOutcome::End::TotalTimeout);
}

TEST(ToolProcess, OutputKeepsARunAlive)
{
    // Lines for about 4 s against a 3-s idle timeout: each one resets it, so the run ends on its own.
    ToolProcess::Spec chatty = shell(u"i=0; while [ $i -lt 40 ]; do echo x; sleep 0.1; i=$((i+1)); done"_s);
    chatty.idleTimeout = 3s;
    chatty.totalTimeout = 0ms;
    const auto run = start(std::move(chatty));
    ASSERT_TRUE(finish(*run));
    EXPECT_EQ(run->process->outcome().end, ToolOutcome::End::Exited);
    EXPECT_EQ(run->process->outcome().exitCode, 0);
}

TEST(ToolProcess, DestroyingARunningProcessKillsIt)
{
    ToolProcess::Spec spec = shell(u"sleep 30 & echo $!; wait"_s);
    spec.streamLines = true;
    auto run = start(std::move(spec));
    ASSERT_TRUE(spinUntil([&] { return !run->out.isEmpty(); }, kCeilingMs));
    const auto child = static_cast<pid_t>(run->out.first().toInt());
    run->process.reset();
    EXPECT_TRUE(processGone(child));
}
