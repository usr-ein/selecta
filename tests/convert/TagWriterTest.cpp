// SPDX-License-Identifier: GPL-3.0-or-later
// C1: tags written per format and read back twice, through Selecta's reader and independently through TagLib's
// format classes, including the ID3v2.3 compensations (UTF-16, TXXX for v2.4-only frames, joined values, Latin-1
// APIC description, no ID3v1/APE).
#include "convert/TagWriter.h"

#include "ConvertTestUtil.h"
#include "DecodeHelpers.h"
#include "SyntheticAudio.h"
#include "convert/TagReader.h"

#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <taglib/aifffile.h>
#include <taglib/apetag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/generalencapsulatedobjectframe.h>
#include <taglib/id3v1tag.h>
#include <taglib/id3v2tag.h>
#include <taglib/infotag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/privateframe.h>
#include <taglib/textidentificationframe.h>
#include <taglib/tpropertymap.h>
#include <taglib/wavfile.h>
#include <taglib/xiphcomment.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using namespace selecta::testing;

namespace {

QString textOf(const TagLib::ID3v2::Tag *tag, const char *frameId)
{
    const TagLib::ID3v2::FrameList frames = tag->frameListMap()[frameId];
    return frames.isEmpty() ? QString() : QString::fromStdString(frames.front()->toString().to8Bit(true));
}

const TagLib::ID3v2::TextIdentificationFrame *textFrame(const TagLib::ID3v2::Tag *tag, const char *frameId)
{
    const TagLib::ID3v2::FrameList frames = tag->frameListMap()[frameId];
    return frames.isEmpty() ? nullptr : dynamic_cast<const TagLib::ID3v2::TextIdentificationFrame *>(frames.front());
}

const TagLib::ID3v2::UserTextIdentificationFrame *txxx(const TagLib::ID3v2::Tag *tag, const QString &description)
{
    return TagLib::ID3v2::UserTextIdentificationFrame::find(
        const_cast<TagLib::ID3v2::Tag *>(tag), // NOLINT
        TagLib::String(description.toStdString(), TagLib::String::UTF8));
}

QByteArray encodePath(const QString &path)
{
    return QFile::encodeName(path);
}

// The size of the ID3v2 tag at the start of a file, header included (its size field is four 7-bit bytes), or 0.
qsizetype id3v2TagSize(const QByteArray &bytes)
{
    if (bytes.size() < 10 || !bytes.startsWith("ID3")) {
        return 0;
    }
    quint32 size = 0;
    for (qsizetype i = 6; i < 10; ++i) {
        size = (size << 7U) | (static_cast<quint32>(static_cast<quint8>(bytes.at(i))) & 0x7FU);
    }
    return 10 + static_cast<qsizetype>(size);
}

// What Selecta's reader returns for every text field the model had (fields TagLib renames on read aside).
void expectFieldsMatch(const MetadataModel &expected, const MetadataModel &actual)
{
    const QMap<QString, MetadataModel::Field> &fields = expected.fields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        EXPECT_EQ(actual.values(it.key()), it.value().values) << it.key().toStdString();
    }
}

class TagWriting : public ::testing::Test
{
protected:
    QTemporaryDir dir;
    MetadataModel model = richModel();
    selecta::testing::Pcm pcm = sine(440, 1.0);

    QString file(const QString &name, const QByteArray &bytes) { return writeFile(dir.path(), name, bytes); }
};

} // namespace

