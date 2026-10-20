// SPDX-License-Identifier: GPL-3.0-or-later
// The collection scanner (docs/contracts/collection.md): what it reads, what it skips, how it compares signatures.
#include "collection/Scanner.h"

#include "SyntheticAudio.h"
#include "TestTree.h"
#include "collection/detail/FileReader.h"
#include "collection/detail/Walk.h"
#include "convert/TagWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace Qt::StringLiterals;
using selecta::collection::comparisonKey;
using selecta::collection::FileSignature;
using selecta::collection::ScannedFile;
using selecta::collection::ScanOptions;
using selecta::collection::ScanResult;
using selecta::collection::test::findFile;
using selecta::collection::test::Format;
using selecta::collection::test::signaturesOf;
using selecta::collection::test::Tags;
using selecta::collection::test::TestTree;
using selecta::match::Codec;
namespace convert = selecta::convert;

namespace {

ScanResult scan(const TestTree &tree, const QHash<QString, FileSignature> &known = {}, const ScanOptions &options = {})
{
    return selecta::collection::scan({tree.root()}, known, options);
}

QStringList paths(const QList<ScannedFile> &files)
{
    QStringList list;
    for (const ScannedFile &file : files) {
        list.append(file.file.path);
    }
    return list;
}

QStringList unreadablePaths(const ScanResult &result)
{
    QStringList list;
    for (const auto &[path, reason] : result.unreadable) {
        list.append(path);
    }
    return list;
}

Tags tags(const QString &artist, const QString &title, const QString &album = {}, const QString &track = {})
{
    Tags tags{{u"ARTIST"_s, artist}, {u"TITLE"_s, title}};
    if (!album.isEmpty()) {
        tags.insert(u"ALBUM"_s, album);
    }
    if (!track.isEmpty()) {
        tags.insert(u"TRACKNUMBER"_s, track);
    }
    return tags;
}

// --- Reading -----------------------------------------------------------------------------------------------------

TEST(Scanner, ReadsTagsAndStreamFactsOfEveryFormat)
{
    TestTree tree;
    const Tags full{{u"ARTIST"_s, u"Bicep"_s},      {u"TITLE"_s, u"Glue (Original Mix)"_s}, {u"ALBUM"_s, u"Isles"_s},
                    {u"ALBUMARTIST"_s, u"Bicep"_s}, {u"TRACKNUMBER"_s, u"3/12"_s},          {u"DISCNUMBER"_s, u"1/2"_s},
                    {u"DATE"_s, u"2021-01-22"_s},   {u"ISRC"_s, u"GBCFB2000123"_s}};
    const QString mp3 = tree.add(u"a.mp3"_s, Format::Mp3, full, 3.0);
    const QString flac = tree.add(u"b.flac"_s, Format::Flac24, full, 3.0);
    const QString wav = tree.add(u"c.wav"_s, Format::Wav, full, 3.0);
    const QString aiff = tree.add(u"d.aiff"_s, Format::Aiff, full, 3.0);
    const QString m4a = tree.add(u"e.m4a"_s, Format::M4aAac, full, 3.0);
    const QString alac = tree.add(u"f.m4a"_s, Format::M4aAlac, full, 3.0);
    const ScanResult result = scan(tree);
    EXPECT_TRUE(result.unreadable.isEmpty()) << result.unreadable.size();
    ASSERT_EQ(result.added.size(), 6);
    for (const ScannedFile &file : result.added) {
        SCOPED_TRACE(file.file.path.toStdString());
        EXPECT_EQ(file.artist, u"Bicep"_s);
        EXPECT_EQ(file.title, u"Glue (Original Mix)"_s); // the version text stays
        EXPECT_EQ(file.album, u"Isles"_s);
        EXPECT_EQ(file.albumArtist, u"Bicep"_s);
        EXPECT_EQ(file.trackNumber, 3);
        EXPECT_EQ(file.discNumber, 1);
        EXPECT_EQ(file.year, 2021);
        EXPECT_EQ(file.isrc, u"GBCFB2000123"_s);
        EXPECT_FALSE(file.namesFromFileName);
        EXPECT_NEAR(static_cast<double>(file.durationMs), 3000.0, 60.0);
        EXPECT_EQ(file.quality.sampleRateHz, 44100);
        EXPECT_GT(file.file.size, 0);
        EXPECT_GT(file.file.modifiedMs, 0);
    }
    EXPECT_EQ(findFile(result.added, mp3)->quality.codec, Codec::Mp3);
    EXPECT_EQ(findFile(result.added, mp3)->quality.bitrateKbps, 320);
    EXPECT_EQ(findFile(result.added, mp3)->quality.vbr, false);
    EXPECT_EQ(findFile(result.added, flac)->quality.codec, Codec::Flac);
    EXPECT_EQ(findFile(result.added, flac)->quality.bitDepth, 24);
    EXPECT_EQ(findFile(result.added, wav)->quality.codec, Codec::Wav);
    EXPECT_EQ(findFile(result.added, wav)->quality.bitDepth, 24);
    EXPECT_EQ(findFile(result.added, aiff)->quality.codec, Codec::Aiff);
    EXPECT_EQ(findFile(result.added, m4a)->quality.codec, Codec::Aac);
    EXPECT_EQ(findFile(result.added, alac)->quality.codec, Codec::Alac);
    EXPECT_EQ(paths(result.added), (QStringList{mp3, flac, wav, aiff, m4a, alac})); // sorted by path
}

TEST(Scanner, StreamFactsComeFromTheHeaderAlone)
{
    TestTree tree;
    const QString vbr = tree.add(u"vbr.mp3"_s, Format::Mp3Vbr, {}, 4.0);
    const QString plain = tree.add(u"plain.mp3"_s, Format::Mp3NoLameTag, {}, 4.0);
    const QString ogg = tree.add(u"ogg.oga"_s, Format::OggFlac, {}, 4.0);
    const QString fast = tree.add(u"fast.m4a"_s, Format::M4aAacFastStart, {}, 4.0);
    const QString flac = tree.add(u"lossless.flac"_s, Format::Flac16, {}, 4.0);
    const ScanResult result = scan(tree);
    ASSERT_EQ(result.added.size(), 4) << paths(result.added).join(u", ").toStdString(); // .oga isn't an audio extension
    const ScannedFile *vbrFile = findFile(result.added, vbr);
    ASSERT_NE(vbrFile, nullptr);
    EXPECT_EQ(vbrFile->quality.vbr, true);
    // The LAME tag's average over the stream: what the file's size says, not the first frame's rate.
    const double average = static_cast<double>(vbrFile->file.size) * 8.0 / 4.0 / 1000.0;
    EXPECT_NEAR(static_cast<double>(vbrFile->quality.bitrateKbps), average, average * 0.15);
    EXPECT_NEAR(static_cast<double>(vbrFile->durationMs), 4000.0, 60.0);
    const ScannedFile *plainFile = findFile(result.added, plain);
    ASSERT_NE(plainFile, nullptr);
    EXPECT_EQ(plainFile->quality.bitrateKbps, 192);
    EXPECT_NEAR(static_cast<double>(plainFile->durationMs), 4000.0, 120.0); // estimated from the size: CBR is exact
    EXPECT_NEAR(static_cast<double>(findFile(result.added, fast)->durationMs), 4000.0, 60.0);
    EXPECT_EQ(findFile(result.added, flac)->quality.bitDepth, 16);
    EXPECT_EQ(findFile(result.added, ogg), nullptr);
}

TEST(Scanner, UntaggedFilesTakeTheirNamesFromThePath)
{
    TestTree tree;
    const QString first = tree.add(u"Bicep/Isles (2021)/03 - Glue.mp3"_s);
    tree.add(u"Bicep/Isles (2021)/04 - Atlas.mp3"_s);
    const QString titleOnly =
        tree.add(u"Singles/Bicep - Apricots (Extended Mix).flac"_s, Format::Flac16, {{u"ALBUM"_s, u"Apricots"_s}});
    const ScanResult result = scan(tree);
    ASSERT_EQ(result.added.size(), 3);
    const ScannedFile *glue = findFile(result.added, first);
    ASSERT_NE(glue, nullptr);
    EXPECT_TRUE(glue->namesFromFileName);
    EXPECT_EQ(glue->title, u"Glue"_s);
    EXPECT_EQ(glue->artist, u"Bicep"_s);
    EXPECT_EQ(glue->album, u"Isles"_s);
    EXPECT_EQ(glue->trackNumber, 3);
    EXPECT_EQ(glue->year, 2021);
    // Some tags but no title: the names come from the file, the tags that exist stay.
    const ScannedFile *apricots = findFile(result.added, titleOnly);
    ASSERT_NE(apricots, nullptr);
    EXPECT_TRUE(apricots->namesFromFileName);
    EXPECT_EQ(apricots->artist, u"Bicep"_s);
    EXPECT_EQ(apricots->title, u"Apricots (Extended Mix)"_s);
    EXPECT_EQ(apricots->album, u"Apricots"_s);
}

TEST(Scanner, SeveralArtistValuesAreJoined)
{
    TestTree tree;
    convert::MetadataModel model;
    const QString path = tree.add(u"collab.flac"_s, Format::Flac16);
    model.set(convert::keys::Artist, QStringList{u"Bicep"_s, u"Clara La San"_s}, convert::Provenance::User);
    model.set(convert::keys::Title, u"Water"_s, convert::Provenance::User);
    ASSERT_TRUE(convert::writeTags(path, model).ok());
    const ScanResult result = scan(tree);
    ASSERT_EQ(result.added.size(), 1);
    EXPECT_EQ(result.added.front().artist, u"Bicep & Clara La San"_s);
}

TEST(Scanner, NumbersAndYearsAreParsedLeniently)
{
    using selecta::collection::detail::leadingNumber;
    using selecta::collection::detail::yearOf;
    EXPECT_EQ(leadingNumber(u"3"), 3);
    EXPECT_EQ(leadingNumber(u"03/12"), 3);
    EXPECT_EQ(leadingNumber(u" 7 "), 7);
    EXPECT_EQ(leadingNumber(u"A1"), 0);
    EXPECT_EQ(leadingNumber(u""), 0);
    EXPECT_EQ(leadingNumber(u"123456789"), 1234); // bounded
    EXPECT_EQ(yearOf(u"2021"), 2021);
    EXPECT_EQ(yearOf(u"2021-01-22"), 2021);
    EXPECT_EQ(yearOf(u"2021-01-22T10:00:00"), 2021);
    EXPECT_EQ(yearOf(u"21"), 0);
    EXPECT_EQ(yearOf(u"20210"), 0);
    EXPECT_EQ(yearOf(u"0999"), 0);
    EXPECT_EQ(yearOf(u"May 2021"), 0);
}

// --- Skip rules ------------------------------------------------------------------------------------------------------

TEST(Scanner, SkipsHiddenTemporaryAndNonAudioFiles)
{
    TestTree tree;
    const QString kept = tree.add(u"Artist - Title.mp3"_s);
    tree.add(u".hidden.mp3"_s);
    tree.add(u"._Artist - Title.mp3"_s); // AppleDouble
    tree.add(u".hidden folder/Artist - Title.mp3"_s);
    tree.add(u"Artist - Title.mp3.selecta-part"_s);
    tree.add(u"Artist - Other.part"_s);
    tree.add(u"Artist - Other.mp3.tmp"_s);
    tree.add(u"~$Artist - Other.mp3"_s);
    tree.add(u"Artist - Other.mp3~"_s);
    tree.addBytes(u"cover.jpg"_s, "jpeg");
    tree.addBytes(u"notes.txt"_s, "text");
    tree.addBytes(u"Artist - Title.cue"_s, "cue");
    tree.addBytes(u".DS_Store"_s, "store");
#ifdef UF_HIDDEN
    const QString flagged = tree.add(u"Finder hidden.mp3"_s);
    ASSERT_EQ(::chflags(QFile::encodeName(flagged).constData(), UF_HIDDEN), 0);
#endif
    const ScanResult result = scan(tree);
    EXPECT_EQ(paths(result.added), QStringList{kept});
    EXPECT_TRUE(result.unreadable.isEmpty());
}

TEST(Scanner, ExcludedFoldersAreNotPartOfTheCollection)
{
    TestTree tree;
    const QString kept = tree.add(u"Music/Artist - Title.mp3"_s);
    const QString inbox = tree.add(u"Music/Selecta Inbox/Artist - Other.mp3"_s);
    ScanOptions options;
    options.excludedFolders = {tree.path(u"Music/Selecta Inbox"_s)};
    const ScanResult first = scan(tree, {}, options);
    EXPECT_EQ(paths(first.added), QStringList{kept});
    // A known file under an excluded folder is removed: it isn't part of the collection.
    const QHash<QString, FileSignature> known{{inbox, {inbox, 1, 1}}, {kept, findFile(first.added, kept)->file}};
    const ScanResult second = scan(tree, known, options);
    EXPECT_EQ(second.removed, QStringList{inbox});
    EXPECT_TRUE(second.added.isEmpty());
}

TEST(Scanner, SymlinksOutsideTheRootsAreSkipped)
{
    TestTree tree;
    TestTree outside;
    outside.add(u"Elsewhere/Artist - Outside.mp3"_s);
    const QString inside = tree.add(u"Music/Artist - Inside.mp3"_s);
    ASSERT_EQ(::symlink(QFile::encodeName(outside.path(u"Elsewhere"_s)).constData(),
                        QFile::encodeName(tree.path(u"Music/Linked folder"_s)).constData()),
              0);
    ASSERT_EQ(::symlink(QFile::encodeName(outside.path(u"Elsewhere/Artist - Outside.mp3"_s)).constData(),
                        QFile::encodeName(tree.path(u"Music/Linked file.mp3"_s)).constData()),
              0);
    const ScanResult result = selecta::collection::scan({tree.path(u"Music"_s)}, {});
    EXPECT_EQ(paths(result.added), QStringList{inside});
}

TEST(Scanner, SymlinksInsideTheRootsAreFollowedOnce)
{
    TestTree tree;
    const QString real = tree.add(u"Music/Albums/Artist - Album/01 - Title.mp3"_s);
    // A second path to the same folder, a second path to the same file, and a loop back to the root.
    ASSERT_EQ(::symlink(QFile::encodeName(tree.path(u"Music/Albums/Artist - Album"_s)).constData(),
                        QFile::encodeName(tree.path(u"Music/Shortcut"_s)).constData()),
              0);
    ASSERT_EQ(::symlink(QFile::encodeName(real).constData(),
                        QFile::encodeName(tree.path(u"Music/Albums/Again.mp3"_s)).constData()),
              0);
    ASSERT_EQ(::symlink(QFile::encodeName(tree.path(u"Music"_s)).constData(),
                        QFile::encodeName(tree.path(u"Music/Albums/Artist - Album/Loop"_s)).constData()),
              0);
    const ScanResult result = selecta::collection::scan({tree.path(u"Music"_s)}, {});
    ASSERT_EQ(result.added.size(), 1) << paths(result.added).join(u", ").toStdString();
    EXPECT_TRUE(result.unreadable.isEmpty());
}

TEST(Scanner, OverlappingRootsScanEachFileOnce)
{
    TestTree tree;
    tree.add(u"Music/Artist - One.mp3"_s);
    tree.add(u"Music/Sub/Artist - Two.mp3"_s);
    const ScanResult result =
        selecta::collection::scan({tree.path(u"Music/Sub"_s), tree.path(u"Music"_s), tree.path(u"Music/"_s)}, {});
    EXPECT_EQ(result.added.size(), 2);
}

// --- Signatures --------------------------------------------------------------------------------------------------

TEST(Scanner, RescanReadsOnlyWhatChanged)
{
    TestTree tree;
    const QString same = tree.add(u"Artist - Same.mp3"_s, Format::Mp3, tags(u"Artist"_s, u"Same"_s));
    const QString edited = tree.add(u"Artist - Edited.mp3"_s, Format::Mp3, tags(u"Artist"_s, u"Edited"_s));
    const QString gone = tree.add(u"Artist - Gone.mp3"_s);
    const ScanResult first = scan(tree);
    ASSERT_EQ(first.added.size(), 3);
    const QHash<QString, FileSignature> known = signaturesOf(first);

    // Unchanged: corrupt its bytes behind the scanner's back, keeping size and time. It must not be read again.
    const FileSignature sameSignature = known.value(same);
    {
        QFile file(same);
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        file.write(QByteArray(static_cast<qsizetype>(sameSignature.size), 'x'));
    }
    ASSERT_TRUE(TestTree::setModified(same, sameSignature.modifiedMs));
    // Edited: new tags, a later time. Gone: deleted. New: added.
    tree.add(u"Artist - Edited.mp3"_s, Format::Mp3, tags(u"Artist"_s, u"Edited again"_s));
    ASSERT_TRUE(TestTree::setModified(edited, known.value(edited).modifiedMs + 5000));
    ASSERT_TRUE(QFile::remove(gone));
    const QString added = tree.add(u"Artist - New.mp3"_s);

    std::vector<std::pair<int, int>> progress;
    const ScanResult second = selecta::collection::scan(
        {tree.root()}, known, {}, nullptr, [&progress](int done, int total) { progress.emplace_back(done, total); });
    EXPECT_EQ(paths(second.added), QStringList{added});
    ASSERT_EQ(paths(second.changed), QStringList{edited});
    EXPECT_EQ(second.changed.front().title, u"Edited again"_s);
    EXPECT_EQ(second.removed, QStringList{gone});
    EXPECT_TRUE(second.unreadable.isEmpty()); // the corrupted unchanged file wasn't opened
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.front(), std::make_pair(0, 2));
    EXPECT_EQ(progress.back(), std::make_pair(2, 2));
}

