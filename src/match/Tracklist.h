// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Normalizer.h"

#include <QList>
#include <QString>
#include <QStringView>

namespace selecta::match {

enum class TracklistLineKind : quint8 {
    Track,        // artist and title found, or free text taken as the title
    Unidentified, // "ID - ID", "Artist - ID", "Unknown Artist - Unknown Title": never searched (note §9.1)
    Empty,        // blank, a heading or separator, only numbering, or a link (links go through META-3)
};

// One line of a pasted tracklist (PRD BATCH-1, note §9.1).
struct TracklistLine
{
    TracklistLineKind kind = TracklistLineKind::Empty;
    QString raw;           // the line as given (cut to Normalizer::kMaxFieldLength)
    QString artist;        // as written, cleaned; empty for free text
    QString title;         // as written, version text included
    QString labelHint;     // a trailing "[LABEL]" after the title or version (1001tracklists style)
    int timestampSec = -1; // "12:34" or "[1:02:03]" at the start; -1 when none
    int durationSec = -1;  // a trailing length ("5:23", "(5:23)", Discogs style); -1 when none
    bool mashup = false;   // "A w/ B" or a "w/" layer line: flagged, searched as written (splitting is P1)
    TrackKeys keys;        // Normalizer::trackKeys(artist, title): de-duplication uses keys.exactKey
};

// Tracklist parsing (note §9.1). Stripped from the start, in any combination: bullets ("-", "*", "•", "►"…),
// numbering ("01.", "1)", "#1", "[01]", "(1)", "Track 1:", vinyl "A1"), timestamps ("12:34", "[1:02:03]",
// "(12:34)"). Stripped from the end: a length ("5:23", "(5:23)") and a "[LABEL]". The rest is split on the first
// spaced dash (" - ", " – ", " — ", " -- "), on a tab, or around one quoted title (Artist "Title"); otherwise it is
// free text taken as the title. "ID" (or "Unknown …") as the title marks an unidentified track.
//
// Threading: pure; any thread. Failure modes: none; lines are cut to Normalizer::kMaxFieldLength and at most
// kMaxLines lines are parsed.
class TracklistParser final
{
public:
    static constexpr qsizetype kMaxLines = 5000;

    static TracklistLine parseLine(QStringView line);

    // Every non-empty line, de-duplicated (first occurrence kept): tracks on keys.exactKey, unidentified lines on
    // their normalised text.
    static QList<TracklistLine> parse(QStringView text);
};

} // namespace selecta::match
