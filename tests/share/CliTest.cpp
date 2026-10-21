// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli share index (tools/selecta-cli/commands/share/): the collection's folders by their roles, as core shares
// them. Runs the command as a process over a temporary folder; nothing touches the network.
#include "ShareTestSupport.h"

#include <QProcess>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::share::test::Tree;

namespace {

struct CliRun
{
    int exitCode = -1;
    QString out;
    QString err;
};

CliRun cli(const QStringList &arguments)
{
    QProcess process;
    process.start(QStringLiteral(SELECTA_CLI_PATH), QStringList{u"share"_s} + arguments);
    CliRun run;
    if (!process.waitForFinished(30'000)) {
        process.kill();
        process.waitForFinished(3000);
        run.err = process.errorString();
        return run;
    }
    run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    run.out = QString::fromUtf8(process.readAllStandardOutput());
    run.err = QString::fromUtf8(process.readAllStandardError());
    return run;
}

class ShareCli : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tree.file(u"Tracks/Loose.mp3"_s);
        tree.file(u"Tracks/Sub/Deeper.mp3"_s);
        tree.file(u"Albums/Stray.mp3"_s);
        tree.file(u"Albums/Okuma Drift - Neon Rain/01 Neon Rain.mp3"_s);
        tree.file(u"Albums/Okuma Drift - Neon Rain/cover.jpg"_s);
        tree.file(u"Albums/Okuma Drift - Neon Rain/CD2/01 Bonus.mp3"_s);
    }

    Tree tree;
};

TEST_F(ShareCli, SharesEachFolderByItsRole)
{
    const CliRun run = cli({u"index"_s, u"--tracks"_s, tree.path(u"Tracks"_s), u"--albums"_s, tree.path(u"Albums"_s),
                            u"--lookup"_s, u"Tracks\\Loose.mp3"_s, u"--lookup"_s, u"Tracks\\Sub\\Deeper.mp3"_s,
                            u"--lookup"_s, u"Albums\\Okuma Drift - Neon Rain\\01 Neon Rain.mp3"_s, u"--lookup"_s,
                            u"Albums\\Stray.mp3"_s, u"--lookup"_s, u"Albums\\Okuma Drift - Neon Rain\\cover.jpg"_s});
    ASSERT_EQ(run.exitCode, 0) << run.err.toStdString();
    EXPECT_TRUE(run.out.contains(u"→ Tracks (tracks)"_s)) << run.out.toStdString();
    EXPECT_TRUE(run.out.contains(u"→ Albums (albums)"_s)) << run.out.toStdString();
    EXPECT_TRUE(run.out.contains(u"2 files in 3 folders"_s)) << run.out.toStdString();
    EXPECT_TRUE(run.out.contains(u"lookup     \"Tracks\\Loose.mp3\" → "_s));
    EXPECT_TRUE(run.out.contains(u"lookup     \"Albums\\Okuma Drift - Neon Rain\\01 Neon Rain.mp3\" → "_s));
    EXPECT_TRUE(run.out.contains(u"lookup     \"Tracks\\Sub\\Deeper.mp3\": File not shared."_s));
    EXPECT_TRUE(run.out.contains(u"lookup     \"Albums\\Stray.mp3\": File not shared."_s));
    EXPECT_TRUE(run.out.contains(u"lookup     \"Albums\\Okuma Drift - Neon Rain\\cover.jpg\": File not shared."_s));
}

TEST_F(ShareCli, AFolderInBothRolesIsOneRoot)
{
    const CliRun run = cli({u"index"_s, u"--tracks"_s, tree.path(u"Albums"_s), u"--albums"_s, tree.path(u"Albums/"_s)});
    ASSERT_EQ(run.exitCode, 0) << run.err.toStdString();
    EXPECT_TRUE(run.out.contains(u"→ Albums (tracks and albums)"_s)) << run.out.toStdString();
    EXPECT_TRUE(run.out.contains(u"2 files in 2 folders"_s)) << run.out.toStdString();
}

TEST_F(ShareCli, AFolderWithoutARoleIsRefused)
{
    for (const QStringList &arguments : {QStringList{u"index"_s, tree.path(u"Albums"_s)}, QStringList{u"index"_s},
                                         QStringList{u"index"_s, u"--tracks"_s}}) {
        const CliRun run = cli(arguments);
        EXPECT_NE(run.exitCode, 0) << arguments.join(u' ').toStdString();
        EXPECT_TRUE(run.err.contains(u"--tracks <folder>"_s)) << run.err.toStdString();
    }
}

} // namespace