TEST(Scanner, NothingChangedReadsNothing)
{
    TestTree tree;
    tree.add(u"A - One.mp3"_s);
    tree.add(u"A - Two.flac"_s, Format::Flac16);
    const ScanResult first = scan(tree);
    int total = -1;
    const ScanResult second = selecta::collection::scan({tree.root()}, signaturesOf(first), {}, nullptr,
                                                        [&total](int /*done*/, int all) { total = all; });
    EXPECT_TRUE(second.added.isEmpty());
    EXPECT_TRUE(second.changed.isEmpty());
    EXPECT_TRUE(second.removed.isEmpty());
    EXPECT_EQ(total, 0);
}

TEST(Scanner, PathsAreComparedAsNfcButReportedAsOnDisk)
{
    TestTree tree;
    // Written with decomposed (NFD) bytes, as HFS+ and many tools do; APFS keeps the bytes as given.
    const QString nfc = u"Caf\u00e9 Tacvba - Eres.mp3"_s;
    const QByteArray nfdBytes = nfc.normalized(QString::NormalizationForm_D).toUtf8();
    const QByteArray nativePath = QFile::encodeName(tree.root()) + '/' + nfdBytes;
    {
        QFile file;
        const int descriptor = ::open(nativePath.constData(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        ASSERT_GE(descriptor, 0);
        ASSERT_TRUE(file.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle));
        file.write(selecta::collection::test::audioBytes(Format::Mp3, 2.0));
    }
    const ScanResult first = scan(tree);
    ASSERT_EQ(first.added.size(), 1);
    const QString reported = first.added.front().file.path;
    EXPECT_TRUE(QFileInfo::exists(reported)) << "the reported path opens the file";
    EXPECT_EQ(QFileInfo(reported).fileName().normalized(QString::NormalizationForm_C), nfc);

    // Known entries under either spelling of the path are the same file, unchanged.
    for (const auto form : {QString::NormalizationForm_C, QString::NormalizationForm_D}) {
        FileSignature signature = first.added.front().file;
        signature.path = reported.normalized(form);
        const ScanResult second = scan(tree, {{signature.path, signature}});
        EXPECT_TRUE(second.added.isEmpty());
        EXPECT_TRUE(second.changed.isEmpty());
        EXPECT_TRUE(second.removed.isEmpty());
        EXPECT_EQ(comparisonKey(reported), comparisonKey(signature.path));
    }
}

TEST(Scanner, ComparisonKeysAreAbsoluteCleanAndNfc)
{
    EXPECT_EQ(comparisonKey(u"/Music//Bicep/./Isles/../Glue.mp3"_s), u"/Music/Bicep/Glue.mp3"_s);
    EXPECT_EQ(comparisonKey(QString(u"/Music/Café"_s)), QString(u"/Music/Café"_s));
    EXPECT_TRUE(QDir::isAbsolutePath(comparisonKey(u"relative/x.mp3"_s)));
}

// --- Offline volumes and unreadable folders
// ------------------------------------------------------------------------------

TEST(Scanner, AMissingRootRemovesNothing)
{
    TestTree tree;
    const QString root = tree.path(u"Unplugged drive"_s);
    const QString known = root + u"/Artist - Title.mp3"_s;
    const ScanResult result = selecta::collection::scan({root}, {{known, {known, 1000, 1000}}});
    EXPECT_TRUE(result.removed.isEmpty());
    ASSERT_EQ(result.unreadable.size(), 1);
    EXPECT_EQ(result.unreadable.front().first, root);
}

TEST(Scanner, AnUnlistableFolderRemovesNothingUnderIt)
{
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can list anything";
    }
    TestTree tree;
    tree.add(u"Open/Artist - One.mp3"_s);
    const QString locked = tree.add(u"Locked/Artist - Two.mp3"_s);
    const ScanResult first = scan(tree);
    ASSERT_EQ(first.added.size(), 2);
    const QString folder = tree.path(u"Locked"_s);
    ASSERT_EQ(::chmod(QFile::encodeName(folder).constData(), 0), 0);
    const ScanResult second = scan(tree, signaturesOf(first));
    ::chmod(QFile::encodeName(folder).constData(), 0755); // so the temporary folder can be cleaned up
    EXPECT_TRUE(second.removed.isEmpty()) << second.removed.join(u", ").toStdString();
    EXPECT_EQ(unreadablePaths(second), QStringList{folder});
    EXPECT_FALSE(second.removed.contains(locked));
}

