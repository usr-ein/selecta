// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ArrivalService.h"

#include "SyntheticAudio.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "convert/TagWriter.h"
#include "core/ActivityLog.h"
#include "core/InboxService.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::core::ArrivalResult;
using selecta::core::ArrivalService;
using selecta::core::Download;
using selecta::library::InboxItem;
namespace synth = selecta::testing;
namespace keys = selecta::convert::keys;

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
    image.fill(QColor(200, 40, 90));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return bytes;
}

selecta::meta::Release neonRain()
{
    selecta::meta::Release release;
    release.source = selecta::meta::Source::Spotify;
    release.title = u"Neon Rain - Extended Mix"_s;
    release.artists = {u"Okuma Drift"_s, u"Halden Rue"_s};
    release.album = u"Night Signals"_s;
    release.albumArtists = {u"Okuma Drift"_s};
    release.releaseDate = u"2024-03-08"_s;
    release.trackNumber = 2;
    release.trackTotal = 9;
    release.isrc = u"GBXXX2400002"_s;
    release.label = u"Tidal Records"_s;
    release.genres = {u"Deep House"_s, u"House"_s};
    release.spotifyUrl = QUrl(u"https://open.spotify.com/track/0000000000000000000002"_s);
    release.durationMs = 6'000;
    return release;
}

class ArrivalServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        selecta::core::LibraryConfig library;
        library.inboxRoot = dir.filePath(u"Inbox"_s);
        ASSERT_TRUE(QDir().mkpath(library.inboxRoot));
        inbox = std::make_unique<selecta::core::InboxService>(db, library, [this](const QString &path, QString *to) {
            trashed.append(path);
            *to = dir.filePath(u"Trash/"_s + QFileInfo(path).fileName());
            QDir().mkpath(dir.filePath(u"Trash"_s));
            return QFile::rename(path, *to);
        });
        ArrivalService::Options options;
        options.stagingDir = dir.filePath(u"staging"_s);
        options.encodedBy = u"Selecta 9.9.9"_s;
        options.session = std::nullopt;
        options.maxConcurrent = 2;
        arrival = std::make_unique<ArrivalService>(
            db, *inbox, log,
            [this](const QString &path, QString *to) {
                trashed.append(path);
                *to = dir.filePath(u"Trash/"_s + QFileInfo(path).fileName());
                QDir().mkpath(dir.filePath(u"Trash"_s));
                return QFile::rename(path, *to);
            },
            options, [] { return qint64{1'800'000'000'000}; });
    }

    // A completed download whose original is `bytes`, named like the peer named it.
    Download downloaded(qint64 id, const QString &fileName, const QByteArray &bytes,
                        std::optional<selecta::meta::Release> release = neonRain())
    {
        Download download;
        download.id = id;
        download.peer = u"vinyl_ghost"_s;
        download.remotePath = (u"Music\\"_s + fileName).toUtf8();
        download.size = bytes.size();
        download.received = bytes.size();
        download.state = Download::State::Converting;
        download.release = std::move(release);
        download.localPath = dir.filePath(u"incoming/%1/%2"_s.arg(id).arg(fileName));
        EXPECT_TRUE(writeFile(download.localPath, bytes));
        return download;
    }

    ArrivalResult arrive(const Download &download)
    {
        QSignalSpy finished(arrival.get(), &ArrivalService::finished);
        arrival->start(download);
        EXPECT_TRUE(QTest::qWaitFor([&finished] { return !finished.isEmpty(); }, 30'000));
        if (finished.isEmpty()) {
            return {};
        }
        EXPECT_EQ(finished.at(0).at(0).toLongLong(), download.id);
        return finished.at(0).at(1).value<ArrivalResult>();
    }

    QString inboxPath(const QString &relative) const { return dir.filePath(u"Inbox/"_s + relative); }

    QTemporaryDir dir;
    selecta::storage::Database db;
    selecta::core::ActivityLog log{db};
    QStringList trashed;
    std::unique_ptr<selecta::core::InboxService> inbox;
    std::unique_ptr<ArrivalService> arrival;
};

} // namespace

