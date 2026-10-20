// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/AlbumArrival.h"

#include "SyntheticAudio.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "convert/TagWriter.h"
#include "library/Naming.h"
#include "library/Quarantine.h"
#include "support/QuarantineSupport.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::AlbumArrivalJob;
using selecta::core::AlbumArrivalResult;
using selecta::core::arriveAlbum;
using selecta::library::InboxItem;
namespace synth = selecta::testing;
namespace keys = selecta::convert::keys;
namespace naming = selecta::library::naming;

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray jpeg(int size)
{
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(QColor(40, 90, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return bytes;
}

QByteArray flac(double frequency, double seconds)
{
    return synth::makeFlac(synth::quantised(synth::sine(frequency, seconds), 16));
}

// A track of "Night Signals", as Spotify lists it.
selecta::meta::Release nightSignals(int number, const QString &title, qint64 durationMs)
{
    selecta::meta::Release release;
    release.source = selecta::meta::Source::Spotify;
    release.title = title;
    release.artists = {u"Okuma Drift"_s};
    release.album = u"Night Signals"_s;
    release.albumArtists = {u"Okuma Drift"_s};
    release.releaseDate = u"2024-03-08"_s;
    release.trackNumber = number;
    release.trackTotal = 3;
    release.durationMs = durationMs;
    release.spotifyAlbumId = u"0000000000000000000a01"_s;
    return release;
}

class AlbumArrivalTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        job.albumId = 7;
        job.library.inboxRoot = dir.filePath(u"Inbox"_s);
        ASSERT_TRUE(QDir().mkpath(job.library.inboxRoot));
        job.stagingDir = dir.filePath(u"staging"_s);
        job.encodedBy = u"Selecta 9.9.9"_s;
        job.peer = u"vinyl_ghost"_s;
        job.folderName = u"Okuma Drift - Night Signals (2024) [FLAC]"_s;
        job.trash = [this](const QString &path, QString *to) {
            trashed.append(path);
            *to = dir.filePath(u"Trash/"_s + QFileInfo(path).fileName());
            QDir().mkpath(dir.filePath(u"Trash"_s));
            return QFile::rename(path, *to);
        };
    }

    // A downloaded file of the album, named as the peer named it.
    void add(qint64 id, const QString &name, const QByteArray &bytes)
    {
        const QString path = dir.filePath(u"incoming/%1/%2"_s.arg(id).arg(name));
        ASSERT_TRUE(writeFile(path, bytes));
        job.files.append({.downloadId = id, .localPath = path, .remoteName = name});
    }

    QString inboxPath(const QString &relative) const { return dir.filePath(u"Inbox/"_s + relative); }

    QString trackName(const QString &number, const QString &title, const QString &extension) const
    {
        naming::Fields fields;
        fields.track = number;
        fields.title = title;
        return naming::componentName(job.library.templates.albumTrack, fields, job.library.cleaner, extension);
    }

    QTemporaryDir dir;
    AlbumArrivalJob job;
    QStringList trashed;
};

} // namespace