TEST(Scanner, RemovedFilesAreOnlyThoseUnderTheRoots)
{
    TestTree tree;
    tree.add(u"Music/Artist - Here.mp3"_s);
    const QString elsewhere = u"/Volumes/Other/Artist - There.mp3"_s;
    const QString underRoot = tree.path(u"Music/Artist - Deleted.mp3"_s);
    const QHash<QString, FileSignature> known{{elsewhere, {elsewhere, 1, 1}}, {underRoot, {underRoot, 1, 1}}};
    const ScanResult result = selecta::collection::scan({tree.path(u"Music"_s)}, known);
    EXPECT_EQ(result.removed, QStringList{underRoot});
}

// --- Folder roles (PRD LIB-1) ----------------------------------------------------------------------------------------

TEST(Scanner, ATracksFolderGivesItsLooseFilesOnly)
{
    TestTree tree;
    const QString loose = tree.add(u"Tracks/Artist - Loose.mp3"_s);
    const QString nested = tree.add(u"Tracks/Old album/01 - Nested.mp3"_s);
    tree.addBytes(u"Tracks/Broken/02 - Corrupt.mp3"_s, QByteArray(4096, 'x')); // never opened: subfolders are left out
    ScanOptions options;
    options.tracksFolders = {tree.path(u"Tracks"_s)};
    const ScanResult first = selecta::collection::scan({tree.path(u"Tracks"_s)}, {}, options);
    EXPECT_EQ(paths(first.added), QStringList{loose});
    EXPECT_TRUE(first.unreadable.isEmpty());
    // A file known from before (a scan of the folder as a whole) is no longer part of the collection.
    QHash<QString, FileSignature> known = signaturesOf(first);
    known.insert(nested, {nested, QFileInfo(nested).size(), 1});
    const ScanResult second = selecta::collection::scan({tree.path(u"Tracks"_s)}, known, options);
    EXPECT_EQ(second.removed, QStringList{nested});
    EXPECT_TRUE(second.added.isEmpty());
    EXPECT_TRUE(second.changed.isEmpty());
}

