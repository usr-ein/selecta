// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CollectionBrowse.h"

#include "match/Quality.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QMap>

#include <algorithm>
#include <limits>
#include <tuple>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

QString translated(const char *text)
{
    return QCoreApplication::translate("selecta::core::CollectionBrowse", text);
}

// Where a section sorts: by rank, then by key.
struct SectionId
{
    int rank = 0;
    QString key;

    friend bool operator<(const SectionId &a, const SectionId &b) { return std::tie(a.rank, a.key) < std::tie(b.rank, b.key); }
};

// What a track brings to its tile's place in a section.
struct TrackFacts
{
    QString artistKey; // its album's artist for an album track: a compilation stays whole
    QString titleKey;  // its album's title for an album track
};

struct TileFacts
{
    QString artistKey;
    QString titleKey;
    qint64 newestMs = 0;
    int firstTrack = 0;
};

constexpr int kLastRank = std::numeric_limits<int>::max();

// "October 2026": the month's name as it stands alone, capitalised for a heading.
QString monthTitle(QDate date)
{
    const QLocale locale;
    QString month = locale.standaloneMonthName(date.month(), QLocale::LongFormat);
    if (!month.isEmpty()) {
        month[0] = month.at(0).toUpper();
    }
    return u"%1 %2"_s.arg(month).arg(date.year());
}

// The initial a name files under: its first letter, upper case, accents dropped; "#" for anything else.
QString initialOf(const QString &key)
{
    if (key.isEmpty() || !key.at(0).isLetter()) {
        return u"#"_s;
    }
    if (key.at(0).isHighSurrogate() && key.size() > 1) {
        return key.left(2).toUpper(); // a letter outside the BMP
    }
    return QString(key.at(0).toUpper());
}

} // namespace

QualityTier tierOfBitrate(int kbps)
{
    if (kbps <= 0) {
        return QualityTier::Unknown;
    }
    if (kbps >= k320MinimumKbps) {
        return QualityTier::Top;
    }
    if (kbps >= 256) {
        return QualityTier::From256;
    }
    if (kbps >= 192) {
        return QualityTier::From192;
    }
    return QualityTier::Below192;
}

QualityTier qualityTier(const match::AudioQuality &quality, const TierEvidence &evidence)
{
    QualityTier tier = QualityTier::Unknown;
    if (match::isLossless(quality.codec) || evidence.fromLossless) {
        tier = QualityTier::Lossless;
    } else if (quality.codec == match::Codec::Mp3 && quality.vbr.value_or(false) &&
               quality.bitrateKbps >= kV0MinimumKbps) {
        tier = QualityTier::Top;
    } else {
        tier = tierOfBitrate(quality.bitrateKbps);
    }
    if (evidence.likelySourceKbps) {
        // What the file was made from bounds what it is; an unknown quality takes what the check found.
        const QualityTier source = tierOfBitrate(*evidence.likelySourceKbps);
        if (source != QualityTier::Unknown && (tier == QualityTier::Unknown || source < tier)) {
            tier = source;
        }
    }
    return tier;
}

int tierDisplayOrder(QualityTier tier)
{
    switch (tier) {
    case QualityTier::Below192:
        return 0;
    case QualityTier::From192:
        return 1;
    case QualityTier::From256:
        return 2;
    case QualityTier::Top:
        return 3;
    case QualityTier::Lossless:
        return 4;
    case QualityTier::Unknown:
        break;
    }
    return 5;
}

QString tierName(QualityTier tier)
{
    switch (tier) {
    case QualityTier::Below192:
        return translated("Below 192 kbps");
    case QualityTier::From192:
        return translated("192–255 kbps");
    case QualityTier::From256:
        return translated("256–319 kbps");
    case QualityTier::Top:
        return translated("320 kbps or V0");
    case QualityTier::Lossless:
        return translated("From lossless");
    case QualityTier::Unknown:
        break;
    }
    return translated("Quality unknown");
}

