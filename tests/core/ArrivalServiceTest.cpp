// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/ArrivalService.h"

#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "audio/AnalysisCache.h"
#include "convert/QualityAdvisor.h"
#include "convert/TagReader.h"
#include "convert/TagWriter.h"
#include "core/ActivityLog.h"
#include "core/InboxService.h"
#include "core/SpectrumCheck.h"
#include "library/HistoryStore.h"
#include "library/InboxStore.h"
#include "library/Quarantine.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "support/QuarantineSupport.h"

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
        makeArrival();
    }

    // The service, keeping spectrum checks in `cache` (none: findings only).
    void makeArrival(std::shared_ptr<selecta::audio::AnalysisCache> cache = {})
    {
        arrival.reset();
        ArrivalService::Options options;
        options.stagingDir = dir.filePath(u"staging"_s);
        options.encodedBy = u"Selecta 9.9.9"_s;
        options.session = std::nullopt;
        options.maxConcurrent = 2;
        options.analysisCache = std::move(cache);
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

TEST_F(ArrivalServiceTest, ArrivedFilesAreQuarantinedConvertedOrNot)
{
    if (!selecta::library::quarantine::isSupported()) {
        GTEST_SKIP() << "this platform has no quarantine attribute";
    }
    // A FLAC converted to MP3 320: the MP3 in the Inbox is quarantined by Selecta.
    const ArrivalResult converted = arrive(
        downloaded(11, u"a.flac"_s, synth::makeFlac(synth::quantised(synth::sine(440.0, 2.0), 16)), std::nullopt));
    ASSERT_TRUE(converted.ok) << converted.message.toStdString();
    ASSERT_TRUE(converted.message.endsWith(u".mp3"_s)) << converted.message.toStdString();
    EXPECT_EQ(selecta::library::quarantine::agentOf(inboxPath(converted.message)), u"Selecta"_s);

    // A download that already had an attribute: that one survives the conversion, unchanged.
    const Download marked =
        downloaded(12, u"b.flac"_s, synth::makeFlac(synth::quantised(synth::sine(330.0, 2.0), 16)), std::nullopt);
    ASSERT_TRUE(selecta::testing::setQuarantine(marked.localPath, selecta::testing::safariApproved()));
    const ArrivalResult kept = arrive(marked);
    ASSERT_TRUE(kept.ok) << kept.message.toStdString();
    EXPECT_EQ(selecta::library::quarantine::value(inboxPath(kept.message)), selecta::testing::safariApproved());

    // Lossy audio kept as it is, with fresh tags: quarantined too.
    const ArrivalResult lossy =
        arrive(downloaded(13, u"c.mp3"_s, synth::makeMp3(synth::sine(220.0, 2.0), {.bitrateKbps = 192})));
    ASSERT_TRUE(lossy.ok) << lossy.message.toStdString();
    EXPECT_EQ(selecta::library::quarantine::agentOf(inboxPath(lossy.message)), u"Selecta"_s);
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

TEST_F(ArrivalServiceTest, AnMp3At320IsOnlyTaggedWithoutAWarning)
{
    const Download download =
        downloaded(4, u"track.mp3"_s, synth::makeMp3(synth::sine(440.0, 5.0), {.bitrateKbps = 320}));
    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(result.warning.isEmpty()) << result.warning.toStdString(); // already what the Inbox promises (CONV-2)
    EXPECT_TRUE(selecta::library::InboxStore(db).find(result.inboxItem)->warning.isEmpty());
    const auto quality = selecta::convert::probeSourceQuality(inboxPath(result.message), false);
    ASSERT_TRUE(quality.ok());
    EXPECT_EQ(quality.value().bitrateKbps, 320); // never re-encoded
    EXPECT_EQ(selecta::convert::readTags(inboxPath(result.message)).value().value(keys::Title),
              u"Neon Rain - Extended Mix"_s); // but tagged
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

TEST_F(ArrivalServiceTest, AFileFarFromTheLengthTheSearchShowedIsFlagged)
{
    Download download = downloaded(7, u"short.flac"_s, synth::makeFlac(synth::quantised(synth::sine(440.0, 6.0), 16)));
    selecta::core::FileChoice file;
    file.peer = download.peer;
    file.remotePath = download.remotePath;
    file.durationSec = 20; // the search said 0:20; it plays 0:06
    download.file = file;
    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    EXPECT_TRUE(result.warning.contains(u"It plays 0:06, not the 0:20 the search showed"_s))
        << result.warning.toStdString();
    // The Inbox keeps it, with the warning (DL-10).
    EXPECT_EQ(selecta::library::InboxStore(db).find(result.inboxItem)->warning, result.warning);

    // Close enough: nothing to say.
    Download close = downloaded(8, u"close.flac"_s, synth::makeFlac(synth::quantised(synth::sine(440.0, 6.0), 16)));
    file.durationSec = 7;
    close.file = file;
    const ArrivalResult fine = arrive(close);
    ASSERT_TRUE(fine.ok) << fine.message.toStdString();
    EXPECT_TRUE(fine.warning.isEmpty()) << fine.warning.toStdString();
}

// PREV-9: every arrival's original is checked before conversion; what it finds stays with the Inbox item, and the
// report with its spectrogram goes into the analysis cache.
TEST_F(ArrivalServiceTest, ALosslessFileLowPassedAt16kHzArrivesWithTheFinding)
{
    const auto cache = std::make_shared<selecta::audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    makeArrival(cache);
    // Noise with nothing above 16 kHz, as a lossy encoder leaves it, stored as a 16-bit FLAC.
    const Download download =
        downloaded(1, u"okuma drift - neon rain.flac"_s,
                   synth::makeFlac(synth::quantised(synth::brickwallNoise(16000.0, 6.0, 7, 44100, 2, 0.1), 16)));
    const ArrivalResult result = arrive(download);
    ASSERT_TRUE(result.ok) << result.message.toStdString();

    const auto item = selecta::library::InboxStore(db).find(result.inboxItem);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->qualityChecked);
    EXPECT_EQ(item->qualityFindings, QStringList{u"from-lossy"_s});
    ASSERT_TRUE(item->qualityCutoffHz.has_value());
    EXPECT_NEAR(*item->qualityCutoffHz, 16000, 400);
    EXPECT_FALSE(item->qualitySourceKbps.has_value()); // 16 kHz is no LAME bitrate's lowpass
    EXPECT_EQ(selecta::core::spectrumBadge(*item), u"Likely from a lossy file"_s);
    // It informs; the Inbox still gets its MP3 320, and the original goes to the Trash as usual.
    EXPECT_EQ(item->format, u"MP3"_s);
    EXPECT_EQ(item->bitrate, 320);
    EXPECT_EQ(trashed, QStringList{download.localPath});
    EXPECT_TRUE(result.warning.isEmpty()) << result.warning.toStdString();

    // The full report and its spectrogram, under the original's content key.
    const std::optional<selecta::audio::CacheKey> key = selecta::audio::CacheKey::fromHex(item->qualityKey);
    ASSERT_TRUE(key.has_value()) << item->qualityKey.toStdString();
    const std::optional<selecta::audio::QualityReport> report = cache->loadQuality(*key);
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->hasFinding(selecta::audio::QualityFinding::LikelyFromLossy));
    EXPECT_EQ(report->cutoffHz, *item->qualityCutoffHz);
    ASSERT_TRUE(report->spectrogram.has_value());
    EXPECT_EQ(report->spectrogram->bins(), 256);
    EXPECT_GT(report->spectrogram->columns(), 100);

    // The activity log says so too.
    const QList<selecta::core::ActivityEntry> recent = selecta::core::ActivityLog::recent(db, 1);
    ASSERT_EQ(recent.size(), 1);
    EXPECT_TRUE(recent.constFirst().explanation.contains(u"(Likely from a lossy file)"_s))
        << recent.constFirst().explanation.toStdString();
}

TEST_F(ArrivalServiceTest, AFlacMadeFromAn128kbpsMp3SaysSo)
{
    const auto cache = std::make_shared<selecta::audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    makeArrival(cache);
    synth::Pcm music = synth::tones({110, 1000, 7000}, 6.0, 44100, 2, 0.08);
    const synth::Pcm noise = synth::whiteNoise(6.0, 3, 44100, 2, 0.25);
    for (std::size_t i = 0; i < music.samples.size(); ++i) {
        music.samples[i] += noise.samples[i];
    }
    selecta::audio::DecoderOptions native;
    native.channels = 0;
    const synth::Pcm via128 =
        synth::decodeAll(synth::makeMp3(synth::quantised(music, 16), {.bitrateKbps = 128}), native).pcm;
    const ArrivalResult result =
        arrive(downloaded(2, u"neon rain.flac"_s, synth::makeFlac(synth::quantised(via128, 16))));
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    const auto item = selecta::library::InboxStore(db).find(result.inboxItem);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->qualityFindings, QStringList{u"from-lossy"_s});
    EXPECT_EQ(item->qualitySourceKbps, 128);
    EXPECT_EQ(selecta::core::spectrumBadge(*item), u"Likely from a ≈ 128 kbps MP3"_s);
}

