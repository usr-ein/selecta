// SPDX-License-Identifier: GPL-3.0-or-later
// The Collection view (P-010): the cover wall's model, layout, keys and covers, the album page, the view's folders,
// grouping and "Search for Better Quality", and the Inbox's mark on upgrades.
#include "app/views/CollectionView.h"

#include "SyntheticAudio.h"
#include "app/models/CollectionWallModel.h"
#include "app/views/CollectionAlbumView.h"
#include "app/views/CollectionCovers.h"
#include "app/views/CollectionWall.h"
#include "app/views/InboxView.h"
#include "convert/TagWriter.h"
#include "core/EngineHost.h"
#include "storage/Database.h"

#include <QAccessible>
#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTreeView>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::CollectionAlbumView;
using selecta::app::CollectionCovers;
using selecta::app::CollectionView;
using selecta::app::CollectionWall;
using selecta::app::CollectionWallModel;
using selecta::core::CollectionAlbum;
using selecta::core::CollectionListing;
using selecta::core::CollectionOrder;
using selecta::core::CollectionTrack;
using selecta::core::QualityTier;
using selecta::match::AudioQuality;
using selecta::match::Codec;
namespace synth = selecta::testing;

namespace {

AudioQuality mp3(int kbps)
{
    return AudioQuality{.codec = Codec::Mp3, .bitrateKbps = kbps, .vbr = false, .sampleRateHz = 44100, .bitDepth = 0};
}

qint64 on(int year, int month, int day)
{
    return QDateTime(QDate(year, month, day), QTime(12, 0)).toMSecsSinceEpoch();
}

// A collection to show: tracks and albums, built by hand.
class Listing
{
public:
    qint64 track(const QString &artist, const QString &title, QualityTier tier = QualityTier::Top, quint64 album = 0,
                 int number = 0, qint64 addedMs = 0, AudioQuality quality = mp3(320))
    {
        CollectionTrack track;
        track.id = m_nextId++;
        track.path = u"/Music/%1/%2 - %3.mp3"_s.arg(album != 0 ? u"Albums"_s : u"Tracks"_s, artist, title);
        track.artist = artist;
        track.title = title;
        track.tier = tier;
        track.albumId = album;
        track.trackNumber = number;
        track.addedMs = addedMs;
        track.quality = quality;
        track.durationMs = 372'000;
        m_listing.tracks.append(track);
        return track.id;
    }

    void album(quint64 id, const QString &artist, const QString &title, int year = 2024)
    {
        m_listing.albums.append(CollectionAlbum{.id = id,
                                                .folder = u"/Music/Albums/%1 - %2"_s.arg(artist, title),
                                                .title = title,
                                                .artist = artist,
                                                .year = year,
                                                .variousArtists = false});
    }

    std::shared_ptr<const CollectionListing> get() const { return std::make_shared<const CollectionListing>(m_listing); }

private:
    CollectionListing m_listing;
    qint64 m_nextId = 1;
};

// Two months: October with five loose tracks and an album of two, September with two loose tracks.
Listing twoMonths()
{
    Listing listing;
    listing.album(7, u"Okuma Drift"_s, u"Nightfall"_s);
    listing.track(u"Aster Vale"_s, u"Glass"_s, QualityTier::Lossless, 0, 0, on(2026, 10, 20));
    listing.track(u"Bo Kessel"_s, u"Static Bloom"_s, QualityTier::From192, 0, 0, on(2026, 10, 19), mp3(192));
    listing.track(u"Halden Rue"_s, u"Tidewater"_s, QualityTier::Below192, 0, 0, on(2026, 10, 18), mp3(128));
    listing.track(u"Kassia Moreau"_s, u"Night Transit"_s, QualityTier::Top, 0, 0, on(2026, 10, 17));
    listing.track(u"Marlo Vance"_s, u"Tidal Memory"_s, QualityTier::From256, 0, 0, on(2026, 10, 16), mp3(256));
    listing.track(u"Okuma Drift"_s, u"Dusk"_s, QualityTier::Top, 7, 1, on(2026, 10, 15));
    listing.track(u"Okuma Drift"_s, u"Dawn"_s, QualityTier::Below192, 7, 2, on(2026, 10, 15), mp3(128));
    listing.track(u"Nadia Fenn"_s, u"Copper Sky"_s, QualityTier::Top, 0, 0, on(2026, 9, 10));
    listing.track(u"Theo Saltmarsh"_s, u"Low Tide"_s, QualityTier::Below192, 0, 0, on(2026, 9, 8), mp3(160));
    return listing;
}

// Text as a tooltip shows it: rich text read as Qt would.
QString shownToolTip(const QString &tip)
{
    if (!Qt::mightBeRichText(tip)) {
        return tip;
    }
    QTextDocument document;
    document.setHtml(tip);
    return document.toPlainText();
}

QByteArray jpeg(QSize size, QColor colour)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 95);
    return bytes;
}