TEST(Scanner, AnAlbumsFolderGivesItsSubfoldersOnly)
{
    TestTree tree;
    const QString loose = tree.add(u"Albums/Artist - Loose.mp3"_s);
    const QString first = tree.add(u"Albums/Bicep - Isles/01 - Atlas.mp3"_s);
    const QString disc = tree.add(u"Albums/Moderat - II/CD2/01 - Bad Kingdom.flac"_s, Format::Flac16);
    tree.addFolder(u"Albums/Empty"_s);
    ScanOptions options;
    options.albumsFolders = {tree.path(u"Albums"_s)};
    const ScanResult scanned = selecta::collection::scan({tree.path(u"Albums"_s)}, {}, options);
    EXPECT_EQ(paths(scanned.added), (QStringList{first, disc}));
    // A loose file known from before goes.
    QHash<QString, FileSignature> known = signaturesOf(scanned);
    known.insert(loose, {loose, QFileInfo(loose).size(), 1});
    const ScanResult again = selecta::collection::scan({tree.path(u"Albums"_s)}, known, options);
    EXPECT_EQ(again.removed, QStringList{loose});
    EXPECT_TRUE(again.added.isEmpty());
    // A rescan of one album folder (a Watcher report) takes all of it: the albums folder's rule is for its own files.
    const ScanResult album = selecta::collection::scan({tree.path(u"Albums/Moderat - II"_s)}, {}, options);
    EXPECT_EQ(paths(album.added), QStringList{disc});
}