TEST_F(AlbumArrivalTest, MakesOneTaggedAlbumFolderFromTheTrackList)
{
    add(1, u"01 - Okuma Drift - Neon Rain.flac"_s, flac(440.0, 3.0));
    add(2, u"02 - Okuma Drift - Low Tide.flac"_s, flac(330.0, 4.0));
    add(3, u"cover.jpg"_s, jpeg(1000));
    job.release = nightSignals(1, u"Neon Rain"_s, 3000);
    job.tracks = {nightSignals(1, u"Neon Rain"_s, 3000), nightSignals(2, u"Low Tide"_s, 4000),
                  nightSignals(3, u"Drift Away"_s, 5000)};

    const AlbumArrivalResult result = arriveAlbum(job, nullptr);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.complete);
    EXPECT_TRUE(result.warning.isEmpty()) << result.warning.toStdString();

    // One album in Inbox/Albums, named after the release.
    naming::Fields folder;
    folder.albumArtist = u"Okuma Drift"_s;
    folder.album = u"Night Signals"_s;
    folder.year = u"2024"_s;
    const QString folderName = naming::componentName(job.library.templates.albumFolder, folder, job.library.cleaner);
    const InboxItem &item = result.item;
    EXPECT_EQ(item.relativePath, u"Albums/"_s + folderName);
    EXPECT_EQ(item.kind, InboxItem::Kind::Album);
    EXPECT_EQ(item.title, u"Night Signals"_s);
    EXPECT_EQ(item.artist, u"Okuma Drift"_s);
    EXPECT_EQ(item.year, 2024);
    EXPECT_EQ(item.trackCount, 2);
    EXPECT_EQ(item.format, u"MP3"_s);
    EXPECT_EQ(item.bitrate, 320);
    EXPECT_EQ(item.sourceFormat, u"FLAC 16/44.1"_s);
    EXPECT_EQ(item.tagSource, u"spotify"_s);
    EXPECT_EQ(item.peer, u"vinyl_ghost"_s);

    // Each file matched to its track, in album order.
    ASSERT_EQ(result.tracks.size(), 2);
    EXPECT_EQ(result.tracks.at(0).title, u"Neon Rain"_s);
    EXPECT_EQ(result.tracks.at(0).releaseTrack, 1U);
    EXPECT_EQ(result.tracks.at(0).sourceQuality, u"FLAC 16/44.1"_s);
    EXPECT_NEAR(static_cast<double>(result.tracks.at(0).durationMs), 3000.0, 50.0);
    EXPECT_EQ(result.tracks.at(1).title, u"Low Tide"_s);
    EXPECT_EQ(result.tracks.at(1).releaseTrack, 2U);

    // MP3 320, tagged from the track list, the folder's image as the cover, saved as cover.jpg too.
    const QDir album(inboxPath(item.relativePath));
    const QString first = trackName(u"1"_s, u"Neon Rain"_s, u"mp3"_s);
    EXPECT_EQ(album.entryList(QDir::Files, QDir::Name),
              QStringList({first, trackName(u"2"_s, u"Low Tide"_s, u"mp3"_s), u"cover.jpg"_s}));
    const auto quality = selecta::convert::probeSourceQuality(album.filePath(first), false);
    ASSERT_TRUE(quality.ok());
    EXPECT_EQ(quality.value().codec, selecta::audio::Codec::Mp3);
    EXPECT_EQ(quality.value().bitrateKbps, 320);
    const auto tags = selecta::convert::readTags(album.filePath(first));
    ASSERT_TRUE(tags.ok());
    EXPECT_EQ(tags.value().value(keys::Title), u"Neon Rain"_s);
    EXPECT_EQ(tags.value().value(keys::Album), u"Night Signals"_s);
    EXPECT_EQ(tags.value().value(keys::TrackNumber), u"1/3"_s);
    EXPECT_EQ(tags.value().value(keys::EncodedBy), u"Selecta 9.9.9"_s);
    ASSERT_TRUE(tags.value().frontCover().has_value());
    EXPECT_EQ(QImage::fromData(tags.value().frontCover()->data).width(), 800);

    // The FLAC originals went to the Trash, the image and the download folders are gone, and so is the staging.
    EXPECT_EQ(trashed.size(), 2);
    EXPECT_FALSE(QFileInfo::exists(job.files.at(2).localPath));
    EXPECT_FALSE(QFileInfo::exists(QFileInfo(job.files.at(0).localPath).absolutePath()));
    EXPECT_FALSE(QFileInfo::exists(QDir(job.stagingDir).filePath(u"album-7"_s)));
}