// An MP3 with `picture` embedded as its cover, filed as `type` (3: front cover, 0: other).
bool writeTrackWithCover(const QString &path, const QByteArray &picture, int type = 3)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    const QByteArray audio = synth::makeMp3(synth::sine(440.0, 1.0));
    if (!file.open(QIODevice::WriteOnly) || file.write(audio) != audio.size()) {
        return false;
    }
    file.close();
    selecta::convert::MetadataModel tags;
    tags.set(selecta::convert::keys::Title, u"Neon Rain"_s, selecta::convert::Provenance::User);
    if (!picture.isEmpty()) {
        selecta::convert::Picture cover;
        cover.data = picture;
        cover.mimeType = u"image/jpeg"_s;
        cover.type = type;
        tags.setPictures({cover});
    }
    return selecta::convert::writeTags(path, tags).ok();
}

// The colour at the middle of an image.
QColor middle(const QImage &image)
{
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

bool near(QColor a, QColor b)
{
    return std::abs(a.red() - b.red()) < 24 && std::abs(a.green() - b.green()) < 24 && std::abs(a.blue() - b.blue()) < 24;
}

} // namespace

// --- The model -------------------------------------------------------------------------------------------------------

TEST(CollectionWallModel, HeadersThenTilesPerSection)
{
    CollectionWallModel model;
    model.setListing(twoMonths().get());
    // October: header + 5 loose + the album; September: header + 2.
    ASSERT_EQ(model.rowCount(), 10);
    const auto kind = [&model](int row) {
        return static_cast<CollectionWallModel::Kind>(model.index(row).data(CollectionWallModel::KindRole).toInt());
    };
    EXPECT_EQ(kind(0), CollectionWallModel::Kind::Header);
    EXPECT_EQ(kind(1), CollectionWallModel::Kind::Tile);
    EXPECT_EQ(kind(7), CollectionWallModel::Kind::Header);
    EXPECT_EQ(model.index(0).data(CollectionWallModel::SubtitleRole).toString(), u"7 tracks"_s);
    EXPECT_EQ(model.index(7).data(CollectionWallModel::SubtitleRole).toString(), u"2 tracks"_s);
    // A header reads like a row, but can't be selected.
    EXPECT_TRUE(model.flags(model.index(0)) & Qt::ItemIsEnabled);
    EXPECT_FALSE(model.flags(model.index(0)) & Qt::ItemIsSelectable);
    EXPECT_TRUE(model.flags(model.index(1)) & Qt::ItemIsSelectable);
    EXPECT_EQ(model.index(1).data().toString(), u"Glass"_s); // newest first
    EXPECT_EQ(model.index(8).data().toString(), u"Copper Sky"_s);
}

TEST(CollectionWallModel, ALooseTracksTile)
{
    CollectionWallModel model;
    model.setListing(twoMonths().get());
    const QModelIndex tidewater = model.index(3);
    ASSERT_EQ(tidewater.data().toString(), u"Tidewater"_s);
    EXPECT_EQ(tidewater.data(CollectionWallModel::SubtitleRole).toString(), u"Halden Rue"_s);
    EXPECT_EQ(tidewater.data(CollectionWallModel::DetailRole).toString(), u"MP3 128"_s);
    EXPECT_EQ(tidewater.data(CollectionWallModel::TierRole).toInt(), static_cast<int>(QualityTier::Below192));
    EXPECT_EQ(tidewater.data(CollectionWallModel::TrackIdsRole).value<QList<qint64>>(), QList<qint64>{3});
    EXPECT_EQ(tidewater.data(CollectionWallModel::AlbumIdRole).toULongLong(), 0U);
    EXPECT_EQ(tidewater.data(CollectionWallModel::CoverKeyRole).toString(), u"track:3"_s);
    EXPECT_TRUE(tidewater.data(CollectionWallModel::CoverFolderRole).toString().isEmpty());
    EXPECT_EQ(tidewater.data(CollectionWallModel::CoverFileRole).toString(), u"/Music/Tracks/Halden Rue - Tidewater.mp3"_s);
    EXPECT_EQ(tidewater.data(Qt::AccessibleTextRole).toString(), u"Tidewater, by Halden Rue, MP3 128"_s);
}