TEST(Scanner, RolesApplyWhereverTheWalkMeetsTheirFolders)
{
    TestTree tree;
    const QString plain = tree.add(u"Music/Artist - Plain.mp3"_s);
    const QString track = tree.add(u"Music/Tracks/Artist - Track.mp3"_s);
    tree.add(u"Music/Tracks/Sub/Artist - Left out.mp3"_s);
    tree.add(u"Music/Albums/Artist - Loose.mp3"_s);
    const QString albumTrack = tree.add(u"Music/Albums/Artist - Album/01 - One.mp3"_s);
    ScanOptions options;
    options.tracksFolders = {tree.path(u"Music/Tracks"_s)};
    options.albumsFolders = {tree.path(u"Music/Albums"_s)};
    const ScanResult result = selecta::collection::scan({tree.path(u"Music"_s)}, {}, options);
    EXPECT_EQ(paths(result.added), (QStringList{albumTrack, plain, track})); // sorted by path
}

TEST(Scanner, AFolderWithBothRolesIsScannedWhole)
{
    TestTree tree;
    const QString loose = tree.add(u"Music/Artist - Loose.mp3"_s);
    const QString albumTrack = tree.add(u"Music/Artist - Album/01 - One.mp3"_s);
    ScanOptions options;
    options.tracksFolders = {tree.path(u"Music"_s)};
    options.albumsFolders = {tree.path(u"Music/"_s)};
    const ScanResult result = selecta::collection::scan({tree.path(u"Music"_s)}, {}, options);
    EXPECT_EQ(paths(result.added), (QStringList{albumTrack, loose}));
}