TEST_F(TagWriting, Mp3Id3v23)
{
    const QString path = file(u"song.mp3"_s, makeMp3(pcm, {.id3v1 = true}));
    ASSERT_TRUE(writeTags(path, model).ok());

    // Selecta's reader sees the v2.3 view: joined artists, every field, the cover byte for byte.
    const Result<MetadataModel> read = readTags(path);
    ASSERT_TRUE(read.ok()) << read.error().toString().toStdString();
    expectFieldsMatch(id3v23View(model), read.value());
    EXPECT_EQ(read.value().value(keys::Artist), u"Kerri Vale, AC/DC Tribute Band"_s); // ", " never "/"
    ASSERT_TRUE(read.value().frontCover());
    EXPECT_EQ(read.value().frontCover()->data, model.frontCover()->data);

    // Independently, through TagLib's MPEG classes.
    const QByteArray native = encodePath(path);
    TagLib::MPEG::File mpeg(native.constData());
    ASSERT_TRUE(mpeg.isValid());
    EXPECT_FALSE(mpeg.hasID3v1Tag()); // stripped, never written
    EXPECT_FALSE(mpeg.hasAPETag());
    ASSERT_TRUE(mpeg.hasID3v2Tag());
    const TagLib::ID3v2::Tag *tag = mpeg.ID3v2Tag();
    EXPECT_EQ(tag->header()->majorVersion(), 3U);
    const TagLib::ID3v2::TextIdentificationFrame *title = textFrame(tag, "TIT2");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->textEncoding(), TagLib::String::UTF16);
    const TagLib::ID3v2::TextIdentificationFrame *artist = textFrame(tag, "TPE1");
    ASSERT_NE(artist, nullptr);
    EXPECT_EQ(artist->fieldList().size(), 1U);              // one joined value: v2.3 readers see everything
    EXPECT_EQ(textOf(tag, "TPUB"), u"Fictional Records"_s); // label (D30)
    EXPECT_EQ(textOf(tag, "TSRC"), u"XX0002400001"_s);      // ISRC (D30)
    EXPECT_EQ(textOf(tag, "TKEY"), u"8A"_s);                // verbatim
    EXPECT_EQ(textOf(tag, "TBPM"), u"127.98"_s);            // decimals kept
    EXPECT_EQ(textOf(tag, "TRCK"), u"3/12"_s);
    // TagLib upgrades TYER/TDAT to TDRC when it reads a tag, so look at the bytes on disk for the v2.3 date frames.
    const QByteArray bytes = readFile(path);
    const QByteArray tagBytes = bytes.left(id3v2TagSize(bytes));
    EXPECT_TRUE(tagBytes.contains("TYER"));
    EXPECT_TRUE(tagBytes.contains("TDAT"));
    EXPECT_FALSE(tagBytes.contains("TDRC")); // a v2.4 frame
    EXPECT_EQ(textOf(tag, "WOAS"), u"https://example.invalid/track/123"_s);
    // v2.4-only frames become TXXX in v2.3.
    for (const char *dropped : {"TMOO", "TSST", "TDRL", "TDTG", "TPRO"}) {
        EXPECT_TRUE(tag->frameListMap()[dropped].isEmpty()) << dropped;
    }
    ASSERT_NE(txxx(tag, u"MOOD"_s), nullptr);
    EXPECT_EQ(txxx(tag, u"MOOD"_s)->textEncoding(), TagLib::String::UTF16);
    EXPECT_NE(txxx(tag, u"DISCSUBTITLE"_s), nullptr);
    EXPECT_NE(txxx(tag, u"RELEASEDATE"_s), nullptr);
    EXPECT_NE(txxx(tag, u"MYCUSTOMFIELD"_s), nullptr);
    // Exactly one APIC: front cover, empty Latin-1 description.
    const TagLib::ID3v2::FrameList pictures = tag->frameListMap()["APIC"];
    ASSERT_EQ(pictures.size(), 1U);
    const auto *apic = dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame *>(pictures.front());
    ASSERT_NE(apic, nullptr);
    EXPECT_EQ(apic->type(), TagLib::ID3v2::AttachedPictureFrame::FrontCover);
    EXPECT_EQ(apic->textEncoding(), TagLib::String::Latin1);
    EXPECT_TRUE(apic->description().isEmpty());
    EXPECT_EQ(apic->mimeType(), "image/jpeg");
}