TEST(CollectionWallModel, AnAlbumsTileHoldsItsTracksOfTheSection)
{
    CollectionWallModel model;
    model.setListing(twoMonths().get());
    const QModelIndex album = model.index(6);
    ASSERT_EQ(album.data().toString(), u"Nightfall"_s);
    EXPECT_EQ(album.data(CollectionWallModel::SubtitleRole).toString(), u"Okuma Drift"_s);
    EXPECT_EQ(album.data(CollectionWallModel::DetailRole).toString(), u"Album · 2024 · 2 tracks"_s);
    EXPECT_EQ(album.data(CollectionWallModel::TrackIdsRole).value<QList<qint64>>(), (QList<qint64>{6, 7}));
    EXPECT_EQ(album.data(CollectionWallModel::TierRole).toInt(), static_cast<int>(QualityTier::Below192)); // its lowest
    EXPECT_EQ(album.data(CollectionWallModel::CoverKeyRole).toString(), u"album:7"_s);
    EXPECT_EQ(album.data(CollectionWallModel::CoverFolderRole).toString(), u"/Music/Albums/Okuma Drift - Nightfall"_s);
    EXPECT_EQ(album.data(CollectionWallModel::CoverFileRole).toString(), u"/Music/Albums/Okuma Drift - Dusk.mp3"_s);

    // By quality, the album has a tile in each tier its tracks fall in.
    model.setOrder(CollectionOrder::Quality);
    const int first = model.rowOfAlbum(7);
    ASSERT_GE(first, 0);
    EXPECT_EQ(model.index(first).data(CollectionWallModel::DetailRole).toString(), u"Album · 2024 · 1 of 2 tracks"_s);
    EXPECT_EQ(model.rowsWithAny({6, 7}).size(), 2);
}

TEST(CollectionWallModel, QualityWordsSayWhatTheTierAdds)
{
    CollectionTrack track;
    track.quality = mp3(320);
    track.tier = QualityTier::Top;
    EXPECT_EQ(CollectionWallModel::qualityText(track), u"MP3 320"_s);
    track.tier = QualityTier::Lossless; // made from a FLAC download
    EXPECT_EQ(CollectionWallModel::qualityText(track), u"MP3 320 · from lossless"_s);
    track.tier = QualityTier::Below192; // a fake 320
    EXPECT_EQ(CollectionWallModel::qualityText(track), u"MP3 320 · below 192 kbps"_s);
    track.quality = AudioQuality{.codec = Codec::Flac, .sampleRateHz = 44100, .bitDepth = 16};
    track.tier = QualityTier::Lossless;
    EXPECT_EQ(CollectionWallModel::qualityText(track), u"FLAC 16/44.1"_s);
}

// Names come from files: tooltips show them as they are.
TEST(CollectionWallModel, NamesStayPlainText)
{
    const QString name = u"<b>x</b>&y"_s;
    Listing listing;
    listing.track(name, name);
    CollectionWallModel model;
    model.setListing(listing.get());
    const QModelIndex tile = model.index(1);
    EXPECT_EQ(tile.data().toString(), name);
    EXPECT_TRUE(shownToolTip(tile.data(Qt::ToolTipRole).toString()).startsWith(name + u" — "_s + name))
        << tile.data(Qt::ToolTipRole).toString().toStdString();
}

TEST(CollectionWallModel, TheSelectionsTracksOnceInRowOrder)
{
    CollectionWallModel model;
    model.setListing(twoMonths().get());
    EXPECT_EQ(model.tracksOf({model.index(6), model.index(0), model.index(2), model.index(2)}),
              (QList<qint64>{2, 6, 7})); // the header gives none
    EXPECT_EQ(model.rowsWithAny({3, 9}), (QList<int>{3, 9}));
    EXPECT_TRUE(model.rowsWithAny({}).isEmpty());
    EXPECT_EQ(model.rowOfAlbum(42), -1);

    // The same grouping again changes nothing; another resets.
    const QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    model.setOrder(CollectionOrder::Added);
    EXPECT_EQ(resets.size(), 0);
    model.setOrder(CollectionOrder::Artist);
    EXPECT_EQ(resets.size(), 1);
}

// --- The wall --------------------------------------------------------------------------------------------------------

namespace {

class CollectionWallTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        model.setListing(twoMonths().get());
        wall.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // the viewport is the wall's width
        wall.setModel(&model);
        wall.setAttribute(Qt::WA_DontShowOnScreen);
        wall.resize(1000, 700);
        wall.show();
        wall.doItemsLayout();
    }

    QRect rectOf(int row) const { return wall.visualRect(model.index(row)); }

    CollectionCovers covers;
    CollectionWallModel model;
    CollectionWall wall{covers};
};

} // namespace

