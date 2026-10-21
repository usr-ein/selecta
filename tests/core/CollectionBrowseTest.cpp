// SPDX-License-Identifier: GPL-3.0-or-later
// The Collection view's quality tiers and cover wall sections (P-010).
#include "core/CollectionBrowse.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QStringList>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::browseKey;
using selecta::core::CollectionAlbum;
using selecta::core::CollectionListing;
using selecta::core::CollectionOrder;
using selecta::core::CollectionSection;
using selecta::core::CollectionTrack;
using selecta::core::groupCollection;
using selecta::core::isLosslessQualityText;
using selecta::core::qualityTier;
using selecta::core::QualityTier;
using selecta::core::tierDisplayOrder;
using selecta::core::TierEvidence;
using selecta::core::tierName;
using selecta::core::tierOfBitrate;
using selecta::match::AudioQuality;
using selecta::match::Codec;

namespace {

AudioQuality mp3(int kbps, std::optional<bool> vbr = false)
{
    return AudioQuality{.codec = Codec::Mp3, .bitrateKbps = kbps, .vbr = vbr, .sampleRateHz = 44100, .bitDepth = 0};
}

AudioQuality lossless(Codec codec)
{
    return AudioQuality{
        .codec = codec, .bitrateKbps = 1411, .vbr = std::nullopt, .sampleRateHz = 44100, .bitDepth = 16};
}

// Noon on a day of a month, local time: the month it falls in doesn't depend on the time zone.
qint64 on(int year, int month, int day)
{
    return QDateTime(QDate(year, month, day), QTime(12, 0)).toMSecsSinceEpoch();
}

class Listing
{
public:
    // Adds a track; returns its index.
    int track(const QString &artist, const QString &title, QualityTier tier = QualityTier::Top, quint64 album = 0,
              int number = 0, const QString &genre = {}, qint64 addedMs = 0)
    {
        CollectionTrack track;
        track.id = static_cast<qint64>(m_listing.tracks.size()) + 1;
        track.path = u"/Music/%1 - %2.mp3"_s.arg(artist, title);
        track.artist = artist;
        track.title = title;
        track.tier = tier;
        track.albumId = album;
        track.trackNumber = number;
        track.genre = genre;
        track.addedMs = addedMs;
        m_listing.tracks.append(track);
        return static_cast<int>(m_listing.tracks.size()) - 1;
    }

    void album(quint64 id, const QString &artist, const QString &title)
    {
        m_listing.albums.append(CollectionAlbum{.id = id,
                                                .folder = u"/Albums/%1 - %2"_s.arg(artist, title),
                                                .title = title,
                                                .artist = artist,
                                                .year = 2024,
                                                .variousArtists = artist == u"Various Artists"});
    }

    const CollectionListing &get() const { return m_listing; }

private:
    CollectionListing m_listing;
};

QStringList titles(const QList<CollectionSection> &sections)
{
    QStringList result;
    for (const CollectionSection &section : sections) {
        result.append(section.title);
    }
    return result;
}

// A section's tiles as "track, track…" (album tiles in brackets), by the tracks' titles.
QStringList tiles(const CollectionListing &listing, const CollectionSection &section)
{
    QStringList result;
    for (const selecta::core::CollectionTile &tile : section.tiles) {
        QStringList names;
        for (const int index : tile.tracks) {
            names.append(listing.tracks.at(index).title);
        }
        result.append(tile.albumId != 0 ? u"[%1]"_s.arg(names.join(u", "_s)) : names.join(u", "_s));
    }
    return result;
}

} // namespace

// --- Tiers -----------------------------------------------------------------------------------------------------------

TEST(QualityTiers, BitratesFallInTheirTiers)
{
    EXPECT_EQ(tierOfBitrate(-5), QualityTier::Unknown);
    EXPECT_EQ(tierOfBitrate(0), QualityTier::Unknown);
    EXPECT_EQ(tierOfBitrate(1), QualityTier::Below192);
    EXPECT_EQ(tierOfBitrate(128), QualityTier::Below192);
    EXPECT_EQ(tierOfBitrate(191), QualityTier::Below192);
    EXPECT_EQ(tierOfBitrate(192), QualityTier::From192);
    EXPECT_EQ(tierOfBitrate(255), QualityTier::From192);
    EXPECT_EQ(tierOfBitrate(256), QualityTier::From256);
    EXPECT_EQ(tierOfBitrate(314), QualityTier::From256);
    EXPECT_EQ(tierOfBitrate(315), QualityTier::Top); // a 320 measured a little low
    EXPECT_EQ(tierOfBitrate(320), QualityTier::Top);
    EXPECT_EQ(tierOfBitrate(1411), QualityTier::Top);
}

