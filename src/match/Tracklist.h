// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Normalizer.h"

#include <QList>
#include <QString>
#include <QStringView>

namespace selecta::match {

enum class TracklistLineKind : quint8 {
    Track,        // artist and title found
    Unidentified, // "ID - ID", "Artist - ID": never searched (note §9.1)
    Empty,        // blank, a comment or only numbering
};

// One line of a pasted tracklist (PRD BATCH-1, note §9.1).
struct TracklistLine
{
    TracklistLineKind kind = TracklistLineKind::Empty;
    QString raw;           // the line as given (cut to Normalizer::kMaxFieldLength)
    QString artist;        // as written, cleaned
    QString title;         // as written, version text included
    QString labelHint;     // a trailing "[LABEL]" after the title or version
    int timestampSec = -1; // "12:34" or "[1:02:03]" at the start; -1 when none
    bool mashup = false;   // "A w/ B": flagged, searched as written (splitting is P1)
    TrackKeys keys;        // Normalizer::trackKeys(artist, title): de-duplication uses keys.exactKey
};

// Tracklist parsing (note §9.1): leading numbering ("01.", "1)", "#1"), timestamps and bullets are stripped; the rest
// is "Artist - Title" (spaced dash; en and em dashes too), or free text taken as the title; "ID" as the title marks
// an unidentified track.
//
// Threading: pure; any thread. Failure modes: none; at most kMaxLines lines are parsed.
class TracklistParser final
{
public:
    static constexpr qsizetype kMaxLines = 5000;

    static TracklistLine parseLine(QStringView line);

    // Every non-empty line, de-duplicated on keys.exactKey (first occurrence kept; unidentified lines on their text).
    static QList<TracklistLine> parse(QStringView text);
};

} // namespace selecta::match