TEST_F(TagWriting, Mp3Id3v24KeepsNativeFrames)
{
    const QString path = file(u"song.mp3"_s, makeMp3(pcm, {}));
    TagWriteOptions options;
    options.id3Version = Id3Version::V24;
    ASSERT_TRUE(writeTags(path, model, options).ok());
    const QByteArray native = encodePath(path);
    TagLib::MPEG::File mpeg(native.constData());
    const TagLib::ID3v2::Tag *tag = mpeg.ID3v2Tag();
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->header()->majorVersion(), 4U);
    EXPECT_FALSE(tag->frameListMap()["TMOO"].isEmpty());
    const TagLib::ID3v2::TextIdentificationFrame *artist = textFrame(tag, "TPE1");
    ASSERT_NE(artist, nullptr);
    EXPECT_EQ(artist->fieldList().size(), 2U); // native multiple values
    EXPECT_EQ(artist->textEncoding(), TagLib::String::UTF8);
    const Result<MetadataModel> read = readTags(path);
    ASSERT_TRUE(read.ok());
    expectFieldsMatch(model, read.value());
}

TEST_F(TagWriting, RewritingDropsEveryOldTag)
{
    const QString path = file(u"old.mp3"_s, makeMp3(pcm, {.id3v1 = true}));
    {
        // Someone else's tags: Serato GEOB, a Traktor PRIV, ReplayGain, an APE tag.
        const QByteArray native = encodePath(path);
        TagLib::MPEG::File mpeg(native.constData());
        TagLib::ID3v2::Tag *tag = mpeg.ID3v2Tag(true);
        auto *geob = new TagLib::ID3v2::GeneralEncapsulatedObjectFrame;
        geob->setDescription("Serato Markers2");
        geob->setObject(TagLib::ByteVector("secret cues"));
        tag->addFrame(geob);
        auto *priv = new TagLib::ID3v2::PrivateFrame;
        priv->setOwner("TRAKTOR4");
        priv->setData(TagLib::ByteVector("grid"));
        tag->addFrame(priv);
        tag->addFrame(new TagLib::ID3v2::UserTextIdentificationFrame("REPLAYGAIN_TRACK_GAIN", {"-7.0 dB"}));
        mpeg.APETag(true)->setTitle("ape title");
        ASSERT_TRUE(mpeg.save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v4));
    }
    MetadataModel fresh;
    fresh.set(keys::Title, u"Fresh"_s, Provenance::Release);
    ASSERT_TRUE(writeTags(path, fresh).ok());
    const QByteArray native = encodePath(path);
    TagLib::MPEG::File mpeg(native.constData());
    EXPECT_FALSE(mpeg.hasAPETag());
    EXPECT_FALSE(mpeg.hasID3v1Tag());
    const TagLib::ID3v2::Tag *tag = mpeg.ID3v2Tag();
    ASSERT_NE(tag, nullptr);
    EXPECT_TRUE(tag->frameListMap()["GEOB"].isEmpty());
    EXPECT_TRUE(tag->frameListMap()["PRIV"].isEmpty());
    EXPECT_EQ(txxx(tag, u"REPLAYGAIN_TRACK_GAIN"_s), nullptr);
    EXPECT_EQ(textOf(tag, "TIT2"), u"Fresh"_s);
    // The audio is untouched.
    const selecta::testing::Decoded decoded = decodeAll(readFile(path));
    EXPECT_EQ(decoded.pcm.frames(), pcm.frames());
}

TEST_F(TagWriting, AiffGetsAnId3Chunk)
{
    const QString path = file(u"song.aiff"_s, makeAiff(quantised(pcm, 16)));
    ASSERT_TRUE(writeTags(path, model).ok());
    const QByteArray native = encodePath(path);
    const TagLib::RIFF::AIFF::File aiff(native.constData());
    ASSERT_TRUE(aiff.hasID3v2Tag());
    EXPECT_EQ(aiff.tag()->header()->majorVersion(), 3U);
    EXPECT_EQ(textOf(aiff.tag(), "TPUB"), u"Fictional Records"_s);
    EXPECT_EQ(aiff.tag()->frameListMap()["APIC"].size(), 1U);
    const Result<MetadataModel> read = readTags(path);
    ASSERT_TRUE(read.ok());
    expectFieldsMatch(id3v23View(model), read.value());
    EXPECT_EQ(decodeAll(readFile(path)).pcm.frames(), pcm.frames()); // still decodes
}