TEST(QualityTiers, CodecsAndV0)
{
    EXPECT_EQ(qualityTier(mp3(320)), QualityTier::Top);
    EXPECT_EQ(qualityTier(mp3(256)), QualityTier::From256);
    EXPECT_EQ(qualityTier(mp3(245, true)), QualityTier::Top); // V0
    EXPECT_EQ(qualityTier(mp3(220, true)), QualityTier::Top);
    EXPECT_EQ(qualityTier(mp3(219, true)), QualityTier::From192);         // V1-class
    EXPECT_EQ(qualityTier(mp3(190, true)), QualityTier::Below192);        // V2-class averages under 192
    EXPECT_EQ(qualityTier(mp3(245, std::nullopt)), QualityTier::From192); // not known to be VBR: its bitrate
    EXPECT_EQ(qualityTier(mp3(0)), QualityTier::Unknown);
    EXPECT_EQ(qualityTier(AudioQuality{.codec = Codec::Aac, .bitrateKbps = 256}), QualityTier::From256);
    EXPECT_EQ(qualityTier(AudioQuality{.codec = Codec::Aac, .bitrateKbps = 245, .vbr = true}), QualityTier::From192);
    EXPECT_EQ(qualityTier(AudioQuality{.codec = Codec::Opus, .bitrateKbps = 160}), QualityTier::Below192);
    EXPECT_EQ(qualityTier(AudioQuality{.codec = Codec::Unknown}), QualityTier::Unknown);
    for (const Codec codec : {Codec::Flac, Codec::Alac, Codec::Wav, Codec::Aiff, Codec::Ape, Codec::WavPack}) {
        EXPECT_EQ(qualityTier(lossless(codec)), QualityTier::Lossless) << static_cast<int>(codec);
    }
}

TEST(QualityTiers, WhatTheDownloadSaid)
{
    // Selecta's MP3 320 made from a FLAC download.
    EXPECT_EQ(qualityTier(mp3(320), TierEvidence{.fromLossless = true}), QualityTier::Lossless);
    // A fake 320, and a FLAC made from an MP3: the tier of what they were made from.
    EXPECT_EQ(qualityTier(mp3(320), TierEvidence{.likelySourceKbps = 128}), QualityTier::Below192);
    EXPECT_EQ(qualityTier(lossless(Codec::Flac), TierEvidence{.likelySourceKbps = 192}), QualityTier::From192);
    EXPECT_EQ(qualityTier(mp3(320), TierEvidence{.fromLossless = true, .likelySourceKbps = 320}), QualityTier::Top);
    // A verdict never raises a tier…
    EXPECT_EQ(qualityTier(mp3(128), TierEvidence{.likelySourceKbps = 320}), QualityTier::Below192);
    EXPECT_EQ(qualityTier(mp3(256), TierEvidence{.likelySourceKbps = 256}), QualityTier::From256);
    // …but says what an unknown quality is; a verdict that isn't a bitrate says nothing.
    EXPECT_EQ(qualityTier(mp3(0), TierEvidence{.likelySourceKbps = 128}), QualityTier::Below192);
    EXPECT_EQ(qualityTier(mp3(320), TierEvidence{.likelySourceKbps = 0}), QualityTier::Top);
    EXPECT_EQ(qualityTier(mp3(320), TierEvidence{.likelySourceKbps = -1}), QualityTier::Top);
}

TEST(QualityTiers, WorstFirstUnknownLastEachNamed)
{
    const QList<QualityTier> shown{QualityTier::Below192, QualityTier::From192,  QualityTier::From256,
                                   QualityTier::Top,      QualityTier::Lossless, QualityTier::Unknown};
    QStringList names;
    for (qsizetype i = 0; i < shown.size(); ++i) {
        EXPECT_EQ(tierDisplayOrder(shown.at(i)), i);
        names.append(tierName(shown.at(i)));
    }
    EXPECT_EQ(names, (QStringList{u"Below 192 kbps"_s, u"192–255 kbps"_s, u"256–319 kbps"_s, u"320 kbps or V0"_s,
                                  u"From lossless"_s, u"Quality unknown"_s}));
    // Ranked: a better tier is greater, and anything known beats an unknown quality.
    EXPECT_LT(QualityTier::Unknown, QualityTier::Below192);
    EXPECT_LT(QualityTier::Below192, QualityTier::From192);
    EXPECT_LT(QualityTier::From256, QualityTier::Top);
    EXPECT_LT(QualityTier::Top, QualityTier::Lossless);
}