TEST_F(ArrivalServiceTest, AGenuineLosslessFileArrivesCheckedWithoutFindings)
{
    const auto cache = std::make_shared<selecta::audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    makeArrival(cache);
    const QByteArray flac = synth::makeFlac(synth::quantised(synth::whiteNoise(6.0, 11, 44100, 2, 0.3), 16));
    const ArrivalResult first = arrive(downloaded(3, u"genuine.flac"_s, flac));
    ASSERT_TRUE(first.ok) << first.message.toStdString();
    const auto item = selecta::library::InboxStore(db).find(first.inboxItem);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->qualityChecked);
    EXPECT_TRUE(item->qualityFindings.isEmpty());
    ASSERT_TRUE(item->qualityCutoffHz.has_value());
    EXPECT_GT(*item->qualityCutoffHz, 21000); // the whole band, up to Nyquist
    EXPECT_TRUE(selecta::core::spectrumBadge(*item).isEmpty());
    ASSERT_FALSE(item->qualityKey.isEmpty());

    // The same file again is taken from the cache: the same key, one report kept.
    const ArrivalResult again = arrive(downloaded(4, u"genuine (2).flac"_s, flac));
    ASSERT_TRUE(again.ok) << again.message.toStdString();
    const auto second = selecta::library::InboxStore(db).find(again.inboxItem);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->qualityKey, item->qualityKey);
    EXPECT_EQ(second->qualityCutoffHz, item->qualityCutoffHz);
    EXPECT_EQ(QDir(dir.filePath(u"analysis/quality"_s)).entryList(QDir::Files).size(), 1);
}

