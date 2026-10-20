// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/MusicBrainzClient.h"

#include <QJsonObject>

// MusicBrainz web-service JSON → meta's value types, and the choices of the D30 gap fill (§5.3). Every field is
// optional and type-checked; MBIDs that aren't UUIDs are dropped. Pure functions, thread-safe.
namespace selecta::meta::detail::musicbrainz {

using ::selecta::meta::musicbrainz::Genre;
using ::selecta::meta::musicbrainz::LabelInfo;
using ::selecta::meta::musicbrainz::Recording;
using ::selecta::meta::musicbrainz::ReleaseInfo;

inline constexpr int kMaxRecordings = 100;
inline constexpr int kMaxReleases = 100;
inline constexpr int kMaxGenres = 20;
inline constexpr int kMinSearchScore = 90; // a fallback search hit must score at least this (§5.3)

QList<Recording> recordings(const QJsonObject &object); // "recordings": […] of an ISRC lookup or a search
QList<ReleaseInfo> releases(const QJsonObject &object); // "releases": […] of a browse by recording
QList<Genre> genres(const QJsonObject &object);         // "genres": […], most votes first, zero-vote ones dropped

// True when the recording credits the release's main artist (comparison keys, so "Röyksopp" = "Royksopp").
bool creditsArtist(const Recording &recording, const QStringList &artists);

// The recordings that can be the release's: crediting its main artist and, when both lengths are known, within
// ±3 s; closest length first.
QList<Recording> fittingRecordings(const QList<Recording> &candidates, const Release &release);

// The release a recording appeared on that best matches `release`: barcode = UPC; else title and year; else the
// earliest official release of the same type; never a compilation or DJ mix unless `release` is a compilation.
// `why` receives the rule that chose it, or why none was. Nullopt when no release qualifies.
std::optional<ReleaseInfo> chooseRelease(const QList<ReleaseInfo> &candidates, const Release &release, QString *why);

// The label and catalogue number to use from a release (MusicBrainz's "[no label]" and "[none]" are no answer).
LabelInfo labelOf(const ReleaseInfo &release);

} // namespace selecta::meta::detail::musicbrainz