TEST(QualityTiers, LosslessDownloadsByTheirQualityText)
{
    for (const QString &yes :
         {u"FLAC 16/44.1"_s, u"flac"_s, u" WAV 24/96 "_s, u"AIFF 16/44.1"_s, u"ALAC 24/96"_s, u"PCM"_s}) {
        EXPECT_TRUE(isLosslessQualityText(yes)) << yes.toStdString();
    }
    for (const QString &no :
         {u"MP3 320"_s, u"MP3 VBR 245"_s, u"AAC 256"_s, u"Opus VBR 160"_s, u""_s, u"FLACX 16/44.1"_s, u"Other"_s}) {
        EXPECT_FALSE(isLosslessQualityText(no)) << no.toStdString();
    }
}

TEST(CollectionBrowse, NamesCompareWithoutCaseOrAccents)
{
    EXPECT_EQ(browseKey(u"Étienne de Crécy"), u"etienne de crecy"_s);
    EXPECT_EQ(browseKey(u"  The   KLF "), u"the klf"_s);
    EXPECT_EQ(browseKey(u"STRAßE"), u"straße"_s);      // simple case folding: ß stays
    EXPECT_EQ(browseKey(u"ﬁve"), u"five"_s);           // a ligature
    EXPECT_EQ(browseKey(u"Ｒöyksopp"), u"royksopp"_s); // full width
    EXPECT_EQ(browseKey(u""), QString());
}

// --- Sections --------------------------------------------------------------------------------------------------------

TEST(CollectionBrowse, AnEmptyCollectionHasNoSection)
{
    const CollectionListing empty;
    for (const CollectionOrder order :
         {CollectionOrder::Added, CollectionOrder::Artist, CollectionOrder::Genre, CollectionOrder::Quality}) {
        EXPECT_TRUE(groupCollection(empty, order).isEmpty());
    }
}

TEST(CollectionBrowse, ByQualityWorstFirstPerTrack)
{
    Listing listing;
    listing.album(7, u"Okuma Drift"_s, u"Nightfall"_s);
    listing.track(u"Okuma Drift"_s, u"Dusk"_s, QualityTier::Top, 7, 2);
    listing.track(u"Okuma Drift"_s, u"Dawn"_s, QualityTier::Below192, 7, 1);
    listing.track(u"Okuma Drift"_s, u"Noon"_s, QualityTier::Top, 7, 1);
    listing.track(u"Halden Rue"_s, u"Tidewater"_s, QualityTier::Below192);
    listing.track(u"Aster Vale"_s, u"Glass"_s, QualityTier::Lossless);
    listing.track(u"Bo Kessel"_s, u"Static"_s, QualityTier::Unknown);
    listing.track(u"aster vale"_s, u"Amber"_s, QualityTier::Below192);
    const CollectionListing &all = listing.get();

    const QList<CollectionSection> sections = groupCollection(all, CollectionOrder::Quality);
    EXPECT_EQ(titles(sections),
              (QStringList{u"Below 192 kbps"_s, u"320 kbps or V0"_s, u"From lossless"_s, u"Quality unknown"_s}));
    // The album has a tile where each of its tracks falls; artists compare without case.
    EXPECT_EQ(tiles(all, sections.at(0)), (QStringList{u"Amber"_s, u"Tidewater"_s, u"[Dawn]"_s}));
    EXPECT_EQ(sections.at(0).trackCount, 3);
    EXPECT_EQ(tiles(all, sections.at(1)), QStringList{u"[Noon, Dusk]"_s}); // in track order
    EXPECT_EQ(sections.at(1).tiles.constFirst().albumId, 7U);
    EXPECT_EQ(tiles(all, sections.at(2)), QStringList{u"Glass"_s});
    EXPECT_EQ(tiles(all, sections.at(3)), QStringList{u"Static"_s});
}