TEST_F(TagWriting, WavGetsInfoAndId3)
{
    const QString path = file(u"song.wav"_s, makeWav(quantised(pcm, 16)));
    ASSERT_TRUE(writeTags(path, model).ok());
    const QByteArray native = encodePath(path);
    const TagLib::RIFF::WAV::File wav(native.constData());
    ASSERT_TRUE(wav.hasInfoTag()); // what rekordbox reads
    ASSERT_TRUE(wav.hasID3v2Tag());
    const TagLib::PropertyMap info = wav.InfoTag()->properties();
    EXPECT_FALSE(info["TITLE"].isEmpty());
    EXPECT_EQ(QString::fromStdString(info["ARTIST"].front().to8Bit(true)), u"Kerri Vale, AC/DC Tribute Band"_s);
    EXPECT_EQ(wav.ID3v2Tag()->header()->majorVersion(), 3U);
    EXPECT_EQ(decodeAll(readFile(path)).pcm.frames(), pcm.frames());
}

TEST_F(TagWriting, FlacGetsVorbisCommentsAndAPictureBlock)
{
    const QString path = file(u"song.flac"_s, makeFlac(quantised(pcm, 16), {.id3v2Bytes = 2000}));
    ASSERT_TRUE(writeTags(path, model).ok());
    const QByteArray native = encodePath(path);
    TagLib::FLAC::File flac(native.constData());
    ASSERT_TRUE(flac.isValid());
    EXPECT_FALSE(flac.hasID3v2Tag()); // the prepended ID3 tag is gone
    ASSERT_NE(flac.xiphComment(), nullptr);
    EXPECT_EQ(flac.xiphComment()->fieldListMap()["ARTIST"].size(), 2U); // multiple values native
    ASSERT_EQ(flac.pictureList().size(), 1U);
    const TagLib::FLAC::Picture *picture = flac.pictureList().front();
    EXPECT_EQ(picture->type(), TagLib::FLAC::Picture::FrontCover);
    EXPECT_EQ(picture->width(), 300);
    EXPECT_EQ(picture->height(), 300);
    const Result<MetadataModel> read = readTags(path);
    ASSERT_TRUE(read.ok());
    expectFieldsMatch(model, read.value());
    EXPECT_EQ(decodeAll(readFile(path)).pcm.frames(), pcm.frames());
}

TEST_F(TagWriting, M4aKeyAtomForRekordbox)
{
    const QString path = file(u"song.m4a"_s, makeM4a(pcm, {}));
    ASSERT_TRUE(writeTags(path, model).ok());
    const QByteArray native = encodePath(path);
    const TagLib::MP4::File mp4(native.constData());
    ASSERT_TRUE(mp4.isValid());
    const TagLib::MP4::Tag *tag = mp4.tag();
    ASSERT_TRUE(tag->contains("----:com.apple.iTunes:KEY"));
    EXPECT_EQ(tag->item("----:com.apple.iTunes:KEY").toStringList().front(), "8A");
    EXPECT_TRUE(tag->contains("covr"));
    const Result<MetadataModel> read = readTags(path);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().value(keys::Title), model.value(keys::Title));
    EXPECT_EQ(read.value().value(keys::Label), u"Fictional Records"_s);
}