bool isLosslessQualityText(QStringView quality)
{
    const QStringView trimmed = quality.trimmed();
    const qsizetype space = trimmed.indexOf(u' ');
    const QStringView format = space < 0 ? trimmed : trimmed.left(space);
    for (const QStringView lossless : {u"FLAC", u"ALAC", u"WAV", u"AIFF", u"PCM"}) {
        if (format.compare(lossless, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

const CollectionAlbum *CollectionListing::album(quint64 id) const
{
    if (id == 0) {
        return nullptr;
    }
    const auto it = std::ranges::find(albums, id, &CollectionAlbum::id);
    return it == albums.cend() ? nullptr : &*it;
}

qsizetype CollectionListing::indexOf(qint64 trackId) const
{
    const auto it = std::ranges::find(tracks, trackId, &CollectionTrack::id);
    return it == tracks.cend() ? -1 : std::distance(tracks.cbegin(), it);
}

QString browseKey(QStringView text)
{
    const QString decomposed = text.toString().normalized(QString::NormalizationForm_KD);
    QString key;
    key.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing) {
            key.append(c);
        }
    }
    return key.toCaseFolded().simplified();
}

QList<CollectionSection> groupCollection(const CollectionListing &listing, CollectionOrder order)
{
    const QList<CollectionTrack> &tracks = listing.tracks;
    QHash<quint64, const CollectionAlbum *> albums;
    albums.reserve(listing.albums.size());
    for (const CollectionAlbum &album : listing.albums) {
        albums.insert(album.id, &album);
    }

    // Each track's section, and what places its tile.
    QList<TrackFacts> facts;
    facts.reserve(tracks.size());
    QMap<SectionId, QList<int>> sections;
    QHash<QString, QHash<QString, int>> genreSpellings; // genre key → spelling → tracks
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const CollectionTrack &track = tracks.at(i);
        const CollectionAlbum *album = track.albumId != 0 ? albums.value(track.albumId) : nullptr;
        TrackFacts fact;
        fact.artistKey = browseKey(album ? album->artist : track.artist);
        fact.titleKey = browseKey(album ? album->title : track.title);
        SectionId section;
        switch (order) {
        case CollectionOrder::Added:
            if (track.addedMs > 0) {
                const QDate date = QDateTime::fromMSecsSinceEpoch(track.addedMs).date();
                section.rank = -(date.year() * 12 + date.month()); // newest first
            } else {
                section.rank = kLastRank;
            }
            break;
        case CollectionOrder::Artist:
            section.key = initialOf(fact.artistKey);
            section.rank = section.key == u"#" ? 1 : 0;
            break;
        case CollectionOrder::Genre:
            section.key = browseKey(track.genre);
            section.rank = section.key.isEmpty() ? 1 : 0;
            if (!section.key.isEmpty()) {
                ++genreSpellings[section.key][track.genre];
            }
            break;
        case CollectionOrder::Quality:
            section.rank = tierDisplayOrder(track.tier);
            break;
        }
        facts.append(std::move(fact));
        sections[section].append(static_cast<int>(i));
    }

    QList<CollectionSection> result;
    result.reserve(sections.size());
    for (auto it = sections.cbegin(); it != sections.cend(); ++it) {
        const SectionId &id = it.key();
        const QList<int> &members = it.value();
        CollectionSection section;
        section.trackCount = static_cast<int>(members.size());
        switch (order) {
        case CollectionOrder::Added:
            if (id.rank == kLastRank) {
                section.title = translated("Date unknown");
            } else {
                const QDate date = QDateTime::fromMSecsSinceEpoch(tracks.at(members.constFirst()).addedMs).date();
                section.title = monthTitle(date);
            }
            break;
        case CollectionOrder::Artist:
            section.title = id.key;
            break;
        case CollectionOrder::Genre:
            if (id.key.isEmpty()) {
                section.title = translated("No genre");
            } else {
                // As most of its tracks spell it; a tie goes to the spelling that sorts first.
                const QHash<QString, int> &spellings = genreSpellings.value(id.key);
                int most = 0;
                for (auto spelling = spellings.cbegin(); spelling != spellings.cend(); ++spelling) {
                    if (spelling.value() > most || (spelling.value() == most && spelling.key() < section.title)) {
                        most = spelling.value();
                        section.title = spelling.key();
                    }
                }
            }
            break;
        case CollectionOrder::Quality:
            section.title = tierName(tracks.at(members.constFirst()).tier);
            break;
        }

        // The tracks of one album make one tile; a loose track is a tile of its own.
        QList<CollectionTile> tiles;
        QList<TileFacts> placing;
        QHash<quint64, qsizetype> albumTile;
        for (const int index : members) {
            const CollectionTrack &track = tracks.at(index);
            const bool inAlbum = track.albumId != 0 && albums.contains(track.albumId);
            qsizetype at = inAlbum ? albumTile.value(track.albumId, -1) : -1;
            if (at < 0) {
                at = tiles.size();
                tiles.append(CollectionTile{.albumId = inAlbum ? track.albumId : 0, .tracks = {}});
                placing.append(TileFacts{.artistKey = facts.at(index).artistKey,
                                         .titleKey = facts.at(index).titleKey,
                                         .newestMs = track.addedMs,
                                         .firstTrack = index});
                if (inAlbum) {
                    albumTile.insert(track.albumId, at);
                }
            }
            tiles[at].tracks.append(index);
            placing[at].newestMs = std::max(placing.at(at).newestMs, track.addedMs);
        }
        for (CollectionTile &tile : tiles) {
            if (tile.albumId == 0) {
                continue;
            }
            std::ranges::sort(tile.tracks, [&tracks](int a, int b) {
                const CollectionTrack &x = tracks.at(a);
                const CollectionTrack &y = tracks.at(b);
                // Unnumbered tracks after the numbered ones of their disc.
                const int xNumber = x.trackNumber > 0 ? x.trackNumber : std::numeric_limits<int>::max();
                const int yNumber = y.trackNumber > 0 ? y.trackNumber : std::numeric_limits<int>::max();
                return std::tie(x.discNumber, xNumber, x.path) < std::tie(y.discNumber, yNumber, y.path);
            });
        }
        QList<qsizetype> tileOrder(tiles.size());
        for (qsizetype i = 0; i < tileOrder.size(); ++i) {
            tileOrder[i] = i;
        }
        const bool newestFirst = order == CollectionOrder::Added;
        std::ranges::sort(tileOrder, [&placing, newestFirst](qsizetype a, qsizetype b) {
            const TileFacts &x = placing.at(a);
            const TileFacts &y = placing.at(b);
            if (newestFirst && x.newestMs != y.newestMs) {
                return x.newestMs > y.newestMs;
            }
            return std::tie(x.artistKey, x.titleKey, x.firstTrack) < std::tie(y.artistKey, y.titleKey, y.firstTrack);
        });
        section.tiles.reserve(tiles.size());
        for (const qsizetype i : std::as_const(tileOrder)) {
            section.tiles.append(std::move(tiles[i]));
        }
        result.append(std::move(section));
    }
    return result;
}

} // namespace selecta::core
