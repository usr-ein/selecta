// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace selecta::match {

// A title split into its parts (note §4.1 steps 7–8, §5). Display text, not keys.
struct TitleParts
{
    QString core;           // the title without version, feat. and other trailing bracket text: "Glue"
    VersionInfo version;    // from trailing (…)/[…] groups and a trailing " - … Mix/Edit/Remix/Version/Dub" segment
    QStringList featured;   // featured artists as written: "(feat. X)", "[ft. X]", " feat. X", " featuring X"
    QStringList extras;     // other bracket text as written: "Intro", "Interlude", "Clean"; blocks an exact match
    QString labelHint;      // a trailing "[Label]" after a version group, as written
    bool truncated = false; // an unclosed bracket at the end ("Title (Extended Mi"): the name was probably cut
};

// Version classification (note §5). Word lists live in the lexicon (Lexicon.h), compiled in.
//
// Threading: pure functions; call them from any thread.
// Failure modes: none. Text that isn't a version gives std::nullopt (classify) or an Unspecified version (decompose);
// input is cut to Normalizer::kMaxFieldLength code units first.
class VersionClassifier final
{
public:
    // Classifies one version phrase: the text of a bracket group or of a trailing " - " segment ("Extended Mix",
    // "Bicep Remix", "Kerri Chandler's Vocal Dub", "Live at Brixton", "2011 Remaster"). Returns std::nullopt when the
    // phrase is not version text ("Intro", "Part 2", "feat. X").
    //
    // Rules: a named part before a remix-family word makes a remix by that name ("<X> Remix/Mix/Rework/Refix/Flip/
    // Bootleg/Reconstruction/Rerub/Version", plus "<X> Edit" and "<X> Dub" with the Edit or Dub modifier); "<X> VIP"
    // is VIP by X; "remix(ed) by X" also names X; otherwise the class words decide, by precedence Vip > Acapella >
    // Instrumental > Dub > Live > Acoustic > Demo > Extended > Radio > Vocal > Original, and the other class words
    // found become modifiers. "Remaster(ed)" sets the remaster flag only; "Mixed" sets the mixed flag only.
    static std::optional<VersionInfo> classify(QStringView phrase);

    // Splits a title as written into its parts. Groups are peeled from the end: feat. groups go to `featured`,
    // version groups to `version` (several are merged: "(Bicep Remix) (Radio Edit)" is a Bicep remix with the Radio
    // modifier), remaster and mixed groups set flags, web junk ("Official Video", "HQ", "Bonus Track") and bare
    // years are dropped, "Part 2"/"Vol. 3" groups stay in the core, and anything else goes to `extras`. A title made
    // only of one bracket group keeps it as the core.
    static TitleParts decompose(QStringView title);

    // Compatibility of two versions (note §5): see VersionCompatibility.
    static VersionCompatibility compare(const VersionInfo &a, const VersionInfo &b);
};

} // namespace selecta::match
