// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Release.h"
#include "meta/Result.h"
#include "meta/TextMatcher.h"

#include <QFuture>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace selecta::meta {

class DeezerClient;
class SpotifyClient;

// A line to resolve: from Get tracks, a parsed Soulseek result or an Inbox file. Rebuilt from parsed fields, never
// from a raw remote path (paths can carry peer names, §14.8).
struct ResolveQuery
{
    QString artist;  // the credit as written: "Bicep", "Bicep feat. Clara La San"
    QString title;   // may carry its version: "Glue (Extended Mix)", "Glue - Extended Mix"
    QString version; // a version parsed separately; when set it overrides the one in the title
    QString album;   // album hint (album downloads, Inbox folders), optional
    QString label;   // label hint ("[Ninja Tune]" in a tracklist), optional
    std::optional<qint64> durationMs;
    QString isrc;    // from a file's TSRC or a playlist item, optional
};

enum class Confidence : quint8 {
    None,      // best score < 0.60: "not found"
    Ambiguous, // ≥ 0.60: the user picks
    Likely,    // ≥ 0.80: chosen, marked for a glance
    Confident, // ≥ 0.92 and ≥ 0.05 above the best other recording or version: chosen automatically
    Exact,     // the ISRC matches, the length agrees (or is unknown) and the title fits
};

QString confidenceName(Confidence confidence); // "exact", "confident"… Thread-safe (pure).

struct Candidate
{
    Release release;
    double score = 0.0; // 0…1 after multipliers
    Confidence confidence = Confidence::None;
    VersionRelation version = VersionRelation::Unsure;
    QStringList reasons; // why, for the inspector: "ISRC matches", "title 0.97 · artist 1.00 · length Δ 2 s"…
};

struct Resolution
{
    QList<Candidate> candidates;        // best first; other versions (version conflict, score 0) last
    Confidence confidence = Confidence::None; // of the first candidate
    QList<Source> sourcesTried;
    QList<MetaError> sourceErrors;      // sources that failed while others answered
};

// Turns a line into ranked release candidates with scores and explanations (META-2, D23; §14.5 of the metadata
// note, with spotDL's lessons, §8.5):
// - identifiers first: with an ISRC, the tracks carrying it (Spotify isrc: search, or Deezer's ISRC lookup);
// - otherwise text: Spotify (field-filtered search, ≤ 10 hits) when signed in; Deezer (plain search, 25 hits) when
//   Spotify isn't available or has nothing Likely; a second search with the version words when the line names a
//   version and nothing compatible came back — at most 2 searches per source;
// - every hit is scored, never "first hit wins": title 0.40, artist 0.30, length 0.20, album 0.05, label 0.05, with
//   missing evidence leaving the denominator; the length counts only once the versions agree (1 within ±3 s, then
//   exp(−0.1·(Δ−3))); a version conflict scores 0 (kept as an "other version" alternative); DJ-mix segments ×0.3
//   and derivative junk ("slowed", "nightcore", …) ×0.2 unless the line asks for them; the same title by another
//   artist ×0.5. DJ versions (remix, extended, dub, edit) are never penalised as words.
//
// Threading: lives on the meta thread; futures finish there. Ownership: the clients must outlive the resolver; either
// may be null (not configured). Failures: InvalidArgument for a line with neither artist nor title nor ISRC; when every
// source failed, the first source's error. Cancelling the future cancels the searches in flight.
class ReleaseResolver : public QObject
{
    Q_OBJECT

public:
    ReleaseResolver(SpotifyClient *spotify, DeezerClient *deezer, std::shared_ptr<const TextMatcher> matcher,
                    QObject *parent = nullptr);
    ~ReleaseResolver() override;
    ReleaseResolver(const ReleaseResolver &) = delete;
    ReleaseResolver &operator=(const ReleaseResolver &) = delete;
    ReleaseResolver(ReleaseResolver &&) = delete;
    ReleaseResolver &operator=(ReleaseResolver &&) = delete;

    QFuture<Result<Resolution>> resolve(const ResolveQuery &query);

    // Scores one release against a line (no tier margin applied). Thread-safe (pure).
    static Candidate score(const ResolveQuery &query, const Release &release, const TextMatcher &matcher);
    // Scores, de-duplicates (same ISRC and version: the first source wins), sorts and assigns tiers, including the
    // Confident margin over the best *other* recording or version. Thread-safe (pure).
    static Resolution rank(const ResolveQuery &query, const QList<Release> &releases, const TextMatcher &matcher);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace selecta::meta
