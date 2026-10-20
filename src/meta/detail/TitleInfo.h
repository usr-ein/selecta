// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Normalizer.h"
#include "match/Types.h"
#include "meta/Release.h"

#include <QString>
#include <QStringList>

// Titles and credits taken apart for release matching, on top of match/'s Normalizer and VersionClassifier (the
// rules the Soulseek ranking uses), plus what only the resolver penalises: derivative junk and DJ-mix segments.
// Pure functions, thread-safe.
namespace selecta::meta::detail {

struct TitleInfo
{
    match::FieldKey core; // the title without version, feat. and other bracket text, normalised
    QString coreDisplay;  // the same as written ("Glass Harbour"), for search queries
    match::VersionInfo version;
    QStringList featured; // featured artists' keys found in the title
    QStringList junk;     // derivative markers outside the core: "slowed", "nightcore", "sped up"…
    bool djMix = false;   // a DJ-mix segment ("(Mixed)", " - Mixed", "(Mix Cut)")
};

// `explicitVersion`, when it classifies as a version, replaces the one found in the title.
TitleInfo analyseTitle(const QString &title, const QString &explicitVersion = {});

// The first artist of a credit as written: "Kestrel Moon feat. Clara Vale" → "Kestrel Moon", "A & B" → "A".
QString primaryArtist(const QString &credit);

// Normalised keys of a release's credited artists plus the featured artists its title names, without duplicates.
QStringList releaseArtistKeys(const Release &release, const TitleInfo &title);

// The version words to add to a search ("Extended Mix", "Night Tram Remix"), empty for an unspecified version.
QString versionWords(const match::VersionInfo &version);

} // namespace selecta::meta::detail
