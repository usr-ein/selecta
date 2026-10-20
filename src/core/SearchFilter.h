// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchResults.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace selecta::core {

// The formats the strict format filter offers (PRD SRCH-2).
enum class SearchFormat : quint8 {
    Flac,
    Mp3,
    Wav,
    Aiff,
    Alac,
    Aac,
    Ogg,
    Opus,
    Other, // anything else: WMA, APE, WavPack… (shown only when every format is allowed)
};
inline constexpr int kSearchFormatCount = 9;

SearchFormat searchFormatOf(match::Codec codec);
QString searchFormatName(SearchFormat format); // "FLAC", "MP3", "ALAC/M4A"…

// Why a result is hidden (PRD SRCH-9), in the order the checks run: a result counts once, under its first reason.
enum class HiddenReason : quint8 {
    None,
    NotAudio,       // covers, cue sheets, logs…
    NotThisRelease, // "Only this release": its length or name doesn't fit (SRCH-18)
    Format,
    Bitrate,        // the lossy rule (SRCH-3)
    Resolution,     // the lossless rule (SRCH-4)
    UnknownQuality, // strict mode: the peer didn't say (SRCH-6)
    Implausible,    // strict mode: the size contradicts the declared quality (SRCH-7)
    Length,
    Size,
    NoFreeSlot,
    QueueTooLong,
    TooSlow, // the peer's upload speed is under the minimum
    ExcludedWord,
    Owned, // "Already have: hide"
};
inline constexpr int kHiddenReasonCount = 15;

QString hiddenReasonLabel(HiddenReason reason); // "Format", "Bitrate"… for the breakdown

// The filters of the Search view (PRD SRCH-2…6, SRCH-18). The defaults show everything audio; presetFilter() gives
// the presets Get tracks also uses (SRCH-8).
struct SearchFilter
{
    enum class BitrateRule : quint8 {
        Any,
        Equal,
        AtLeast,
        AtMost,
    };
    enum class OwnedResults : quint8 {
        Show,
        Dim,
        Hide,
    };

    quint16 formats = 0; // bit per SearchFormat; 0: every format
    BitrateRule bitrateRule = BitrateRule::Any;
    int bitrateKbps = 320;
    bool cbrOnly = false;   // with a bitrate rule: VBR files are hidden
    int minBitDepth = 0;    // lossless: 16 or 24; 0: any
    QList<int> sampleRates; // lossless, Hz; empty: any
    int minDurationSec = 0; // 0: no bound
    int maxDurationSec = 0;
    qint64 minSizeBytes = 0;
    qint64 maxSizeBytes = 0;
    bool freeSlotOnly = false;
    int maxQueue = 0;     // 0: any
    int minSpeedKBps = 0; // the peer's upload speed, KB/s (1000 bytes); 0: any
    QStringList excludedWords;
    bool strict = false; // hide results whose quality is unknown or implausible
    OwnedResults owned = OwnedResults::Show;
    bool onlyThisRelease = true; // with a chosen release (SRCH-18)

    bool allows(SearchFormat format) const;
    void setAllowed(SearchFormat format, bool allowed);

    // The first reason `result` is hidden, or None. `releaseChosen`: the search ranks against a release.
    HiddenReason hiddenReason(const SearchResult &result, bool releaseChosen) const;

    friend bool operator==(const SearchFilter &, const SearchFilter &) = default;
};

// The filter as JSON, to save it with a preset or a Wishlist search; and back. Reading is lenient and bounded: what is
// missing, of the wrong type or out of range reads as the default.
QJsonObject filterToJson(const SearchFilter &filter);
SearchFilter filterFromJson(const QJsonObject &json);

// A named filter preset (PRD SRCH-8). Search, Get tracks (BATCH-2) and the Wishlist pick from the same list:
// Selecta's own first, which can't be changed, then the user's, saved from the Search view's filters.
struct FilterPreset
{
    QString id; // the built-in ones' below; the user's "user-<n>"
    QString name;
    SearchFilter filter;
    bool builtIn = false;

    friend bool operator==(const FilterPreset &, const FilterPreset &) = default;
};

// Selecta's presets. Everything arrives as MP3 320 (D29), so the default takes only files that can make a real one:
// lossless, or MP3 at 320 kbps. Those with a quality are strict: files that don't say their quality, or whose size
// contradicts it, stay out of the way. "Any quality" filters nothing.
inline constexpr QStringView kMp3OrLosslessPreset = u"mp3-320-or-lossless"; // the default
inline constexpr QStringView kLosslessPreset = u"lossless";
inline constexpr QStringView kHiResPreset = u"hi-res"; // 24-bit lossless
inline constexpr QStringView kAnyQualityPreset = u"any";
QList<FilterPreset> builtInPresets();
// A built-in preset by id; nullopt for any other id.
std::optional<FilterPreset> builtInPreset(QStringView id);

// How results that pass the filters are counted, and why the others are hidden (SRCH-9).
struct FilterCounts
{
    int total = 0; // audio and other files
    int shown = 0;
    std::array<int, kHiddenReasonCount> hidden{}; // by HiddenReason

    void count(HiddenReason reason)
    {
        ++total;
        if (reason == HiddenReason::None) {
            ++shown;
        } else {
            ++hidden.at(static_cast<std::size_t>(reason));
        }
    }
};

} // namespace selecta::core