TEST(ArrivalTags, ComeFromTheRelease)
{
    const selecta::convert::MetadataModel tags = selecta::core::tagsFromRelease(neonRain());
    EXPECT_EQ(tags.value(keys::Title), u"Neon Rain - Extended Mix"_s);
    EXPECT_EQ(tags.value(keys::Artist), u"Okuma Drift, Halden Rue"_s);
    EXPECT_EQ(tags.value(keys::Album), u"Night Signals"_s);
    EXPECT_EQ(tags.value(keys::AlbumArtist), u"Okuma Drift"_s);
    EXPECT_EQ(tags.value(keys::TrackNumber), u"2/9"_s);
    EXPECT_FALSE(tags.contains(keys::DiscNumber));
    EXPECT_EQ(tags.value(keys::Date), u"2024-03-08"_s);
    EXPECT_EQ(tags.value(keys::Isrc), u"GBXXX2400002"_s);
    EXPECT_EQ(tags.value(keys::Label), u"Tidal Records"_s);
    EXPECT_EQ(tags.values(keys::Genre), QStringList{u"Deep House"_s});
    EXPECT_EQ(tags.value(keys::SourceUrl), u"https://open.spotify.com/track/0000000000000000000002"_s);
    EXPECT_EQ(tags.provenance(keys::Title), selecta::convert::Provenance::Release);
}

TEST_F(ArrivalServiceTest, ALosslessFileArrivesAsATaggedMp3_320)
{
    // A FLAC with its own (wrong) title and a cover the arrival processes and keeps.
    const Download download = downloaded(1, u"okuma drift - neon rain.flac"_s,
                                         synth::makeFlac(synth::quantised(synth::sine(440.0, 6.0), 16)));
    selecta::convert::MetadataModel own;
    own.set(keys::Title, u"neon rain (some rip)"_s, selecta::convert::Provenance::Embedded);
    selecta::convert::Picture picture;
    picture.data = jpeg(1200);
    picture.mimeType = u"image/jpeg"_s;
    own.setFrontCover(picture);
    ASSERT_TRUE(selecta::convert::writeTags(download.localPath, own).ok());

    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(result.warning.isEmpty()) << result.warning.toStdString();
    EXPECT_EQ(result.message, u"Singles/Okuma_Drift_Halden_Rue_-_Neon_Rain_-_Extended_Mix.mp3"_s);
    const QString file = inboxPath(result.message);
    ASSERT_TRUE(QFileInfo::exists(file));

    // MP3 320 CBR, tagged from the release, with the cover scaled for CDJs.
    const auto quality = selecta::convert::probeSourceQuality(file, false);
    ASSERT_TRUE(quality.ok());
    EXPECT_EQ(quality.value().codec, selecta::audio::Codec::Mp3);
    EXPECT_EQ(quality.value().bitrateKbps, 320);
    EXPECT_TRUE(quality.value().constantBitrate);
    const auto tags = selecta::convert::readTags(file);
    ASSERT_TRUE(tags.ok());
    EXPECT_EQ(tags.value().value(keys::Title), u"Neon Rain - Extended Mix"_s);
    EXPECT_EQ(tags.value().value(keys::Label), u"Tidal Records"_s);
    EXPECT_EQ(tags.value().value(keys::Isrc), u"GBXXX2400002"_s);
    EXPECT_EQ(tags.value().value(keys::EncodedBy), u"Selecta 9.9.9"_s);
    ASSERT_TRUE(tags.value().frontCover().has_value());
    const QImage cover = QImage::fromData(tags.value().frontCover()->data);
    EXPECT_EQ(cover.width(), 800);

    // The original went to the Trash; the Inbox item and the history say where the file is and what it was.
    EXPECT_EQ(trashed, QStringList{download.localPath});
    EXPECT_FALSE(QFileInfo::exists(QFileInfo(download.localPath).absolutePath())); // the download's folder is gone
    const std::optional<InboxItem> item = selecta::library::InboxStore(db).find(result.inboxItem);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->relativePath, result.message);
    EXPECT_EQ(item->format, u"MP3"_s);
    EXPECT_EQ(item->bitrate, 320);
    EXPECT_EQ(item->sourceFormat, u"FLAC 16/44.1"_s);
    EXPECT_EQ(item->tagSource, u"spotify"_s);
    EXPECT_EQ(item->artist, u"Okuma Drift, Halden Rue"_s);
    EXPECT_EQ(item->peer, u"vinyl_ghost"_s);
    EXPECT_EQ(item->year, 2024);
    EXPECT_NEAR(static_cast<double>(item->durationMs), 6000.0, 50.0);
    const QList<selecta::library::HistoryEntry> history = selecta::library::HistoryStore(db).recent(5);
    ASSERT_EQ(history.size(), 1);
    EXPECT_EQ(history.constFirst().inboxItem, result.inboxItem);
    EXPECT_EQ(history.constFirst().quality, u"FLAC 16/44.1"_s);
    EXPECT_EQ(inbox->entries().size(), 1);
}

