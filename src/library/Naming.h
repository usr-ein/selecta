// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringView>

namespace selecta::library::naming {

// The longest file or folder name Selecta writes, in UTF-8 bytes: APFS allows 255; the margin leaves room for " (2)"
// and for copies to other file systems (references/conversion-and-metadata.md §7.2).
inline constexpr int kMaxComponentBytes = 200;

// Values a naming template can use. Tags come from peers' files and from metadata services: all untrusted.
struct Fields
{
    QString artist;
    QString albumArtist;
    QString title; // including the version, e.g. "Tidewater (Original Mix)"
    QString album;
    QString year;
    QString track; // a number; {track} pads it to two digits
    QString disc;
    QString discTotal;
    QString label;
    QString catalogNumber;
    QString key;
    QString bpm;
};

// The three templates of Settings → Filing (PRD DL-13, D22). `[…]` marks an optional section, dropped when a token
// inside it is empty.
struct Templates
{
    QString single = QStringLiteral("{artist} - {title}");
    QString albumFolder = QStringLiteral("{albumartist} - {album}[ ({year})]");
    QString albumTrack = QStringLiteral("{track} - {title}");
};

// The name cleaner (PRD DL-13). `separator` replaces spaces and symbols; the PRD default is '_'.
struct Cleaner
{
    bool enabled = true;
    QChar separator = u'_';
};

// Expands `{token}`, `{token:NN}` (zero-padded to NN digits) and `[optional sections]`. Tokens: artist, albumartist,
// title, album, year, track, disc, disctotal, label, catalognumber, key, bpm. An unknown token expands to nothing.
QString expand(QStringView pattern, const Fields &fields);

// The PRD DL-13 cleaner: NFC, quotes dropped, "&" → "and", spaces, brackets and other symbols → separator, repeats
// collapsed, separators and dots trimmed at both ends. Unicode letters and digits are kept.
// Example: "Halden & Rue - Tidewater (Original Mix)" → "Halden_and_Rue_-_Tidewater_Original_Mix".
QString clean(const QString &name, QChar separator);

// Makes one path component safe whatever the cleaner setting: NFC; control, bidirectional and invisible characters
// removed; "/", "\", ":" and other reserved characters replaced; no leading dot or dash, no trailing dot or space; no
// reserved device names; at most `maxBytes` UTF-8 bytes, cut between characters. Never returns an empty name.
QString safeComponent(const QString &name, int maxBytes = kMaxComponentBytes);

// A finished file or folder name: the template expanded with fallbacks for missing values ("Unknown Artist",
// "Untitled"…), then cleaned (if enabled) and made safe, with ".extension" appended when given. The whole name,
// extension included, fits kMaxComponentBytes.
QString componentName(QStringView pattern, const Fields &fields, Cleaner cleaner, const QString &extension = {});

} // namespace selecta::library::naming
