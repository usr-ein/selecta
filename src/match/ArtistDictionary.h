// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <cstddef>
#include <memory>

namespace selecta::match {

namespace detail {
struct ArtistDictionaryData;
}

// Known artist names, for the parser's "which segment is the artist?" evidence (note §4.1 step 6, §17.5 A): the
// collection's artists weighted by their track counts (MatchIndex builds one), or the artists of the current search's
// release candidates (the caller builds a small one per search, SRCH-17).
//
// Threading: immutable once constructed; const members may be called concurrently from any thread. Move-only; share
// it through a const owner or std::shared_ptr<const ArtistDictionary>.
// Failure modes: none. Empty names are ignored; weights below 1 count as 1.
class ArtistDictionary final
{
public:
    struct Entry
    {
        QString name;   // as written ("Röyksopp", "Above & Beyond", "Bicep feat. X")
        int weight = 1; // evidence strength, e.g. the number of owned tracks
    };

    struct Hit
    {
        int weight = 0; // 0 = not found
        int edits = 0;  // 0 for an exact hit on the key, its squashed form or one member of the artist set
        bool found() const { return weight > 0; }
        bool exact() const { return found() && edits == 0; }
    };

    ArtistDictionary(); // empty
    explicit ArtistDictionary(const QList<Entry> &entries);
    ~ArtistDictionary();
    ArtistDictionary(const ArtistDictionary &) = delete;
    ArtistDictionary &operator=(const ArtistDictionary &) = delete;
    ArtistDictionary(ArtistDictionary &&) noexcept;
    ArtistDictionary &operator=(ArtistDictionary &&) noexcept;

    // Looks up an artist as written: the whole squashed artist key, then each member of its artist set, exactly;
    // then fuzzily within the token rules and field budget (§17.3). Returns the best hit (exact before fuzzy, then
    // the highest weight).
    Hit lookup(QStringView artist) const;

    qsizetype size() const;
    bool isEmpty() const { return size() == 0; }
    std::size_t memoryUsage() const;

private:
    std::unique_ptr<detail::ArtistDictionaryData> m_data;
};

} // namespace selecta::match