TEST_F(CollectionWallTest, TilesFillTheRowsUnderTheirHeader)
{
    // (1000 − 2 × 24 + 18) / (150 + 18): 5 columns of (952 − 4 × 18) / 5 = 176.
    EXPECT_EQ(wall.columns(), 5);
    EXPECT_EQ(wall.tileSize().width(), 176);
    const QRect header = rectOf(0);
    EXPECT_EQ(header.left(), CollectionWall::kMargin);
    EXPECT_EQ(header.width(), 1000 - (2 * CollectionWall::kMargin));
    for (int row = 1; row <= 5; ++row) {
        EXPECT_EQ(rectOf(row).top(), header.bottom() + 1 + CollectionWall::kHeaderGap) << row;
        EXPECT_EQ(rectOf(row).left(), CollectionWall::kMargin + ((row - 1) * (176 + CollectionWall::kSpacing)));
        EXPECT_EQ(rectOf(row).size(), wall.tileSize());
    }
    // The sixth wraps; September's header comes below the last row of October.
    EXPECT_EQ(rectOf(6).left(), CollectionWall::kMargin);
    EXPECT_GT(rectOf(6).top(), rectOf(5).bottom());
    EXPECT_GT(rectOf(7).top(), rectOf(6).bottom());
    EXPECT_EQ(rectOf(8).top(), rectOf(7).bottom() + 1 + CollectionWall::kHeaderGap);

    // Narrow: one column, no wider than the widest tile.
    wall.resize(300, 700);
    wall.doItemsLayout();
    EXPECT_EQ(wall.columns(), 1);
    EXPECT_EQ(wall.tileSize().width(), CollectionWall::kMaxTileWidth);
}

TEST_F(CollectionWallTest, WhatIsUnderAPointIsItsRow)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const QRect rect = rectOf(row);
        if (rect.bottom() < wall.viewport()->height()) {
            EXPECT_EQ(wall.indexAt(rect.center()).row(), row) << row;
        }
    }
    EXPECT_FALSE(wall.indexAt(QPoint(5, 5)).isValid()); // the margin
    EXPECT_FALSE(wall.indexAt(rectOf(1).topRight() + QPoint(CollectionWall::kSpacing / 2, 10)).isValid()); // a gap
}

TEST_F(CollectionWallTest, TheKeysMoveBetweenTilesPassingOverHeaders)
{
    wall.setCurrentIndex(model.index(2));
    const auto press = [this](Qt::Key key) {
        QTest::keyClick(&wall, key);
        return wall.currentIndex().row();
    };
    EXPECT_EQ(press(Qt::Key_Right), 3);
    EXPECT_EQ(press(Qt::Key_Left), 2);
    EXPECT_EQ(press(Qt::Key_Down), 6); // the album, below in the second row; nearest column
    EXPECT_EQ(press(Qt::Key_Down), 9); // September's second tile, under the album's column
    EXPECT_EQ(press(Qt::Key_Up), 6);
    EXPECT_EQ(press(Qt::Key_Up), 2);
    EXPECT_EQ(press(Qt::Key_Up), 2); // the top row: nowhere to go
    EXPECT_EQ(press(Qt::Key_End), 9);
    EXPECT_EQ(press(Qt::Key_Left), 8);
    EXPECT_EQ(press(Qt::Key_Left), 6); // September's header passed over
    EXPECT_EQ(press(Qt::Key_Home), 1);
    // ⌘A selects the tiles, not the headers.
    wall.selectAll();
    EXPECT_EQ(wall.selectionModel()->selectedIndexes().size(), 8);
}

TEST_F(CollectionWallTest, ADragFromTheBackgroundSelectsTheTilesItCovers)
{
    const QPoint start(5, rectOf(1).top() + 10);
    const QPoint end = rectOf(3).center();
    QTest::mousePress(wall.viewport(), Qt::LeftButton, {}, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(end), wall.viewport()->mapToGlobal(QPointF(end)), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(wall.viewport(), &move);
    QTest::mouseRelease(wall.viewport(), Qt::LeftButton, {}, end);
    QList<int> rows;
    for (const QModelIndex &index : wall.selectionModel()->selectedIndexes()) {
        rows.append(index.row());
    }
    std::ranges::sort(rows);
    EXPECT_EQ(rows, (QList<int>{1, 2, 3}));
}

TEST_F(CollectionWallTest, AnAlbumOpensOnADoubleClickOrReturn)
{
    QSignalSpy opened(&wall, &CollectionWall::albumOpened);
    QTest::mouseDClick(wall.viewport(), Qt::LeftButton, {}, rectOf(2).center()); // a loose track
    EXPECT_TRUE(opened.isEmpty());
    QTest::mouseDClick(wall.viewport(), Qt::LeftButton, {}, rectOf(6).center());
    ASSERT_EQ(opened.size(), 1);
    EXPECT_EQ(opened.constFirst().constFirst().toULongLong(), 7U);

    wall.setCurrentIndex(model.index(6));
    for (QShortcut *shortcut : wall.findChildren<QShortcut *>()) {
        if (shortcut->key() == QKeySequence(Qt::Key_Return)) {
            Q_EMIT shortcut->activated();
        }
    }
    EXPECT_EQ(opened.size(), 2);
}

// The wall is a list to VoiceOver: a row for each header and tile, with its words.
TEST_F(CollectionWallTest, VoiceOverReadsAList)
{
    QAccessibleInterface *list = QAccessible::queryAccessibleInterface(&wall);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->role(), QAccessible::List);
    ASSERT_EQ(list->childCount(), model.rowCount());
    EXPECT_EQ(list->child(0)->text(QAccessible::Name), u"October 2026, 7 tracks"_s.replace(
                                                            u"October 2026"_s, model.index(0).data().toString()));
    EXPECT_EQ(list->child(3)->text(QAccessible::Name), u"Tidewater, by Halden Rue, MP3 128"_s);
}

