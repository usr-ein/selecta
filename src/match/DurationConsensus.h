// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace selecta::match {

inline constexpr int kConsensusWindowSec = 3; // results within ±3 s of each other form one cluster
inline constexpr int kConfidentMinPeers = 3;  // independent peers needed for a confident cluster (BATCH-6)
inline constexpr qsizetype kMaxConsensusSamples = 10000;

// One identity-passing result of a Get tracks line: who shares it, its length, and its parsed version.
struct DurationSample
{
    QString peer;
    int durationSec = 0; // 0 = absent: ignored
    VersionInfo version;
};

struct DurationCluster
{
    int minSec = 0;
    int maxSec = 0;
    int peers = 0; // distinct peers
    int samples = 0;
    QString labelKey; // VersionInfo::key() named by the cluster's explicit labels (majority); "" when none
    VersionClass label = VersionClass::Unspecified;
    int labelVotes = 0; // samples carrying that label
    int unlabelled = 0; // samples with an Unspecified version
};

struct DurationConsensus
{
    QList<DurationCluster> clusters; // most peers first
    qsizetype preferred = -1;        // index of the wanted version's cluster; -1 when none fits
    bool confident = false;          // preferred has ≥ 3 peers and no other cluster claims the same version
    QStringList explanation;
};

// Duration consensus for a Get tracks line (note §9.1, T51): clusters usually correspond to versions; explicitly
// labelled results name their cluster. For a wanted explicit version, the cluster labelled with it is preferred. For
// an unversioned line (or Original), the preferred cluster is one labelled Original or carrying no explicit label at
// all; clusters named by an explicit non-original version are not preferred. When several qualify, the one with the
// most peers wins.
//
// Threading: pure; any thread. Failure modes: none; samples without a length are ignored, at most
// kMaxConsensusSamples are used.
DurationConsensus durationConsensus(const QList<DurationSample> &samples, const VersionInfo &wanted);

} // namespace selecta::match