TEST_F(AlbumArrivalTest, EveryFileOfTheAlbumIsQuarantined)
{
    if (!selecta::library::quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    add(1, u"01 - Okuma Drift - Neon Rain.flac"_s, flac(440.0, 2.0));
    add(2, u"02 - Okuma Drift - Low Tide.flac"_s, flac(330.0, 2.0));
    add(3, u"cover.jpg"_s, jpeg(600));
    // A download that already had an attribute keeps it through the conversion to MP3.
    ASSERT_TRUE(selecta::testing::setQuarantine(job.files.at(1).localPath, selecta::testing::safariApproved()));

    const AlbumArrivalResult result = arriveAlbum(job, nullptr);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    const QDir album(inboxPath(result.item.relativePath));
    EXPECT_EQ(selecta::library::quarantine::agentOf(album.path()), u"Selecta"_s);
    const QStringList files = album.entryList(QDir::Files | QDir::Hidden, QDir::Name);
    ASSERT_EQ(files.size(), 3) << files.join(u", "_s).toStdString(); // two MP3s and cover.jpg
    for (const QString &name : files) {
        const QByteArray attribute = selecta::library::quarantine::value(album.filePath(name));
        if (name.startsWith(u"02"_s)) {
            EXPECT_EQ(attribute, selecta::testing::safariApproved());
        } else {
            EXPECT_EQ(selecta::library::quarantine::agentOf(album.filePath(name)), u"Selecta"_s) << name.toStdString();
        }
    }
}

TEST_F(AlbumArrivalTest, WithoutATrackListFilesKeepTheirOwnTagsAndNames)
{
    job.folderName = u"Kassia Moreau - Harbour Lights"_s;
    add(1, u"01 - Kassia Moreau - Night Transit.mp3"_s, synth::makeMp3(synth::sine(440.0, 3.0), {.bitrateKbps = 320}));
    add(2, u"02 - Kassia Moreau - Glass Harbour.mp3"_s, synth::makeMp3(synth::sine(330.0, 3.0), {.bitrateKbps = 320}));
    selecta::convert::MetadataModel own;
    own.set(keys::Artist, u"Kassia Moreau"_s, selecta::convert::Provenance::Embedded);
    own.set(keys::Title, u"Night Transit (Original Mix)"_s, selecta::convert::Provenance::Embedded);
    own.set(keys::Album, u"Harbour Lights"_s, selecta::convert::Provenance::Embedded);
    ASSERT_TRUE(selecta::convert::writeTags(job.files.at(0).localPath, own).ok());

    const AlbumArrivalResult result = arriveAlbum(job, nullptr);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.item.tagSource, u"file"_s);
    EXPECT_EQ(result.item.album, u"Harbour Lights"_s);
    ASSERT_EQ(result.tracks.size(), 2);
    EXPECT_EQ(result.tracks.at(0).title, u"Night Transit (Original Mix)"_s);
    EXPECT_EQ(result.tracks.at(1).title, u"Glass Harbour"_s); // from its name: it has no tags
    EXPECT_EQ(result.tracks.at(1).artist, u"Kassia Moreau"_s);

    // Both files say the same album; MP3 320 is only tagged, so the originals are simply deleted.
    const QDir album(inboxPath(result.item.relativePath));
    const auto second = selecta::convert::readTags(album.filePath(trackName(u"2"_s, u"Glass Harbour"_s, u"mp3"_s)));
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().value(keys::Album), u"Harbour Lights"_s);
    EXPECT_EQ(second.value().value(keys::TrackNumber), u"2"_s);
    EXPECT_TRUE(trashed.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(job.files.at(0).localPath));
}

TEST_F(AlbumArrivalTest, AMissingFileMakesAPartialAlbum)
{
    add(1, u"01 - Okuma Drift - Neon Rain.flac"_s, flac(440.0, 2.0));
    job.missing = 1;
    const AlbumArrivalResult result = arriveAlbum(job, nullptr);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_FALSE(result.complete);
    EXPECT_TRUE(result.warning.contains(u"1 file"_s)) << result.warning.toStdString();
    EXPECT_EQ(result.item.trackCount, 1);
}

TEST_F(AlbumArrivalTest, AFolderWithNoUsableAudioFailsAndPublishesNothing)
{
    add(1, u"cover.jpg"_s, jpeg(400));
    add(2, u"01 - broken.flac"_s, synth::randomBytes(100'000, 7));
    const AlbumArrivalResult result = arriveAlbum(job, nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_TRUE(QDir(inboxPath(u"Albums"_s)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    EXPECT_TRUE(QFileInfo::exists(job.files.at(1).localPath)); // nothing was thrown away
    EXPECT_TRUE(trashed.isEmpty());
}

TEST_F(AlbumArrivalTest, ACancelledArrivalStopsAndCleansUp)
{
    add(1, u"01 - Okuma Drift - Neon Rain.flac"_s, flac(440.0, 2.0));
    selecta::audio::CancellationFlag cancel;
    cancel.cancel();
    const AlbumArrivalResult result = arriveAlbum(job, &cancel);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_TRUE(QFileInfo::exists(job.files.at(0).localPath));
    EXPECT_FALSE(QFileInfo::exists(QDir(job.stagingDir).filePath(u"album-7"_s)));
}
