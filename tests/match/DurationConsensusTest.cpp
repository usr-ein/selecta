// SPDX-License-Identifier: GPL-3.0-or-later
// Duration consensus for Get tracks lines (note §9.1, T51, BATCH-6).
#include "match/DurationConsensus.h"

#include "MatchTestSupport.h"
#include "match/VersionClassifier.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::match::DurationCluster;
using selecta::match::DurationConsensus;
using selecta::match::DurationSample;
using selecta::match::VersionClass;
using selecta::match::VersionClassifier;
using selecta::match::VersionInfo;

namespace {

VersionInfo version(const QString &phrase)
{
    if (phrase.isEmpty()) {
        return {};
    }
    return VersionClassifier::classify(phrase).value_or(VersionInfo{});
}

// "peer:seconds:version" entries.
QList<DurationSample> samples(std::initializer_list<std::tuple<const char *, int, const char *>> entries)
{
    QList<DurationSample> list;
    for (const auto &[peer, seconds, phrase] : entries) {
        list.append({QString::fromUtf8(peer), seconds, version(QString::fromUtf8(phrase))});
    }
    return list;
}

// T51: 211 s (5 peers, 2 say "Radio Edit") and 408 s (4 peers, 3 say "Extended Mix").
QList<DurationSample> t51()
{
    return samples({{"a", 211, "Radio Edit"},
                    {"b", 211, "Radio Edit"},
                    {"c", 212, ""},
                    {"d", 210, ""},
                    {"e", 211, ""},
                    {"f", 408, "Extended Mix"},
                    {"g", 409, "Extended Mix"},
                    {"h", 408, "Extended Mix"},
                    {"i", 407, ""}});
}

TEST(DurationConsensus, ClustersCorrespondToVersions) // T51
{
    const DurationConsensus consensus = selecta::match::durationConsensus(t51(), {});
    ASSERT_EQ(consensus.clusters.size(), 2);
    const DurationCluster &radio = consensus.clusters.at(0); // most peers first
    EXPECT_EQ(radio.minSec, 210);
    EXPECT_EQ(radio.maxSec, 212);
    EXPECT_EQ(radio.peers, 5);
    EXPECT_EQ(radio.label, VersionClass::Radio);
    EXPECT_EQ(radio.labelVotes, 2);
    EXPECT_EQ(radio.unlabelled, 3);
    const DurationCluster &extended = consensus.clusters.at(1);
    EXPECT_EQ(extended.peers, 4);
    EXPECT_EQ(extended.label, VersionClass::Extended);
    EXPECT_EQ(extended.labelVotes, 3);
    // The unversioned line: both clusters are named by an explicit non-original version, so neither is preferred.
    EXPECT_EQ(consensus.preferred, -1);
    EXPECT_FALSE(consensus.confident);
    EXPECT_FALSE(consensus.explanation.isEmpty());
}

TEST(DurationConsensus, AnExplicitLineTakesItsLabelledCluster) // T51
{
    const DurationConsensus extended = selecta::match::durationConsensus(t51(), version(u"Extended Mix"_s));
    ASSERT_GE(extended.preferred, 0);
    EXPECT_EQ(extended.clusters.at(extended.preferred).minSec, 407);
    EXPECT_TRUE(extended.confident); // 4 peers, no other cluster named Extended

    const DurationConsensus radio = selecta::match::durationConsensus(t51(), version(u"Radio Edit"_s));
    ASSERT_GE(radio.preferred, 0);
    EXPECT_EQ(radio.clusters.at(radio.preferred).minSec, 210);
    EXPECT_TRUE(radio.confident);

    const DurationConsensus dub = selecta::match::durationConsensus(t51(), version(u"Dub"_s));
    EXPECT_EQ(dub.preferred, -1);
    EXPECT_FALSE(dub.confident);
}

TEST(DurationConsensus, AnUnversionedLinePrefersTheUnclaimedCluster) // T51, BATCH-6
{
    const QList<DurationSample> list = samples({{"a", 211, ""},
                                                {"b", 211, ""},
                                                {"c", 212, "Original Mix"},
                                                {"d", 210, ""},
                                                {"f", 408, "Extended Mix"},
                                                {"g", 409, "Extended Mix"},
                                                {"h", 408, ""}});
    for (const VersionInfo &wanted : {VersionInfo{}, version(u"Original Mix"_s)}) {
        const DurationConsensus consensus = selecta::match::durationConsensus(list, wanted);
        ASSERT_GE(consensus.preferred, 0);
        const DurationCluster &preferred = consensus.clusters.at(consensus.preferred);
        EXPECT_EQ(preferred.minSec, 210);
        EXPECT_EQ(preferred.label, VersionClass::Original);
        EXPECT_TRUE(consensus.confident); // 4 peers; the other cluster is named Extended
    }
}

TEST(DurationConsensus, TwoPlausibleClustersAreNotConfident)
{
    // Two unlabelled lengths, both backed by several peers: which one is the original is a guess.
    const QList<DurationSample> list =
        samples({{"a", 211, ""}, {"b", 211, ""}, {"c", 212, ""}, {"d", 300, ""}, {"e", 301, ""}});
    const DurationConsensus consensus = selecta::match::durationConsensus(list, {});
    ASSERT_GE(consensus.preferred, 0);
    EXPECT_EQ(consensus.clusters.at(consensus.preferred).minSec, 211); // more peers
    EXPECT_FALSE(consensus.confident);
}

TEST(DurationConsensus, OnePeersOddLengthIsNoise)
{
    const QList<DurationSample> list =
        samples({{"a", 211, ""}, {"b", 211, ""}, {"c", 212, ""}, {"d", 97, ""}}); // a truncated file
    const DurationConsensus consensus = selecta::match::durationConsensus(list, {});
    ASSERT_GE(consensus.preferred, 0);
    EXPECT_EQ(consensus.clusters.at(consensus.preferred).peers, 3);
    EXPECT_TRUE(consensus.confident);
}

TEST(DurationConsensus, PeersAreCountedOnce)
{
    // One peer sharing the same file three times is one witness, not three.
    const QList<DurationSample> list = samples({{"a", 211, ""}, {"a", 211, ""}, {"a", 212, ""}, {"b", 211, ""}});
    const DurationConsensus consensus = selecta::match::durationConsensus(list, {});
    ASSERT_EQ(consensus.clusters.size(), 1);
    EXPECT_EQ(consensus.clusters.front().peers, 2);
    EXPECT_EQ(consensus.clusters.front().samples, 4);
    EXPECT_FALSE(consensus.confident); // fewer than 3 independent peers
}

TEST(DurationConsensus, ClustersStayTight)
{
    // A chain 3 s apart never merges two versions: a cluster spans at most 6 s.
    const QList<DurationSample> list =
        samples({{"a", 200, ""}, {"b", 203, ""}, {"c", 206, ""}, {"d", 209, ""}, {"e", 212, ""}, {"f", 260, ""}});
    const DurationConsensus consensus = selecta::match::durationConsensus(list, {});
    ASSERT_EQ(consensus.clusters.size(), 3);
    for (const DurationCluster &cluster : consensus.clusters) {
        EXPECT_LE(cluster.maxSec - cluster.minSec, 2 * selecta::match::kConsensusWindowSec);
    }
}

TEST(DurationConsensus, AMislabelledFileDoesntNameItsCluster)
{
    // One "(Extended Mix)" in the radio-edit cluster: the cluster's label is still Radio Edit, by majority.
    const QList<DurationSample> list = samples({{"a", 211, "Radio Edit"},
                                                {"b", 211, "Radio Edit"},
                                                {"c", 212, "Extended Mix"},
                                                {"d", 408, "Extended Mix"},
                                                {"e", 409, "Extended Mix"},
                                                {"f", 408, "Extended Mix"}});
    const DurationConsensus consensus = selecta::match::durationConsensus(list, version(u"Extended Mix"_s));
    ASSERT_GE(consensus.preferred, 0);
    EXPECT_EQ(consensus.clusters.at(consensus.preferred).minSec, 408);
    EXPECT_TRUE(consensus.confident);
}

TEST(DurationConsensus, SamplesWithoutALengthAreIgnored)
{
    EXPECT_TRUE(selecta::match::durationConsensus({}, {}).clusters.isEmpty());
    const DurationConsensus consensus = selecta::match::durationConsensus(samples({{"a", 0, ""}, {"b", -5, ""}}), {});
    EXPECT_TRUE(consensus.clusters.isEmpty());
    EXPECT_EQ(consensus.preferred, -1);
}

TEST(DurationConsensus, BoundsHugeInput)
{
    QList<DurationSample> list;
    for (int i = 0; i < 20000; ++i) {
        list.append({u"peer%1"_s.arg(i), 200 + (i % 50), {}});
    }
    const DurationConsensus consensus = selecta::match::durationConsensus(list, {});
    int counted = 0;
    for (const DurationCluster &cluster : consensus.clusters) {
        counted += cluster.samples;
    }
    EXPECT_EQ(counted, static_cast<int>(selecta::match::kMaxConsensusSamples));
}

} // namespace