TEST_F(CollectionWallTest, PaintingAsksForTheCoversOnScreen)
{
    QSignalSpy ready(&covers, &CollectionCovers::ready);
    const QPixmap shot = wall.grab();
    EXPECT_FALSE(shot.isNull());
    // The files aren't there: each cover comes back as none, and is asked for once.
    EXPECT_TRUE(QTest::qWaitFor([&] { return covers.pending() == 0 && !ready.isEmpty(); }, 5000));
    const auto asked = ready.size();
    EXPECT_TRUE(covers.cover(u"track:3"_s).has_value());
    EXPECT_TRUE(covers.cover(u"track:3"_s)->isNull());
    wall.grab();
    QTest::qWait(50);
    EXPECT_EQ(ready.size(), asked);
}

TEST_F(CollectionWallTest, ANewGroupingIsLaidOutAgain)
{
    model.setOrder(CollectionOrder::Quality);
    wall.doItemsLayout();
    for (int row = 0; row < model.rowCount(); ++row) {
        const bool header =
            model.index(row).data(CollectionWallModel::KindRole).toInt() == static_cast<int>(CollectionWallModel::Kind::Header);
        EXPECT_EQ(rectOf(row).width(), header ? 1000 - (2 * CollectionWall::kMargin) : 176) << row;
    }
}

// --- Covers ----------------------------------------------------------------------------------------------------------

TEST(CollectionCovers, AnEmbeddedCoverIsReadSquareAndBounded)
{
    QTemporaryDir dir;
    const QString front = dir.filePath(u"front.mp3"_s);
    const QString other = dir.filePath(u"other.mp3"_s);
    const QString wide = dir.filePath(u"wide.mp3"_s);
    const QString none = dir.filePath(u"none.mp3"_s);
    ASSERT_TRUE(writeTrackWithCover(front, jpeg({1200, 1200}, Qt::red)));
    ASSERT_TRUE(writeTrackWithCover(other, jpeg({500, 500}, Qt::blue), 0)); // filed as "other"
    ASSERT_TRUE(writeTrackWithCover(wide, jpeg({800, 400}, Qt::green)));
    ASSERT_TRUE(writeTrackWithCover(none, {}));

    const QImage big = CollectionCovers::read({.folder = {}, .file = front});
    ASSERT_FALSE(big.isNull());
    EXPECT_EQ(big.size(), QSize(CollectionCovers::kSide, CollectionCovers::kSide));
    EXPECT_TRUE(near(middle(big), Qt::red));
    EXPECT_TRUE(near(middle(CollectionCovers::read({.folder = {}, .file = other})), Qt::blue));
    const QImage cut = CollectionCovers::read({.folder = {}, .file = wide});
    EXPECT_EQ(cut.width(), cut.height()); // its middle
    EXPECT_TRUE(CollectionCovers::read({.folder = {}, .file = none}).isNull());
    EXPECT_TRUE(CollectionCovers::read({.folder = {}, .file = dir.filePath(u"missing.mp3"_s)}).isNull());
}

TEST(CollectionCovers, AnAlbumsFolderImageComesFirst)
{
    QTemporaryDir dir;
    const QString album = dir.filePath(u"Okuma Drift - Nightfall"_s);
    ASSERT_TRUE(writeTrackWithCover(album + u"/01 Dusk.mp3"_s, jpeg({300, 300}, Qt::red)));
    {
        QFile cover(album + u"/cover.jpg"_s);
        ASSERT_TRUE(cover.open(QIODevice::WriteOnly));
        cover.write(jpeg({300, 300}, Qt::blue));
    }
    EXPECT_TRUE(near(middle(CollectionCovers::read({.folder = album, .file = album + u"/01 Dusk.mp3"_s})), Qt::blue));
    // Not an image after all: the track's own.
    {
        QFile cover(album + u"/cover.jpg"_s);
        ASSERT_TRUE(cover.open(QIODevice::WriteOnly | QIODevice::Truncate));
        cover.write("not a picture");
    }
    EXPECT_TRUE(near(middle(CollectionCovers::read({.folder = album, .file = album + u"/01 Dusk.mp3"_s})), Qt::red));
}