TEST(CollectionBrowse, ByGenreMergesSpellingsAndPutsNoGenreLast)
{
    Listing listing;
    listing.track(u"A"_s, u"One"_s, QualityTier::Top, 0, 0, u"Deep House"_s);
    listing.track(u"B"_s, u"Two"_s, QualityTier::Top, 0, 0, u"deep house"_s);
    listing.track(u"C"_s, u"Three"_s, QualityTier::Top, 0, 0, u"Deep House"_s);
    listing.track(u"D"_s, u"Four"_s, QualityTier::Top, 0, 0, u"Électro"_s);
    listing.track(u"E"_s, u"Five"_s, QualityTier::Top, 0, 0, u"Ambient"_s);
    listing.track(u"F"_s, u"Six"_s);
    listing.album(3, u"G"_s, u"Mixed Bag"_s);
    listing.track(u"G"_s, u"Seven"_s, QualityTier::Top, 3, 1, u"Ambient"_s);
    listing.track(u"G"_s, u"Eight"_s, QualityTier::Top, 3, 2, u"Electro"_s);
    const CollectionListing &all = listing.get();

    const QList<CollectionSection> sections = groupCollection(all, CollectionOrder::Genre);
    // "Électro" and "Electro" are one genre, titled as the first that sorts when they tie.
    EXPECT_EQ(titles(sections), (QStringList{u"Ambient"_s, u"Deep House"_s, u"Electro"_s, u"No genre"_s}));
    EXPECT_EQ(tiles(all, sections.at(0)), (QStringList{u"Five"_s, u"[Seven]"_s}));
    EXPECT_EQ(tiles(all, sections.at(1)), (QStringList{u"One"_s, u"Two"_s, u"Three"_s}));
    EXPECT_EQ(sections.at(1).trackCount, 3);
    EXPECT_EQ(tiles(all, sections.at(2)), (QStringList{u"Four"_s, u"[Eight]"_s}));
    EXPECT_EQ(tiles(all, sections.at(3)), QStringList{u"Six"_s});
}

TEST(CollectionBrowse, ByArtistInitialAlbumsUnderTheirArtist)
{
    Listing listing;
    listing.track(u"Émilie Simon"_s, u"Flowers"_s);
    listing.track(u"bicep"_s, u"Glue"_s);
    listing.track(u"Bicep"_s, u"Atlas"_s);
    listing.track(u"!!!"_s, u"Heart"_s);
    listing.track(u"808 State"_s, u"Pacific"_s);
    listing.track(QString(), u"Untitled"_s);
    listing.track(u"Ólafur Arnalds"_s, u"Saman"_s);
    listing.album(9, u"Various Artists"_s, u"Summer Comp"_s);
    listing.track(u"Zed Bias"_s, u"Neighbourhood"_s, QualityTier::Top, 9, 2);
    listing.track(u"Aphex Twin"_s, u"Xtal"_s, QualityTier::Top, 9, 1);
    const CollectionListing &all = listing.get();

    const QList<CollectionSection> sections = groupCollection(all, CollectionOrder::Artist);
    EXPECT_EQ(titles(sections), (QStringList{u"B"_s, u"E"_s, u"O"_s, u"V"_s, u"#"_s}));
    EXPECT_EQ(tiles(all, sections.at(0)), (QStringList{u"Atlas"_s, u"Glue"_s})); // by artist, then title
    EXPECT_EQ(tiles(all, sections.at(1)), QStringList{u"Flowers"_s});
    EXPECT_EQ(tiles(all, sections.at(2)), QStringList{u"Saman"_s});
    EXPECT_EQ(tiles(all, sections.at(3)), QStringList{u"[Xtal, Neighbourhood]"_s}); // a compilation stays whole
    EXPECT_EQ(tiles(all, sections.at(4)), (QStringList{u"Untitled"_s, u"Heart"_s, u"Pacific"_s}));
}