TEST_F(ArrivalServiceTest, LossyAudioBelowTheFormatIsKeptAsIt)
{
    const Download download =
        downloaded(2, u"track.mp3"_s, synth::makeMp3(synth::sine(440.0, 5.0), {.bitrateKbps = 192}));
    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(result.warning.contains(u"192"_s)) << result.warning.toStdString();
    const auto quality = selecta::convert::probeSourceQuality(inboxPath(result.message), false);
    ASSERT_TRUE(quality.ok());
    EXPECT_EQ(quality.value().bitrateKbps, 192); // never re-encoded
    EXPECT_EQ(selecta::convert::readTags(inboxPath(result.message)).value().value(keys::Title),
              u"Neon Rain - Extended Mix"_s); // but tagged
    EXPECT_TRUE(trashed.isEmpty());           // the copy is simply deleted
    EXPECT_FALSE(QFileInfo::exists(download.localPath));
    EXPECT_EQ(selecta::library::InboxStore(db).find(result.inboxItem)->bitrate, 192);
}

TEST_F(ArrivalServiceTest, WithoutAReleaseTheFilesOwnTagsAreUsed)
{
    const Download download =
        downloaded(3, u"x.flac"_s, synth::makeFlac(synth::quantised(synth::sine(220.0, 3.0), 16)), std::nullopt);
    selecta::convert::MetadataModel own;
    own.set(keys::Artist, u"Kassia Moreau"_s, selecta::convert::Provenance::Embedded);
    own.set(keys::Title, u"Night Transit"_s, selecta::convert::Provenance::Embedded);
    ASSERT_TRUE(selecta::convert::writeTags(download.localPath, own).ok());
    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_EQ(result.message, u"Singles/Kassia_Moreau_-_Night_Transit.mp3"_s);
    EXPECT_EQ(selecta::library::InboxStore(db).find(result.inboxItem)->tagSource, u"file"_s);
}

TEST_F(ArrivalServiceTest, ANameAlreadyInTheInboxGetsANumber)
{
    ASSERT_TRUE(writeFile(inboxPath(u"Singles/Okuma_Drift_Halden_Rue_-_Neon_Rain_-_Extended_Mix.mp3"_s), "taken"));
    const Download download =
        downloaded(4, u"a.flac"_s, synth::makeFlac(synth::quantised(synth::sine(440.0, 2.0), 16)));
    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_EQ(result.message, u"Singles/Okuma_Drift_Halden_Rue_-_Neon_Rain_-_Extended_Mix (2).mp3"_s);
}

TEST_F(ArrivalServiceTest, AFileThatIsntAudioFailsAndStaysPut)
{
    const Download download = downloaded(5, u"broken.flac"_s, synth::randomBytes(500'000, 1234));
    const ArrivalResult result = arrive(download);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.message.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(download.localPath)); // nothing was thrown away
    EXPECT_TRUE(inbox->entries().isEmpty());
    EXPECT_TRUE(QDir(inboxPath(u"Singles"_s)).entryList(QDir::Files).isEmpty());
}

TEST_F(ArrivalServiceTest, AMissingFileFails)
{
    Download download = downloaded(6, u"gone.flac"_s, "x");
    ASSERT_TRUE(QFile::remove(download.localPath));
    const ArrivalResult result = arrive(download);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.message.contains(u"missing"_s)) << result.message.toStdString();
}

TEST_F(ArrivalServiceTest, CancellingAnUnknownArrivalDoesNothing)
{
    const QSignalSpy finished(arrival.get(), &ArrivalService::finished);
    arrival->cancel(99);
    EXPECT_TRUE(finished.isEmpty());
    EXPECT_FALSE(arrival->isBusy());
}