TEST(CollectionCovers, ACoverIsReadOnceAndKept)
{
    QTemporaryDir dir;
    const QString file = dir.filePath(u"a.mp3"_s);
    ASSERT_TRUE(writeTrackWithCover(file, jpeg({400, 400}, Qt::red)));
    CollectionCovers covers;
    QSignalSpy ready(&covers, &CollectionCovers::ready);
    EXPECT_FALSE(covers.cover(u"track:1"_s).has_value());
    covers.request(u"track:1"_s, {.folder = {}, .file = file});
    covers.request(u"track:1"_s, {.folder = {}, .file = file}); // asked twice: read once
    ASSERT_TRUE(ready.wait(5000));
    QTest::qWait(50);
    EXPECT_EQ(ready.size(), 1);
    ASSERT_TRUE(covers.cover(u"track:1"_s).has_value());
    EXPECT_FALSE(covers.cover(u"track:1"_s)->isNull());
    covers.request(u"track:1"_s, {.folder = {}, .file = file}); // kept: nothing to read
    EXPECT_EQ(covers.pending(), 0);

    // The files changed: read again when asked.
    covers.clear();
    EXPECT_FALSE(covers.cover(u"track:1"_s).has_value());
    covers.request(u"track:1"_s, {.folder = {}, .file = file});
    EXPECT_TRUE(ready.wait(5000));
}

TEST(CollectionCovers, RequestsWaitingAreBounded)
{
    CollectionCovers covers;
    for (int i = 0; i < 300; ++i) {
        covers.request(u"track:%1"_s.arg(i), {.folder = {}, .file = u"/nowhere/%1.mp3"_s.arg(i)});
    }
    EXPECT_LE(covers.pending(), CollectionCovers::kMaxWaiting + CollectionCovers::kWorkers);
    EXPECT_TRUE(QTest::qWaitFor([&covers] { return covers.pending() == 0; }, 5000));
    EXPECT_TRUE(covers.cover(u"track:299"_s).has_value()); // the newest are read; the oldest were dropped
    EXPECT_FALSE(covers.cover(u"track:0"_s).has_value());
}

// --- The album page --------------------------------------------------------------------------------------------------

TEST(CollectionAlbumView, AnAlbumsTracksInOrderWithTheirQuality)
{
    Listing listing;
    listing.album(3, u"Various Artists"_s, u"Harbour Sessions"_s, 2025);
    listing.track(u"Nadia Fenn"_s, u"Gull Wing"_s, QualityTier::Below192, 3, 3, 0, mp3(128));
    listing.track(u"Halden Rue"_s, u"Low Sun"_s, QualityTier::Top, 3, 1);
    listing.track(u"Kassia Moreau"_s, u"Breakwater"_s, QualityTier::From192, 3, 2, 0, mp3(192));
    listing.track(u"Elsewhere"_s, u"Loose"_s);
    CollectionCovers covers;
    CollectionAlbumView view(covers);
    EXPECT_FALSE(view.showAlbum(listing.get(), 99));
    ASSERT_TRUE(view.showAlbum(listing.get(), 3));
    EXPECT_EQ(view.albumId(), 3U);
    QTreeView *tracks = view.trackList();
    ASSERT_EQ(tracks->model()->rowCount(), 3);
    const auto at = [tracks](int row, int column) { return tracks->model()->index(row, column).data().toString(); };
    EXPECT_EQ(at(0, CollectionAlbumView::TitleColumn), u"Low Sun"_s);
    EXPECT_EQ(at(1, CollectionAlbumView::TitleColumn), u"Breakwater"_s);
    EXPECT_EQ(at(2, CollectionAlbumView::TitleColumn), u"Gull Wing"_s);
    EXPECT_EQ(at(2, CollectionAlbumView::QualityColumn), u"MP3 128"_s);
    EXPECT_EQ(at(0, CollectionAlbumView::LengthColumn), u"6:12"_s);
    EXPECT_FALSE(tracks->isColumnHidden(CollectionAlbumView::ArtistColumn)); // several artists
    EXPECT_EQ(view.findChild<QLabel *>(u"collection.album.details"_s)->text(),
              u"2025 · 3 tracks · 18:36 · 2 tracks below 320 kbps"_s);

    QSignalSpy selection(&view, &CollectionAlbumView::selectionChanged);
    tracks->selectionModel()->select(tracks->model()->index(2, 0),
                                     QItemSelectionModel::Select | QItemSelectionModel::Rows);
    EXPECT_FALSE(selection.isEmpty());
    EXPECT_EQ(view.selectedTracks(), QList<qint64>{1});
}