TEST(CollectionBrowse, ByMonthAddedNewestFirst)
{
    Listing listing;
    listing.track(u"A"_s, u"Older"_s, QualityTier::Top, 0, 0, {}, on(2026, 9, 3));
    listing.track(u"B"_s, u"Newest"_s, QualityTier::Top, 0, 0, {}, on(2026, 10, 20));
    listing.track(u"C"_s, u"Earlier"_s, QualityTier::Top, 0, 0, {}, on(2026, 10, 2));
    listing.track(u"D"_s, u"Unknown"_s);
    listing.track(u"E"_s, u"Last year"_s, QualityTier::Top, 0, 0, {}, on(2025, 10, 30));
    listing.album(4, u"F"_s, u"Album"_s);
    listing.track(u"F"_s, u"Side A"_s, QualityTier::Top, 4, 1, {}, on(2026, 10, 5));
    listing.track(u"F"_s, u"Side B"_s, QualityTier::Top, 4, 2, {}, on(2026, 10, 9));
    const CollectionListing &all = listing.get();

    const QList<CollectionSection> sections = groupCollection(all, CollectionOrder::Added);
    const QLocale locale;
    const auto month = [&locale](int year, int number) {
        QString name = locale.standaloneMonthName(number, QLocale::LongFormat);
        name[0] = name.at(0).toUpper();
        return u"%1 %2"_s.arg(name).arg(year);
    };
    EXPECT_EQ(titles(sections), (QStringList{month(2026, 10), month(2026, 9), month(2025, 10), u"Date unknown"_s}));
    // An album tile is as new as its newest track.
    EXPECT_EQ(tiles(all, sections.at(0)), (QStringList{u"Newest"_s, u"[Side A, Side B]"_s, u"Earlier"_s}));
    EXPECT_EQ(tiles(all, sections.at(1)), QStringList{u"Older"_s});
    EXPECT_EQ(tiles(all, sections.at(3)), QStringList{u"Unknown"_s});
}

TEST(CollectionBrowse, ATrackOfAnUnknownAlbumIsALooseTile)
{
    Listing listing;
    listing.track(u"A"_s, u"Stray"_s, QualityTier::Top, 99, 1); // no album 99 in the listing
    const CollectionListing &all = listing.get();
    const QList<CollectionSection> sections = groupCollection(all, CollectionOrder::Quality);
    ASSERT_EQ(sections.size(), 1);
    ASSERT_EQ(sections.constFirst().tiles.size(), 1);
    EXPECT_EQ(sections.constFirst().tiles.constFirst().albumId, 0U);
    EXPECT_EQ(all.album(99), nullptr);
    EXPECT_EQ(all.indexOf(1), 0);
    EXPECT_EQ(all.indexOf(42), -1);
}

TEST(CollectionBrowse, UnnumberedAlbumTracksComeLast)
{
    Listing listing;
    listing.album(1, u"A"_s, u"Album"_s);
    listing.track(u"A"_s, u"Bonus"_s, QualityTier::Top, 1, 0);
    listing.track(u"A"_s, u"Second"_s, QualityTier::Top, 1, 2);
    listing.track(u"A"_s, u"First"_s, QualityTier::Top, 1, 1);
    const CollectionListing &all = listing.get();
    const QList<CollectionSection> sections = groupCollection(all, CollectionOrder::Artist);
    ASSERT_EQ(sections.size(), 1);
    EXPECT_EQ(tiles(all, sections.constFirst()), QStringList{u"[First, Second, Bonus]"_s});
}

// 20 000 tracks in 2 000 albums and loose ones: grouping is quick enough for the main thread.
TEST(CollectionBrowse, ALargeCollectionGroupsQuickly)
{
    Listing listing;
    for (int i = 0; i < 2000; ++i) {
        listing.album(static_cast<quint64>(i) + 1, u"Artist %1"_s.arg(i % 700), u"Album %1"_s.arg(i));
    }
    for (int i = 0; i < 20'000; ++i) {
        const quint64 album = i % 2 == 0 ? static_cast<quint64>(i % 2000) + 1 : 0;
        listing.track(u"Artist %1"_s.arg(i % 900), u"Title %1"_s.arg(i), static_cast<QualityTier>(i % 6), album,
                      (i % 12) + 1, u"Genre %1"_s.arg(i % 40), on(2020 + (i % 7), (i % 12) + 1, 1));
    }
    QElapsedTimer timer;
    timer.start();
    int tracks = 0;
    for (const CollectionOrder order :
         {CollectionOrder::Added, CollectionOrder::Artist, CollectionOrder::Genre, CollectionOrder::Quality}) {
        const QList<CollectionSection> sections = groupCollection(listing.get(), order);
        for (const CollectionSection &section : sections) {
            tracks += section.trackCount;
        }
    }
    EXPECT_EQ(tracks, 4 * 20'000);        // every track, once per grouping
    EXPECT_LT(timer.elapsed(), 4 * 1000); // generous: sanitizer builds are slow
}
