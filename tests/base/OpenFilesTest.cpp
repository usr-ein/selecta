// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/OpenFiles.h"

#include "support/OpenFiles.h"

#include <gtest/gtest.h>

using selecta::base::kNoOpenFileLimit;
using selecta::base::detail::raisedOpenFileLimit;

// --- What the soft limit becomes -------------------------------------------------------------------------------------

TEST(OpenFiles, RaisesTheSoftLimitToWhatIsWanted)
{
    // macOS: an app gets 256, the hard limit is unlimited, setrlimit() takes up to OPEN_MAX.
    EXPECT_EQ(raisedOpenFileLimit(256, kNoOpenFileLimit, 10'240, 1024), 1024);
    EXPECT_EQ(raisedOpenFileLimit(256, kNoOpenFileLimit, kNoOpenFileLimit, 1024), 1024);
}

TEST(OpenFiles, NeverLowersTheSoftLimit)
{
    EXPECT_EQ(raisedOpenFileLimit(1024, kNoOpenFileLimit, 10'240, 1024), std::nullopt);
    EXPECT_EQ(raisedOpenFileLimit(1'048'576, kNoOpenFileLimit, kNoOpenFileLimit, 1024), std::nullopt);
    EXPECT_EQ(raisedOpenFileLimit(kNoOpenFileLimit, kNoOpenFileLimit, kNoOpenFileLimit, 1024), std::nullopt);
}

TEST(OpenFiles, StopsAtTheHardLimitAndTheSystemCeiling)
{
    EXPECT_EQ(raisedOpenFileLimit(256, 512, 10'240, 1024), 512);
    EXPECT_EQ(raisedOpenFileLimit(256, kNoOpenFileLimit, 600, 1024), 600);
    EXPECT_EQ(raisedOpenFileLimit(256, 256, 10'240, 1024), std::nullopt); // the hard limit is already reached
}

// --- The process's own limit -----------------------------------------------------------------------------------------

#ifdef Q_OS_UNIX

TEST(OpenFiles, TheProcessLimitIsRaisedButNeverLowered)
{
    const qint64 original = selecta::base::openFileLimit();
    const int open = selecta::testing::openDescriptorCount();
    ASSERT_GT(open, 0);
    const qint64 low = open + 32;
    if (original < low + 16) {
        GTEST_SKIP() << "this process's limit (" << original << ") is too low to lower it for the test";
    }
    const selecta::testing::ScopedOpenFileLimit lowered(low);
    ASSERT_TRUE(lowered.ok());
    EXPECT_EQ(selecta::base::openFileLimit(), low);

    EXPECT_EQ(selecta::base::raiseOpenFileLimit(low + 16), low + 16);
    EXPECT_EQ(selecta::base::openFileLimit(), low + 16);
    EXPECT_EQ(selecta::base::raiseOpenFileLimit(low), low + 16); // asking for less changes nothing
    EXPECT_EQ(selecta::base::openFileLimit(), low + 16);
}

#else

TEST(OpenFiles, NoLimitWhereSocketsAreHandles)
{
    EXPECT_EQ(selecta::base::openFileLimit(), kNoOpenFileLimit);
    EXPECT_EQ(selecta::base::raiseOpenFileLimit(1024), kNoOpenFileLimit);
}

#endif