// --- Corrupt files, bounds, cancellation -----------------------------------------------------------------------------

TEST(Scanner, CorruptFilesAreReportedAndTheScanGoesOn)
{
    TestTree tree;
    const QString good = tree.add(u"Artist - Good.mp3"_s);
    const QString empty = tree.addBytes(u"Artist - Empty.mp3"_s, {});
    const QString garbage = tree.addBytes(u"Artist - Garbage.flac"_s, selecta::testing::randomBytes(5000, 7));
    const QByteArray mp3 = selecta::collection::test::audioBytes(Format::Mp3, 2.0);
    const QString truncated = tree.addBytes(u"Artist - Truncated.mp3"_s, mp3.first(300));
    const QString notAudio = tree.addBytes(u"Artist - Text.wav"_s, "this is not a wave file at all");
    const ScanResult result = scan(tree);
    EXPECT_NE(findFile(result.added, good), nullptr);
    const QStringList unreadable = unreadablePaths(result);
    for (const QString &path : {empty, garbage, notAudio}) {
        EXPECT_TRUE(unreadable.contains(path)) << path.toStdString();
    }
    for (const auto &[path, reason] : result.unreadable) {
        EXPECT_FALSE(reason.isEmpty()) << path.toStdString();
    }
    // A truncated file is either unreadable or read with what its header says: never a crash.
    EXPECT_TRUE(unreadable.contains(truncated) || findFile(result.added, truncated) != nullptr);
}

