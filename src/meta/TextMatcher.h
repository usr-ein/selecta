// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <memory>

// The text comparisons the release resolver needs, behind an interface so the match/ library's Normalizer and
// VersionClassifier (contract: match.md) can replace meta's local adapter once they land: core passes an adapter
// implementing TextMatcher over match/, and nothing else in meta changes.
namespace selecta::meta {

// Version classes of references/library-matching-and-organization.md §5. A DJ treats each as a different track.
enum class VersionClass : quint8 {
    Unspecified, // no version text
    Original,    // Original Mix, Original Version, Main Mix, Album Version
    Extended,    // Extended Mix/Version/Edit, Club Mix, 12" Mix
    Radio,       // Radio Edit/Mix/Version, Edit, Single Version, 7" Version
    Remix,       // <X> Remix / Mix / Rework / Edit / Re-Edit / Refix / Flip / Bootleg / Version / Dub: `by` = X
    Vip,
    Dub,
    Instrumental,
    Vocal,
    Acapella,
    Live,
    Acoustic,
    Demo,
    Other, // bracketed text that is none of the above ("Part 2", "Intro"): compared as text
};

struct Version
{
    VersionClass kind = VersionClass::Unspecified;
    QString by;            // remixer key for Remix (and named Dub/VIP), else empty
    QString raw;           // the version text as written, without brackets
    bool remaster = false; // "Remastered", "2017 Remaster": the same recording, never a conflict
};

enum class VersionRelation : quint8 {
    Same,       // same class and remixer (or both unspecified, or only "remaster" differs)
    Compatible, // unspecified vs Original: probably the same track
    Unsure,     // unspecified vs an explicit version: only the duration can tell
    Conflict,   // two different explicit versions (Extended vs Radio, remix by X vs by Y)
};

// A title taken apart: base text, version, featured artists, and the markers the resolver penalises.
struct ParsedTitle
{
    QString base; // without version, featured artists and junk brackets
    Version version;
    QStringList featured; // artists from "(feat. X)", "ft. X"…
    bool djMix = false;   // a DJ-mix segment: "(Mixed)", " - Mixed", "(Mix Cut)"
    QStringList junk;     // derivative markers present: "slowed", "sped up", "nightcore", "8d", "reverb",
                          // "karaoke", "in the style of", "tribute", "cover", "bass boosted"
};

// Thread-safe: implementations are immutable after construction, so one instance can be shared across threads.
class TextMatcher
{
public:
    TextMatcher() = default;
    TextMatcher(const TextMatcher &) = delete;
    TextMatcher &operator=(const TextMatcher &) = delete;
    TextMatcher(TextMatcher &&) = delete;
    TextMatcher &operator=(TextMatcher &&) = delete;
    virtual ~TextMatcher();

    virtual ParsedTitle parseTitle(const QString &title) const = 0;
    // A credit split into artists: "A & B feat. C" → {A, B, C}; "A, B" → {A, B}; names stay as written.
    virtual QStringList splitArtists(const QString &credit) const = 0;
    // The comparison key: folded case, diacritics and symbols (Röyksopp → royksopp, & → and), single spaces.
    virtual QString key(const QString &text) const = 0;
    // Similarity of two names or titles in [0, 1] on their keys, tolerant to word order and small typos.
    virtual double similarity(const QString &a, const QString &b) const = 0;
    virtual VersionRelation relation(const Version &a, const Version &b) const = 0;
};

// meta's own adapter until match/ lands: the §4.2 key steps (NFKD, case folding, diacritics, a transliteration table,
// & → and, apostrophes), the §5 version classes, token-sort ratio (rapidfuzz) for similarity.
class LocalTextMatcher final : public TextMatcher
{
public:
    LocalTextMatcher() = default;

    ParsedTitle parseTitle(const QString &title) const override;
    QStringList splitArtists(const QString &credit) const override;
    QString key(const QString &text) const override;
    double similarity(const QString &a, const QString &b) const override;
    VersionRelation relation(const Version &a, const Version &b) const override;
};

// A shared LocalTextMatcher. Thread-safe.
std::shared_ptr<const TextMatcher> localTextMatcher();

} // namespace selecta::meta
