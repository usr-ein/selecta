// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/DurationConsensus.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <tuple>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace {

// A cluster spans at most this much: chains of results 3 s apart never merge two versions.
constexpr int kMaxClusterSpanSec = 2 * kConsensusWindowSec;
// Another cluster that could be the wanted version blocks "confident" from two peers on: one peer's odd length is
// noise (a truncated file, another encode), two peers agreeing are an alternative.
constexpr int kMinRivalPeers = 2;

struct Sample
{
    int seconds = 0;
    qsizetype index = 0; // input order, for a stable sort
};

// The cluster's labels: the explicit version named most often (votes, then the smaller key), and the unlabelled count.
void label(DurationCluster &cluster, const QList<DurationSample> &samples, const std::vector<Sample> &members)
{
    QHash<QString, int> votes;
    QHash<QString, VersionClass> classes;
    QSet<QString> peers;
    for (const Sample &member : members) {
        const DurationSample &sample = samples.at(member.index);
        peers.insert(sample.peer);
        if (!sample.version.isExplicit()) {
            ++cluster.unlabelled;
            continue;
        }
        const QString key = sample.version.key();
        ++votes[key];
        classes.insert(key, sample.version.versionClass);
    }
    cluster.peers = static_cast<int>(peers.size());
    cluster.samples = static_cast<int>(members.size());
    for (auto it = votes.cbegin(); it != votes.cend(); ++it) {
        if (it.value() > cluster.labelVotes || (it.value() == cluster.labelVotes && it.key() < cluster.labelKey)) {
            cluster.labelKey = it.key();
            cluster.labelVotes = it.value();
            cluster.label = classes.value(it.key());
        }
    }
}

std::vector<std::vector<Sample>> groups(const QList<DurationSample> &samples)
{
    std::vector<Sample> sorted;
    for (qsizetype i = 0; i < samples.size() && static_cast<qsizetype>(sorted.size()) < kMaxConsensusSamples; ++i) {
        if (samples.at(i).durationSec > 0) {
            sorted.push_back({samples.at(i).durationSec, i});
        }
    }
    std::ranges::stable_sort(sorted, {}, &Sample::seconds);
    std::vector<std::vector<Sample>> result;
    for (const Sample &sample : sorted) {
        const bool joins = !result.empty() && sample.seconds - result.back().back().seconds <= kConsensusWindowSec &&
                           sample.seconds - result.back().front().seconds <= kMaxClusterSpanSec;
        if (!joins) {
            result.emplace_back();
        }
        result.back().push_back(sample);
    }
    return result;
}

// Whether a cluster can hold the wanted version: the cluster labelled with it; for an unversioned or Original line, a
// cluster labelled Original or carrying no explicit label at all.
bool qualifies(const DurationCluster &cluster, const VersionInfo &wanted)
{
    if (wanted.isExplicit() && wanted.versionClass != VersionClass::Original) {
        return cluster.labelKey == wanted.key();
    }
    return cluster.labelKey.isEmpty() || cluster.label == VersionClass::Original;
}

QString describe(const DurationCluster &cluster)
{
    QString text = u"%1–%2 s: %3 peer(s), %4 result(s)"_s.arg(cluster.minSec)
                       .arg(cluster.maxSec)
                       .arg(cluster.peers)
                       .arg(cluster.samples);
    if (!cluster.labelKey.isEmpty()) {
        text += u", labelled %1 (%2)"_s.arg(cluster.labelKey).arg(cluster.labelVotes);
    }
    if (cluster.unlabelled > 0) {
        text += u", %1 unlabelled"_s.arg(cluster.unlabelled);
    }
    return text;
}

} // namespace

DurationConsensus durationConsensus(const QList<DurationSample> &samples, const VersionInfo &wanted)
{
    DurationConsensus consensus;
    for (const std::vector<Sample> &members : groups(samples)) {
        DurationCluster cluster;
        cluster.minSec = members.front().seconds;
        cluster.maxSec = members.back().seconds;
        label(cluster, samples, members);
        consensus.clusters.append(cluster);
    }
    // Most peers first, then most results, then the shorter length.
    std::ranges::stable_sort(consensus.clusters, [](const DurationCluster &a, const DurationCluster &b) {
        return std::make_tuple(-a.peers, -a.samples, a.minSec) < std::make_tuple(-b.peers, -b.samples, b.minSec);
    });
    for (const DurationCluster &cluster : std::as_const(consensus.clusters)) {
        consensus.explanation.append(describe(cluster));
    }
    qsizetype rivals = 0; // other clusters that could hold the wanted version, backed by two peers or more
    for (qsizetype i = 0; i < consensus.clusters.size(); ++i) {
        if (!qualifies(consensus.clusters.at(i), wanted)) {
            continue;
        }
        if (consensus.preferred < 0) {
            consensus.preferred = i; // the most peers among those that qualify
        } else if (consensus.clusters.at(i).peers >= kMinRivalPeers) {
            ++rivals;
        }
    }
    const QString wantedName = wanted.isExplicit() ? wanted.key() : u"unversioned"_s;
    if (consensus.preferred < 0) {
        consensus.explanation.append(
            u"no cluster fits the %1 line: every one is named by another version"_s.arg(wantedName));
        return consensus;
    }
    const DurationCluster &preferred = consensus.clusters.at(consensus.preferred);
    consensus.confident = preferred.peers >= kConfidentMinPeers && rivals == 0;
    QString why;
    if (preferred.peers < kConfidentMinPeers) {
        why = u", not confident: fewer than %1 peers"_s.arg(kConfidentMinPeers);
    } else if (rivals > 0) {
        why = u", not confident: %1 other cluster(s) could be it"_s.arg(rivals);
    } else {
        why = u", confident"_s;
    }
    consensus.explanation.append(u"preferred for the %1 line: %2%3"_s.arg(wantedName, describe(preferred), why));
    return consensus;
}

} // namespace selecta::match
