// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "match/Types.h"

#include <QList>
#include <QString>

#include <cstddef>
#include <memory>

namespace selecta::match {

class ArtistDictionary;

namespace detail {
struct IndexData;
}

// A collection or Inbox track as the library knows it: display values from the tags (or from the filename when the
// file has none) and audio facts from the scanner.
struct LibraryTrack
{
    quint64 id = 0;                     // the caller's id (track/file id), returned in matches; opaque to the index
    Source source = Source::Collection; // Collection or Inbox; History is not valid here
    QString artist;
    QString title; // with its version text, as tagged ("Glue (Original Mix)")
    QString album;
    QString albumArtist;
    qint64 durationMs = 0; // 0 = unknown
    AudioQuality quality;
    quint64 albumId = 0; // the caller's album id (LibraryAlbum::id); 0 = not part of an album
    int trackNumber = 0; // 0 = unknown
    int discNumber = 0;  // 0 = unknown
};

// A local album folder (collection or Inbox; disc folders merged). Its tracks are the LibraryTracks with its id.
struct LibraryAlbum
{
    quint64 id = 0;
    Source source = Source::Collection;
    QString artist;
    QString title;
    int year = 0; // 0 = unknown
    bool variousArtists = false;
};

// A past download (note §13 `download`): matched exactly, never fuzzily (note §1, T20/T21).
struct HistoryEntry
{
    quint64 id = 0;
    QString peer;       // the user it came from
    QString remotePath; // the virtual path exactly as received
    quint64 size = 0;   // bytes, as announced
};

struct MatchIndexInput
{
    QList<LibraryTrack> tracks;
    QList<LibraryAlbum> albums;
    QList<HistoryEntry> history;
    quint64 generation = 0; // the caller's snapshot counter, returned by generation()
};

// An immutable snapshot of the collection, the Inbox and the download history, with precomputed keys (note §10):
// compact track records, token postings with a symmetric-delete fuzzy index (§17.6), album duration signatures (§8),
// the (size, basename) and (peer, path) history maps, and the known-artist dictionary for the parser.
//
// Threading: build() is pure and may take a while (about a second per 100 k tracks; see DESIGN.md): run it on a worker
// thread, then publish the result by swapping a std::shared_ptr<const MatchIndex> atomically. The snapshot never
// changes after build(); every member is const and may be used from any number of threads at once. There are no
// caches inside.
// Ownership: shared; Matcher and AlbumMatcher keep the snapshot alive through their std::shared_ptr.
// Failure modes: none. Input fields are untrusted: strings are cut to Normalizer::kMaxFieldLength, at most
// kMaxRecords tracks and kMaxRecords history entries are indexed (the rest are counted in droppedRecords()). Records
// with an empty title are kept but can only be found through their album; tracks longer than 20 minutes (mixes) are
// not used for track matching. Duplicate ids are kept as separate records.
class MatchIndex final
{
public:
    static constexpr qsizetype kMaxRecords = 4'000'000;

    static std::shared_ptr<const MatchIndex> build(const MatchIndexInput &input);

    ~MatchIndex();
    MatchIndex(const MatchIndex &) = delete;
    MatchIndex &operator=(const MatchIndex &) = delete;
    MatchIndex(MatchIndex &&) = delete;
    MatchIndex &operator=(MatchIndex &&) = delete;

    quint64 generation() const;
    int normVersion() const; // kNormVersion of the build

    qsizetype trackCount() const;
    qsizetype albumCount() const;
    qsizetype historyCount() const;
    qsizetype vocabularySize() const;
    qsizetype droppedRecords() const;

    // The collection's and Inbox's artists, weighted by track count, for RemotePathParser (ParseContext).
    const ArtistDictionary &artists() const;

    // Approximate heap use in bytes.
    std::size_t memoryUsage() const;

private:
    friend class Matcher;
    friend class AlbumMatcher;

    explicit MatchIndex(std::unique_ptr<detail::IndexData> data);
    const detail::IndexData &data() const { return *m_data; }

    std::unique_ptr<const detail::IndexData> m_data;
};

} // namespace selecta::match