// --- The view --------------------------------------------------------------------------------------------------------

namespace {

class CollectionViewTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(db.open(u":memory:"_s));
        view = std::make_unique<CollectionView>(engine, db);
        view->setAttribute(Qt::WA_DontShowOnScreen);
        view->resize(1100, 800);
        view->show();
        selecta::core::CollectionStatus status;
        status.tracksFolder = u"/Music/Tracks"_s;
        status.albumsFolder = u"/Music/Albums"_s;
        status.roots = {status.tracksFolder, status.albumsFolder};
        status.tracks = 9;
        Q_EMIT engine.collectionStatusChanged(status);
        Q_EMIT engine.collectionListingChanged(twoMonths().get());
    }

    template <typename Widget>
    Widget *child(const QString &name) const
    {
        auto *widget = view->findChild<Widget *>(name);
        EXPECT_NE(widget, nullptr) << name.toStdString();
        return widget;
    }

    selecta::core::EngineHost engine{selecta::core::EngineOptions{}};
    selecta::storage::Database db;
    std::unique_ptr<CollectionView> view;
};

} // namespace

TEST_F(CollectionViewTest, ItSaysWhichFolderHoldsWhat)
{
    EXPECT_EQ(child<QLabel>(u"collection.folders"_s)->text(),
              u"Tracks folder: %1 · Albums folder: %2"_s.arg(QDir::toNativeSeparators(u"/Music/Tracks"_s),
                                                             QDir::toNativeSeparators(u"/Music/Albums"_s)));
    selecta::core::CollectionStatus status;
    status.albumsFolder = u"/Music/Albums"_s;
    status.roots = {status.albumsFolder};
    Q_EMIT engine.collectionStatusChanged(status);
    EXPECT_TRUE(child<QLabel>(u"collection.folders"_s)->text().startsWith(u"Tracks folder: not chosen · "_s));
    // Under the home folder, as the Finder says it.
    status.tracksFolder = QDir::homePath() + u"/Music/Tracks"_s;
    Q_EMIT engine.collectionStatusChanged(status);
    EXPECT_TRUE(child<QLabel>(u"collection.folders"_s)->text().startsWith(
        u"Tracks folder: %1 · "_s.arg(QDir::toNativeSeparators(u"~/Music/Tracks"_s))));
}

TEST_F(CollectionViewTest, TheWallShowsTheCollectionGroupedAsChosen)
{
    CollectionWallModel *model = view->wallModel();
    ASSERT_EQ(model->rowCount(), 10);
    EXPECT_EQ(model->order(), CollectionOrder::Added);
    // What is selected stays selected in another grouping.
    view->wall()->selectionModel()->select(model->index(3), QItemSelectionModel::Select); // Tidewater
    auto *order = child<QComboBox>(u"collection.order"_s);
    order->setCurrentIndex(order->findData(static_cast<int>(CollectionOrder::Quality)));
    EXPECT_EQ(model->order(), CollectionOrder::Quality);
    EXPECT_EQ(model->index(0).data().toString(), u"Below 192 kbps"_s);
    EXPECT_EQ(view->selectedTracks(), QList<qint64>{3});
    // And when the collection changes.
    Q_EMIT engine.collectionListingChanged(twoMonths().get());
    EXPECT_EQ(view->selectedTracks(), QList<qint64>{3});
}

TEST_F(CollectionViewTest, SearchingForBetterQualityAsksForTheTracksSelected)
{
    auto *button = child<QPushButton>(u"collection.betterQuality"_s);
    EXPECT_FALSE(button->isEnabled()); // nothing selected
    QSignalSpy asked(view.get(), &CollectionView::betterQualityRequested);
    // Glass is from lossless already: nothing is better.
    view->wall()->selectionModel()->select(view->wallModel()->index(1), QItemSelectionModel::Select);
    EXPECT_FALSE(button->isEnabled());
    // With the album, two more tracks.
    view->wall()->selectionModel()->select(view->wallModel()->index(6), QItemSelectionModel::Select);
    EXPECT_TRUE(button->isEnabled());
    EXPECT_EQ(child<QLabel>(u"collection.selection"_s)->text(), u"2 selected · 3 tracks"_s);
    button->click();
    ASSERT_EQ(asked.size(), 1);
    EXPECT_EQ(asked.constFirst().constFirst().value<QList<qint64>>(), (QList<qint64>{1, 6, 7}));

    // What the engine did, in words.
    Q_EMIT engine.betterQualitySearched(2, 0, 1, 0);
    EXPECT_EQ(child<QLabel>(u"collection.result"_s)->text(),
              u"Looking for better copies of 2 tracks in Get tracks; the Inbox marks what comes as an upgrade. 1 is "
              "from lossless already."_s);
}