TEST_F(TagWriting, FailuresAreReported)
{
    EXPECT_EQ(writeTags(dir.filePath(u"missing.mp3"_s), model).error().code, ErrorCode::OpenFailed);
    const QString ogg = file(u"song.oga"_s, makeOggFlac(quantised(pcm, 16)));
    EXPECT_EQ(writeTags(ogg, model).error().code, ErrorCode::Unsupported);
    const QString garbage = file(u"garbage.mp3"_s, randomBytes(5000, 3));
    EXPECT_EQ(writeTags(garbage, model).error().code, ErrorCode::Unsupported);
    // canWriteTags() says the same as writeTags() does, before trying (P-025: the Inbox converts what it can't tag).
    using selecta::audio::Container;
    for (const auto container :
         {Container::MpegAudio, Container::Aiff, Container::Wav, Container::Flac, Container::Mp4}) {
        EXPECT_TRUE(canWriteTags(container));
    }
    for (const auto container : {Container::Ogg, Container::Other, Container::Unknown}) {
        EXPECT_FALSE(canWriteTags(container));
    }
    const QString opus = file(u"song.opus"_s, makeOpus(sine(440.0, 1.0, 48000)));
    EXPECT_EQ(writeTags(opus, model).error().code, ErrorCode::Unsupported);
    const QString readOnly = file(u"readonly.mp3"_s, makeMp3(pcm, {}));
    QFile::setPermissions(readOnly, QFileDevice::ReadOwner);
    EXPECT_EQ(writeTags(readOnly, model).error().code, ErrorCode::OpenFailed);
    QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

TEST(TagReading, AliasesTotalsAndBounds)
{
    const QTemporaryDir dir;
    const QString path = writeFile(dir.path(), u"aliases.flac"_s, makeFlac(quantised(sine(440, 0.5), 16)));
    {
        const QByteArray native = QFile::encodeName(path);
        TagLib::FLAC::File flac(native.constData());
        TagLib::Ogg::XiphComment *comment = flac.xiphComment(true);
        comment->addField("ALBUM ARTIST", "Someone");
        comment->addField("TRACKNUMBER", "4");
        comment->addField("TOTALTRACKS", "9");
        comment->addField("DISCNUMBER", "1");
        comment->addField("DISCTOTAL", "2");
        comment->addField("ORGANIZATION", "A Label");
        comment->addField("KEY", "11B");
        comment->addField("DATE", "2020-01-02");
        comment->addField("YEAR", "2020");
        auto *picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/jpeg");
        picture->setData(TagLib::ByteVector(4096, 'x'));
        flac.addPicture(picture);
        ASSERT_TRUE(flac.save());
    }
    const Result<MetadataModel> read = readTags(path);
    ASSERT_TRUE(read.ok());
    const MetadataModel &m = read.value();
    EXPECT_EQ(m.value(keys::AlbumArtist), u"Someone"_s);
    EXPECT_EQ(m.value(keys::TrackNumber), u"4/9"_s);
    EXPECT_EQ(m.value(keys::DiscNumber), u"1/2"_s);
    EXPECT_FALSE(m.contains(u"TOTALTRACKS"));
    EXPECT_EQ(m.value(keys::Label), u"A Label"_s);
    EXPECT_EQ(m.value(keys::InitialKey), u"11B"_s);
    EXPECT_EQ(m.values(keys::Date), QStringList{u"2020-01-02"_s}); // DATE wins over the YEAR alias
    EXPECT_EQ(m.provenance(keys::Label), Provenance::Embedded);
    ASSERT_EQ(m.pictures().size(), 1);

    TagReadOptions tight;
    tight.maxPictureBytes = 1000;
    EXPECT_TRUE(readTags(path, tight).value().pictures().isEmpty()); // too big: skipped, not loaded
}

TEST(TagReading, Failures)
{
    const QTemporaryDir dir;
    EXPECT_EQ(readTags(dir.filePath(u"none"_s)).error().code, ErrorCode::OpenFailed);
    EXPECT_EQ(readTags(dir.path()).error().code, ErrorCode::OpenFailed);
    const QString garbage = writeFile(dir.path(), u"x.bin"_s, randomBytes(1000, 1));
    EXPECT_EQ(readTags(garbage).error().code, ErrorCode::Unsupported);
    // A file with no tags reads as an empty model.
    const QString plain = writeFile(dir.path(), u"plain.wav"_s, makeWav(sine(440, 0.2)));
    const Result<MetadataModel> empty = readTags(plain);
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty.value().isEmpty());
}

