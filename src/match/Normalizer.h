// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QString>
#include <QStringList>
#include <QStringView>

namespace selecta::match {

// Version of the key rules below. Every stored key carries it; bumping it makes the library re-key in the background
// (note §4: cheap, no file I/O). Bump it whenever a change to Normalizer, VersionClassifier or the lexicon changes
// any key for any input.
inline constexpr int kNormVersion = 1;

// Fields are normalised alike except for a few artist- and title-specific rules (note §4.2 steps 7 and 8).
enum class FieldKind : quint8 {
    Plain,  // steps 1–6 and 8: words of a path, a query or a label
    Title,  // track titles: as Plain
    Album,  // album titles: as Plain
    Artist, // artist names: also drop a leading "the" and "…, The" forms; no "pt"/"vol" canonicalisation
};

// A normalised field, ready for comparison (FieldMatcher, note §17).
struct FieldKey
{
    QString key;      // tokens separated by single spaces, e.g. "daft punk"
    QString squashed; // the key without spaces: "daftpunk" (note §4.2 step 9)
    QString leet;     // squashed leet-folded key from the raw text: "P!nk" → "pink", "deadmau5" → "deadmaus" (§17.3)

    bool isEmpty() const { return key.isEmpty(); }
};

// Artist keys (note §4.2 step 9). Featured artists never enter key or set.
struct ArtistKeys
{
    FieldKey field;       // all primary artists: "above and beyond"
    QStringList set;      // each primary artist's key, sorted and unique: {"above", "beyond"}
    QStringList featured; // featured artists' keys, sorted and unique: "(feat. X)", "ft. X", "featuring X"
};

// Title keys: the title is decomposed into its core, version, featured artists and other bracket text.
struct TitleKeys
{
    FieldKey field;       // the core title without version, feat. or other bracket text: "glue"
    QString extraKey;     // other bracket text, normalised and joined with " / ": "intro"; a mismatch blocks exact
    VersionInfo version;  // VersionClassifier on the version text
    QStringList featured; // featured artists' keys found in the title
    QString labelHint;    // a trailing "[Label]" after the version, as written (1001tracklists style)
};

// Every key of one track, as stored in the library schema (note §13) and used by MatchIndex.
struct TrackKeys
{
    int normVersion = kNormVersion;
    ArtistKeys artist;
    TitleKeys title;
    FieldKey album;   // album title without edition words ("Deluxe", "Remastered", …) or years
    QString exactKey; // squash(artist) ␟ squash(title) ␟ version key; ␟ is U+241F
    QString looseKey; // squash(first artist of the set) ␟ squash(title)
};

// Normalisation shared by remote fields, local tags and filenames (note §4.2). It must stay one function for all of
// them, otherwise keys drift apart.
//
// Threading: every function is pure and reentrant; call it from any thread. No shared state, no caches.
// Failure modes: none. Any input gives a result; text longer than kMaxFieldLength code units is cut to that length
// first, so the work per call is bounded. An empty or all-punctuation input gives empty keys.
//
// Pipeline (note §4.2, steps): repair UTF-8-read-as-Latin-1 mojibake ("RÃ¶yksopp"); split CamelCase on the raw text
// ("OkumaDrift" → "Okuma Drift", "DJKoze" → "DJ Koze", but not "TiK", "MCs"); NFKD; case fold; drop non-spacing
// marks; transliterate letters that do not decompose (ø æ œ ß ł đ ð þ ı …) from a fixed in-repo table; "&" and "+" →
// " and ", "$" → "s", apostrophes removed without a space, dots between single letters removed ("A.T.F.C." →
// "atfc"), every other non-alphanumeric character → space; NFC; tokenise; artist and title rules; join.
class Normalizer final
{
public:
    // Input longer than this many UTF-16 code units is cut before normalising (untrusted input stays bounded).
    static constexpr qsizetype kMaxFieldLength = 512;

    // The normalised key of one field: tokens separated by single spaces. Idempotent:
    // normalize(normalize(x, kind), kind) == normalize(x, kind) for every input.
    static QString normalize(QStringView text, FieldKind kind = FieldKind::Plain);

    // The tokens of normalize(text, kind).
    static QStringList tokens(QStringView text, FieldKind kind = FieldKind::Plain);

    // A key without its spaces. Takes an already normalised key.
    static QString squash(QStringView key);

    // The squashed leet-folded key of raw text: in words that mix letters with digits or with "$", "@" or "!" between
    // letters, 0→o 1→i 3→e 4→a 5→s 7→t $→s @→a !→i. Pure numbers are never folded ("blink 182" ≠ "blink 183").
    static QString leetKey(QStringView text, FieldKind kind = FieldKind::Plain);

    // key, squashed and leet keys of one field.
    static FieldKey fieldKey(QStringView text, FieldKind kind = FieldKind::Plain);

    // Artist keys of an artist field as written: featured artists are extracted, primary artists split into a
    // matching set on " & ", ", ", " and ", " x ", " vs ", " vs. ", " + ", "; ", " / ", " with " (the display string is
    // left alone; note §4.1 step 7).
    static ArtistKeys artistKeys(QStringView artist);

    // Title keys of a title as written, version text included ("Glue (Bicep Remix)", "Song - Extended Mix").
    static TitleKeys titleKeys(QStringView title);

    // Album key: edition words and bracketed years removed, then normalised as an album.
    static FieldKey albumKey(QStringView album);

    // Every key of a track from its display values (tags, or parsed remote fields).
    static TrackKeys trackKeys(QStringView artist, QStringView title, QStringView album = {});

    // Repairs UTF-8 text that was decoded as Latin-1 or Windows-1252 ("RÃ¶yksopp" → "Röyksopp"). Returns the text
    // unchanged unless the repair gives valid UTF-8 that contains a non-ASCII character. Idempotent.
    static QString repairMojibake(QStringView text);

    // Splits CamelCase words on the raw text: before an upper-case letter that follows a lower-case one and is
    // followed by a lower-case one ("OkumaDrift"), and between an upper-case run and a capitalised word with at least
    // two lower-case letters ("DJKoze" → "DJ Koze"). Everything else is left alone ("TiK ToK", "MCs", "DJs", "MGMT").
    // Wrong splits ("McCartney" → "Mc Cartney") are harmless: both sides get them, and squashed keys still agree.
    static QString splitCamelCase(QStringView text);
};

} // namespace selecta::match