TEST_F(CollectionViewTest, AnAlbumOpensWithItsTracksAndEscapeGoesBack)
{
    CollectionWall *wall = view->wall();
    Q_EMIT wall->albumOpened(7);
    auto *album = child<CollectionAlbumView>(u"collection.album"_s);
    EXPECT_TRUE(album->isVisible());
    ASSERT_EQ(album->trackList()->model()->rowCount(), 2);
    EXPECT_TRUE(album->trackList()->isColumnHidden(CollectionAlbumView::ArtistColumn)); // one artist
    // Its selection is what the button acts on.
    album->trackList()->selectionModel()->select(album->trackList()->model()->index(1, 0),
                                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
    EXPECT_EQ(view->selectedTracks(), QList<qint64>{7});
    EXPECT_TRUE(child<QPushButton>(u"collection.betterQuality"_s)->isEnabled());

    // A new listing keeps it open; one without it goes back to the wall.
    Q_EMIT engine.collectionListingChanged(twoMonths().get());
    EXPECT_TRUE(album->isVisible());
    Listing without;
    without.track(u"Aster Vale"_s, u"Glass"_s);
    Q_EMIT engine.collectionListingChanged(without.get());
    EXPECT_FALSE(album->isVisible());
    EXPECT_TRUE(wall->isVisible());

    // Back, from the album to the wall, its tile current.
    Q_EMIT engine.collectionListingChanged(twoMonths().get());
    Q_EMIT wall->albumOpened(7);
    Q_EMIT album->backRequested();
    EXPECT_TRUE(wall->isVisible());
    EXPECT_EQ(wall->currentIndex().data(CollectionWallModel::AlbumIdRole).toULongLong(), 7U);
}

TEST_F(CollectionViewTest, AnEmptyCollectionSaysSo)
{
    Q_EMIT engine.collectionListingChanged(std::make_shared<const CollectionListing>());
    EXPECT_EQ(view->wallModel()->rowCount(), 0);
    EXPECT_FALSE(view->wall()->grab().isNull()); // paints its words, no row
}

// --- The Inbox's mark ------------------------------------------------------------------------------------------------

TEST(InboxUpgrades, AnUpgradeIsMarkedAndSaysWhetherItCanReplaceItsFile)
{
    selecta::core::EngineHost engine{selecta::core::EngineOptions{}};
    selecta::app::InboxView inbox(engine);
    selecta::core::InboxEntry upgrade;
    upgrade.item.id = 1;
    upgrade.item.kind = selecta::library::InboxItem::Kind::Single;
    upgrade.item.state = selecta::library::InboxItem::State::Ready;
    upgrade.item.relativePath = u"Singles/Okuma Drift - Dusk.mp3"_s;
    upgrade.item.title = u"Dusk"_s;
    upgrade.item.replaces = u"/Music/Albums/Okuma Drift - Nightfall/01 Dusk.mp3"_s;
    selecta::core::InboxEntry mismatch = upgrade;
    mismatch.item.id = 2;
    mismatch.item.relativePath = u"Singles/Okuma Drift - Dawn.mp3"_s;
    mismatch.item.replaces = u"/Music/Tracks/Okuma Drift - Dawn.m4a"_s;
    Q_EMIT engine.inboxChanged({upgrade, mismatch});
    auto *items = inbox.findChild<QTreeView *>(u"inbox.items"_s);
    ASSERT_NE(items, nullptr);
    const QModelIndex singles = items->model()->index(0, 0);
    ASSERT_EQ(items->model()->rowCount(singles), 2);
    QStringList statuses;
    QStringList tips;
    for (int row = 0; row < 2; ++row) {
        statuses.append(items->model()->index(row, 4, singles).data().toString());
        tips.append(shownToolTip(items->model()->index(row, 4, singles).data(Qt::ToolTipRole).toString()));
    }
    statuses.sort();
    EXPECT_EQ(statuses, (QStringList{u"Upgrade"_s, u"⚠ Upgrade, can't replace"_s}));
    EXPECT_TRUE(std::ranges::any_of(tips, [](const QString &tip) { return tip.contains(u"moves that file to the Trash"_s); }));
    EXPECT_TRUE(std::ranges::any_of(tips, [](const QString &tip) { return tip.contains(u"(M4A, not MP3)"_s); }));
}
