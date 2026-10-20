// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QStringView>

#include <optional>

// Internal: lookups into the word lists of Lexicon.cpp, the one compiled-in data file of the library (contract X1).
// Every list is sorted at compile time; lookups are binary searches. Inputs are normalised text (Normalizer) unless a
// function says otherwise. Pure functions, any thread.
namespace selecta::match::detail::lexicon {

// --- Normalisation -------------------------------------------------------------------------------------------------

// The fixed transliteration of a case-folded letter that does not decompose ("ø" → "o", "ß" → "ss"), or empty.
QStringView transliteration(char32_t codePoint);

// Apostrophe-like characters, removed without a space ("What's" → "whats").
bool isApostrophe(char32_t codePoint);

// Leet folding of one character (0→o 1→i 3→e 4→a 5→s 7→t $→s @→a !→i), or 0 when it has none.
char16_t leetFold(char16_t c);

// "one" … "twenty" → 1 … 20, after part/volume/chapter.
std::optional<int> numberWord(QStringView token);

// Words after which roman numerals and number words become digits: "part", "volume", "chapter".
bool isNumberingWord(QStringView token);

// Light canonicalisation of one token: "pt" → "part", "vol" → "volume"; empty when unchanged.
QStringView canonicalToken(QStringView token);

// Stop words weigh 1 in token alignment instead of their length ("the", "and", "feat"…).
bool isLightWord(QStringView token);

// --- Versions ------------------------------------------------------------------------------------------------------

enum class VersionWord : quint8 {
    None,
    ClassWord,      // maps to a class (classWord())
    RemixKeyword,   // remix, rmx, rework, refix, flip, bootleg, reconstruction, rerub, remake
    MixKeyword,     // mix: a remix when named, a filler after class words ("Extended Mix")
    EditKeyword,    // edit, reedit: a named edit, or an unnamed radio edit
    VersionKeyword, // version: a remix when named ("Kerri Chandler Version"), a filler after class words
    DubKeyword,     // dub: a class on its own, a named dub ("<X> Dub") otherwise
    VipKeyword,     // vip
};

VersionWord versionWord(QStringView token);

// The class a class word stands for, with the modifier it gives when it isn't the main class.
struct ClassWord
{
    VersionClass versionClass = VersionClass::Unspecified;
    VersionModifier modifier = VersionModifier::None;
    bool needsKeyword = false; // only a class word before mix/version/edit ("main", "album", "single", "12", "7")
};
std::optional<ClassWord> classWord(QStringView token);

// Class precedence for the main class: higher wins (Vip > Acapella > … > Original).
int classPrecedence(VersionClass versionClass);

// The modifier that stands for a class when it isn't the main class.
VersionModifier modifierFor(VersionClass versionClass);

bool isRemasterWord(QStringView token); // remaster, remastered, remasters, digitally, digital
bool isMixedWord(QStringView token);    // mixed
bool isFeatWord(QStringView token);     // feat, ft, featuring (normalised)

// "<X> Version" words that make a variant, not a remix: clean, explicit, mono, language names… (the phrase is then
// title extra text, not a version).
bool isVariantWord(QStringView token);

// Bracket text dropped from titles and names: web junk ("official video", "hq"), "bonus track", producer credits.
bool isWebJunkPhrase(QStringView normalisedPhrase);

// --- Paths -----------------------------------------------------------------------------------------------------------

// Generic folder names that never give an artist or album (note §4.1 step 2): stop list, genres, letter buckets,
// decades and years.
bool isGenericFolder(QStringView normalisedName);

// "va", "various artists", "compilations"…
bool isVariousArtists(QStringView normalisedName);

// Quality words of a bracket group or a scene folder ("flac", "320", "v0", "web", "vinyl", "24bit"…). `strong` words
// alone make a group a quality tag; weak ones only together with a strong one.
enum class QualityWord : quint8 { None, Weak, Strong };
QualityWord qualityWord(QStringView token);

// Scene source tags ("web", "cd", "vinyl", "vls"…) and their display form ("WEB", "CD", "VINYL").
QStringView sceneSource(QStringView token);

// Edition words stripped from album titles ("deluxe", "remastered", "anniversary", "edition"…). `strong` words alone
// make a group edition junk.
bool isEditionWord(QStringView token);
bool isStrongEditionWord(QStringView token);

// Release-type tags in album folder names: "ep", "lp", "single", "maxi", "album".
bool isReleaseTypeWord(QStringView token);

// Top-level domains recognised in website prefixes and suffixes ("com", "net", "ru"…).
bool isWebsiteTld(QStringView token);

// Generic CD-rip file names: "track", "audiotrack", "piste", "pista", "titel", "titre" (followed by a number).
bool isRipWord(QStringView token);

// --- File kinds ------------------------------------------------------------------------------------------------------

bool isAudioExtension(QStringView lowerExtension);
bool isCompanionExtension(QStringView lowerExtension);

// --- Queries ---------------------------------------------------------------------------------------------------------

// Stop and version words never sent in a network query (note §17.7).
bool isQueryStopWord(QStringView foldedWord);

// Nicotine+'s responder word separators (pynicotine/utils.py PUNCTUATION) plus whitespace and Unicode punctuation.
bool isResponderSeparator(char32_t codePoint);

} // namespace selecta::match::detail::lexicon