TEST_F(TagWriting, PredictionMatchesWhatEveryFormatReadsBack)
{
    MetadataModel edgy = richModel();
    edgy.set(keys::Genre, QStringList{u"17"_s, u"Techno"_s}, Provenance::Release); // a numeric ID3v1 genre
    edgy.set(keys::TrackNumber, u"A1"_s, Provenance::Release);                     // a vinyl side
    edgy.set(u"TRACKTOTAL"_s, u"12"_s, Provenance::Release);
    edgy.set(u"YEAR"_s, u"1999"_s, Provenance::Release); // an alias next to its canonical DATE
    edgy.set(u"1STARTSWITHDIGIT"_s, u"x"_s, Provenance::Release);
    edgy.set(u"COMPILATION"_s, u"1"_s, Provenance::Release);
    edgy.set(u"PRODUCER"_s, QStringList{u"P One"_s, u"P Two"_s}, Provenance::Release);
    edgy.set(u"COMMENT:SPECIAL"_s, u"described comment"_s, Provenance::Release);
    MetadataModel numericGenre = richModel();
    numericGenre.set(keys::Genre, u"17"_s, Provenance::Release);

    struct Case
    {
        QString name;
        QByteArray bytes;
        selecta::audio::Container container = selecta::audio::Container::Unknown;
        Id3Version version = Id3Version::V23;
    };
    using selecta::audio::Container;
    const QList<Case> cases = {
        {u"a.mp3"_s, makeMp3(pcm, {}), Container::MpegAudio, Id3Version::V23},
        {u"b.mp3"_s, makeMp3(pcm, {}), Container::MpegAudio, Id3Version::V24},
        {u"c.aiff"_s, makeAiff(quantised(pcm, 16)), Container::Aiff, Id3Version::V23},
        {u"d.wav"_s, makeWav(quantised(pcm, 16)), Container::Wav, Id3Version::V24},
        {u"e.flac"_s, makeFlac(quantised(pcm, 16)), Container::Flac, Id3Version::V23},
        {u"f.m4a"_s, makeM4a(pcm, {}), Container::Mp4, Id3Version::V23},
        {u"g.m4a"_s, makeM4a(pcm, {.alac = true}), Container::Mp4, Id3Version::V23},
    };
    for (const MetadataModel &model : {edgy, numericGenre}) {
        for (const Case &c : cases) {
            SCOPED_TRACE(c.name.toStdString());
            const QString path = file(c.name, c.bytes);
            TagWriteOptions options;
            options.id3Version = c.version;
            ASSERT_TRUE(writeTags(path, model, options).ok());
            const Result<MetadataModel> read = readTags(path);
            ASSERT_TRUE(read.ok());
            const MetadataModel predicted = tagsAsWritten(model, c.container, options);
            const QStringList predictedKeys = predicted.keys();
            EXPECT_EQ(read.value().keys(), predictedKeys);
            for (const QString &key : predictedKeys) {
                EXPECT_EQ(read.value().values(key), predicted.values(key)) << key.toStdString();
            }
        }
    }
    // The conversions spelled out.
    const MetadataModel mp4 = tagsAsWritten(edgy, Container::Mp4);
    EXPECT_FALSE(mp4.contains(keys::TrackNumber)); // "A1" doesn't fit MP4's integer pair
    EXPECT_EQ(mp4.value(keys::Bpm), u"128"_s);     // rounded, where TagLib alone would truncate to 127
    EXPECT_FALSE(mp4.contains(u"1STARTSWITHDIGIT"_s));
    EXPECT_EQ(mp4.value(keys::InitialKey), u"8A"_s); // through the KEY atom
    EXPECT_EQ(tagsAsWritten(numericGenre, Container::MpegAudio).value(keys::Genre), u"Rock"_s);
    EXPECT_EQ(tagsAsWritten(edgy, Container::MpegAudio).value(keys::Genre), u"17; Techno"_s); // v2.3: joined
    EXPECT_EQ(tagsAsWritten(edgy, Container::Flac).value(keys::TrackNumber), u"A1/12"_s);
    EXPECT_FALSE(tagsAsWritten(edgy, Container::Flac).contains(u"YEAR"_s)); // DATE wins, as when reading
    EXPECT_TRUE(tagsAsWritten(edgy, Container::Flac).pictures().isEmpty());
}