TEST_F(ArrivalServiceTest, WithoutACacheTheFindingsStayWithTheItem)
{
    const ArrivalResult result = arrive(
        downloaded(5, u"low.flac"_s,
                   synth::makeFlac(synth::quantised(synth::brickwallNoise(16000.0, 6.0, 5, 44100, 2, 0.1), 16))));
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    const auto item = selecta::library::InboxStore(db).find(result.inboxItem);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->qualityChecked);
    EXPECT_EQ(item->qualityFindings, QStringList{u"from-lossy"_s});
    EXPECT_TRUE(item->qualityKey.isEmpty()); // no spectrogram to show
}

TEST_F(ArrivalServiceTest, AFileTooShortToJudgeIsCheckedWithoutVerdict)
{
    const auto cache = std::make_shared<selecta::audio::AnalysisCache>(dir.filePath(u"analysis"_s));
    makeArrival(cache);
    selecta::meta::Release release = neonRain();
    release.durationMs = 1'000;
    const ArrivalResult result = arrive(downloaded(
        6, u"short.flac"_s, synth::makeFlac(synth::quantised(synth::whiteNoise(1.0, 2, 44100, 2, 0.3), 16)), release));
    ASSERT_TRUE(result.ok) << result.message.toStdString();
    const auto item = selecta::library::InboxStore(db).find(result.inboxItem);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->qualityChecked);
    EXPECT_TRUE(item->qualityFindings.isEmpty());
    EXPECT_FALSE(item->qualityCutoffHz.has_value()); // under 2 s: no cut-off measured
    EXPECT_FALSE(item->qualityKey.isEmpty());        // the spectrogram is still there to look at
}
