// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/RemotePathParser.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

// Internal: cleaning a remote file or folder name (note §4.1 step 3) and scanning its track numbering (step 4), for
// RemotePathParser. Hand-written scanners; pure functions, any thread.
namespace selecta::match::detail {

// A cleaned name (note §4.1 step 3): tags extracted, junk stripped, brackets that belong to the title kept.
struct CleanName
{
    QString text;         // the name with kept bracket groups
    QString plain;        // the name without any bracket group (zone F)
    QStringList brackets; // kept groups and leading groups, without brackets (zone B)
    int year = 0;
    int disc = 0;
    QString catalogue;
    QString quality;
    QString source;
    bool vinylHint = false;
    QList<ParseRule> rules;
};

// A track-number prefix (note §4.1 step 4): "03 - ", "03. ", "1-05 ", "101 ", vinyl "B2 ".
struct Prefix
{
    int disc = 0;
    int track = 0;
    int digits = 0;           // digits of the (last) number
    QString vinyl;            // "B2"
    qsizetype length = 0;     // characters consumed, separator included
    bool punctuation = false; // separated by "." "-" ")" "_" rather than spaces only
    bool leadingZero = false;
    bool rest = false; // text follows the prefix
};

// Splits on top-level occurrences of `separator` (not inside brackets).
QStringList splitTopLevel(QStringView text, QStringView separator);

// "CD2", "CD 2", "Disc 2", "Disk-2", "DVD1", "Disc Two": the disc number, or nothing.
std::optional<int> discNumberOf(QStringView text);

// Appends a rule once.
void addRule(QList<ParseRule> &rules, ParseRule rule);

// Cleans a file name body or a folder name: mojibake repaired, NFC, dashes and quotes unified, underscores and dots
// as spaces when the name has no spaces, websites stripped, "(Artist - Title) Remixer Remix" unwrapped, and every
// bracket group sorted into a year, quality, catalogue, disc or release-type tag, junk, or text kept for the title.
CleanName cleanName(QStringView raw);

// The track-number prefix at the start of a cleaned name, if it has one; whether to strip it is the parser's
// decision (siblings, leading zero, punctuation).
std::optional<Prefix> scanPrefix(QStringView text);

// "Track 01", "AudioTrack 01", "Track01", "Piste 3".
std::optional<int> ripNumber(QStringView text);

} // namespace selecta::match::detail