TEST(Scanner, TooManyFilesRemoveNothing)
{
    TestTree tree;
    for (int i = 0; i < 5; ++i) {
        tree.add(u"Artist - Track %1.mp3"_s.arg(i));
    }
    ScanOptions options;
    options.maxFiles = 2;
    const QString deleted = tree.path(u"Artist - Deleted.mp3"_s);
    const ScanResult result = scan(tree, {{deleted, {deleted, 1, 1}}}, options);
    EXPECT_EQ(result.added.size(), 2);
    EXPECT_TRUE(result.removed.isEmpty());
    EXPECT_FALSE(result.unreadable.isEmpty());
}

TEST(Scanner, FoldersNestedTooDeeplyAreReported)
{
    TestTree tree;
    tree.add(u"a/b/c/d/Artist - Deep.mp3"_s);
    const QString shallow = tree.add(u"a/Artist - Shallow.mp3"_s);
    ScanOptions options;
    options.maxDepth = 2;
    const ScanResult result = scan(tree, {}, options);
    EXPECT_EQ(paths(result.added), QStringList{shallow});
    EXPECT_EQ(unreadablePaths(result), QStringList{tree.path(u"a/b/c"_s)});
}

TEST(Scanner, CancellingBeforeStartReturnsNothing)
{
    TestTree tree;
    tree.add(u"Artist - Title.mp3"_s);
    selecta::audio::CancellationFlag cancel;
    cancel.cancel();
    const ScanResult result = selecta::collection::scan({tree.root()}, {}, {}, &cancel);
    EXPECT_TRUE(result.cancelled);
    EXPECT_TRUE(result.added.isEmpty());
    EXPECT_TRUE(result.removed.isEmpty());
}

TEST(Scanner, CancellingDuringTheReadKeepsWhatWasRead)
{
    TestTree tree;
    for (int i = 0; i < 12; ++i) {
        tree.add(u"Artist - Track %1.mp3"_s.arg(i, 2, 10, QChar(u'0')));
    }
    const QString deleted = tree.path(u"Artist - Deleted.mp3"_s);
    selecta::audio::CancellationFlag cancel;
    ScanOptions options;
    options.readers = 1;
    const ScanResult result = selecta::collection::scan({tree.root()}, {{deleted, {deleted, 1, 1}}}, options, &cancel,
                                                        [&cancel](int done, int /*total*/) {
                                                            if (done >= 3) {
                                                                cancel.cancel();
                                                            }
                                                        });
    EXPECT_TRUE(result.cancelled);
    EXPECT_GE(result.added.size(), 3);
    EXPECT_LT(result.added.size(), 12);
    EXPECT_TRUE(result.removed.isEmpty()); // removals need the whole picture
}

TEST(Scanner, ResultsDontDependOnTheNumberOfReaders)
{
    TestTree tree;
    for (int i = 0; i < 10; ++i) {
        tree.add(u"Folder %1/Artist - Track %2.mp3"_s.arg(i % 3).arg(i), i % 2 == 0 ? Format::Mp3 : Format::Flac16,
                 tags(u"Artist"_s, u"Track %1"_s.arg(i)));
    }
    ScanOptions one;
    one.readers = 1;
    ScanOptions many;
    many.readers = 8;
    const ScanResult a = scan(tree, {}, one);
    const ScanResult b = scan(tree, {}, many);
    ASSERT_EQ(a.added.size(), 10);
    ASSERT_EQ(paths(a.added), paths(b.added));
    for (qsizetype i = 0; i < a.added.size(); ++i) {
        EXPECT_EQ(a.added.at(i).title, b.added.at(i).title);
        EXPECT_EQ(a.added.at(i).durationMs, b.added.at(i).durationMs);
    }
}

TEST(Scanner, NeverWritesUnderTheRoots)
{
    TestTree tree;
    const QString mp3 = tree.add(u"Artist - Title.mp3"_s, Format::Mp3, tags(u"Artist"_s, u"Title"_s));
    const QString flac = tree.add(u"Artist - Other.flac"_s, Format::Flac16, tags(u"Artist"_s, u"Other"_s));
    const auto snapshot = [&] {
        QList<QPair<QByteArray, qint64>> state;
        for (const QString &path : {mp3, flac}) {
            QFile file(path);
            EXPECT_TRUE(file.open(QIODevice::ReadOnly));
            state.append({file.readAll(), QFileInfo(path).lastModified().toMSecsSinceEpoch()});
        }
        return state;
    };
    const auto before = snapshot();
    // Read-only files and a read-only folder: a scan needs no write access at all.
    ::chmod(QFile::encodeName(mp3).constData(), 0444);
    ::chmod(QFile::encodeName(flac).constData(), 0444);
    ::chmod(QFile::encodeName(tree.root()).constData(), 0555);
    const ScanResult result = scan(tree);
    ::chmod(QFile::encodeName(tree.root()).constData(), 0755);
    EXPECT_EQ(result.added.size(), 2);
    EXPECT_TRUE(result.unreadable.isEmpty());
    EXPECT_EQ(snapshot(), before);
    EXPECT_EQ(QDir(tree.root()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 2);
}

} // namespace
