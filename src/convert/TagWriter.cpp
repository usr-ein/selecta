// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/TagWriter.h"

#include "convert/TagReader.h"
#include "convert/detail/TagLibUtil.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>

#include <array>
#include <limits>
#include <taglib/aifffile.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/commentsframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/infotag.h>
#include <taglib/mp4coverart.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/mp4itemfactory.h>
#include <taglib/mp4tag.h>
#include <taglib/mpegfile.h>
#include <taglib/textidentificationframe.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/urllinkframe.h>
#include <taglib/wavfile.h>
#include <taglib/xiphcomment.h>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

// Frames TagLib drops when it saves ID3v2.3 (downgradeFrames); written as TXXX instead (conversion §3).
constexpr std::array<QStringView, 5> kV24OnlyKeys = {keys::Mood, keys::DiscSubtitle, keys::ReleaseDate,
                                                     keys::TaggingDate, keys::ProducedNotice};
// Joined with the artist separator in ID3v2.3 (D-6).
constexpr std::array<QStringView, 7> kArtistKeys = {keys::Artist,  keys::AlbumArtist, keys::Composer,   keys::Lyricist,
                                                    keys::Remixer, keys::Conductor,   u"ORIGINALARTIST"};
constexpr auto kMp4KeyAtom = "----:com.apple.iTunes:KEY";
// MP4 stores track, disc and BPM numbers as 16-bit integers.
constexpr int kMp4MaxNumber = std::numeric_limits<qint16>::max();

bool isV24Only(QStringView key)
{
    return std::ranges::find(kV24OnlyKeys, key) != kV24OnlyKeys.end();
}

bool isArtistKey(QStringView key)
{
    return std::ranges::find(kArtistKeys, key) != kArtistKeys.end();
}

// URLs stay separate frames (one WXXX each) rather than being joined into one text.
bool keepsSeparateValues(const QString &key)
{
    return key == u"URL"_s || key.startsWith(u"URL:"_s);
}

TagLib::PropertyMap toProperties(const MetadataModel &model, bool skipV24Only)
{
    TagLib::PropertyMap properties;
    const QMap<QString, MetadataModel::Field> &fields = model.fields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (skipV24Only && isV24Only(it.key())) {
            continue;
        }
        properties.insert(detail::toTagLib(it.key()), detail::toTagLib(it.value().values));
    }
    return properties;
}

void logRejected(const TagLib::PropertyMap &rejected, const QString &path)
{
    if (path.isEmpty()) {
        return; // a prediction (tagsAsWritten), not a file
    }
    for (const auto &[key, values] : rejected) {
        qCDebug(lcConvert) << "TagLib can't store" << detail::fromTagLib(key) << "in" << path;
    }
}

// The text frames of the ID3v2 tag writeTags() builds: the v2.3 view and TXXX compensation for v2.3, explicit text
// encodings.
void fillId3v2Text(TagLib::ID3v2::Tag *tag, const MetadataModel &model, const TagWriteOptions &options,
                   const QString &path)
{
    const bool v23 = options.id3Version == Id3Version::V23;
    const MetadataModel view = v23 ? id3v23View(model, options) : model;
    // v2.3 knows Latin-1 and UTF-16; v2.4 adds UTF-8.
    const TagLib::String::Type encoding = v23 ? TagLib::String::UTF16 : TagLib::String::UTF8;
    logRejected(tag->setProperties(toProperties(view, v23)), path);
    if (v23) {
        const QMap<QString, MetadataModel::Field> &fields = view.fields();
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            if (isV24Only(it.key())) {
                tag->addFrame(new TagLib::ID3v2::UserTextIdentificationFrame(
                    detail::toTagLib(it.key()), detail::toTagLib(it.value().values), encoding));
            }
        }
    }
    // Frames created from properties hold UTF-8 internally; set the encoding explicitly so the file doesn't depend
    // on TagLib's conversion at render time.
    for (TagLib::ID3v2::Frame *frame : tag->frameList()) {
        if (auto *text = dynamic_cast<TagLib::ID3v2::TextIdentificationFrame *>(frame)) {
            text->setTextEncoding(encoding);
        } else if (auto *comment = dynamic_cast<TagLib::ID3v2::CommentsFrame *>(frame)) {
            comment->setTextEncoding(encoding);
        } else if (auto *lyrics = dynamic_cast<TagLib::ID3v2::UnsynchronizedLyricsFrame *>(frame)) {
            lyrics->setTextEncoding(encoding);
        } else if (auto *url = dynamic_cast<TagLib::ID3v2::UserUrlLinkFrame *>(frame)) {
            url->setTextEncoding(encoding);
        }
    }
}

void fillId3v2(TagLib::ID3v2::Tag *tag, const MetadataModel &model, const TagWriteOptions &options, const QString &path)
{
    fillId3v2Text(tag, model, options, path);
    if (const std::optional<Picture> cover = model.frontCover()) {
        // One front cover, empty description in Latin-1 (conversion §3: what Engine DJ and older readers need).
        auto *apic = new TagLib::ID3v2::AttachedPictureFrame;
        apic->setTextEncoding(TagLib::String::Latin1);
        apic->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
        apic->setMimeType(detail::toTagLib(cover->mimeType.isEmpty() ? u"image/jpeg"_s : cover->mimeType));
        apic->setDescription(TagLib::String());
        apic->setPicture(detail::toTagLib(cover->data));
        tag->addFrame(apic);
    }
}

// "n" or "n/N" with whole numbers MP4's 16-bit fields hold, normalised as TagLib reads it back ("03/12" → "3/12",
// "3/0" → "3"); empty for anything else ("A1", "1-2", "").
QString mp4NumberPair(const QString &value)
{
    static const QRegularExpression pattern(uR"(^\s*(\d{1,5})\s*(?:/\s*(\d{1,5})\s*)?$)"_s);
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch()) {
        return {};
    }
    const int number = match.captured(1).toInt();
    const int total = match.hasCaptured(2) ? match.captured(2).toInt() : 0;
    if (number > kMp4MaxNumber || total > kMp4MaxNumber) {
        return {};
    }
    return total > 0 ? u"%1/%2"_s.arg(number).arg(total) : QString::number(number);
}

// A BPM as the whole number MP4's `tmpo` atom holds (rounded, where TagLib would truncate); empty when it isn't one.
QString mp4Bpm(const QString &value)
{
    bool ok = false;
    const double bpm = value.trimmed().toDouble(&ok);
    if (!ok || !(bpm >= 0.5) || bpm >= kMp4MaxNumber + 0.5) {
        return {};
    }
    return QString::number(qRound(bpm));
}

struct Mp4Fields
{
    TagLib::PropertyMap properties; // for MP4::Tag::setProperties()
    QStringList musicalKey;         // for the ----:com.apple.iTunes:KEY atom
};

// The field as MP4 stores it, or nothing: track and disc numbers that MP4's integer pairs can't hold are left out, BPM
// is rounded, and keys TagLib can't render as an atom (not starting with A–Z, or with a ':' that a free-form atom name
// can't carry) are dropped.
std::optional<QStringList> mp4Values(const QString &key, const QStringList &values)
{
    QStringList stored = values;
    if (key == keys::TrackNumber || key == keys::DiscNumber || key == keys::Bpm) {
        const QString number = key == keys::Bpm ? mp4Bpm(values.first()) : mp4NumberPair(values.first());
        if (number.isEmpty()) {
            return std::nullopt;
        }
        stored = QStringList{number};
    }
    const TagLib::MP4::ItemFactory *factory = TagLib::MP4::ItemFactory::instance();
    const auto [name, item] = factory->itemFromProperty(detail::toTagLib(key), detail::toTagLib(stored));
    if (name.isEmpty() || !item.isValid() || factory->renderItem(name, item).isEmpty()) {
        return std::nullopt;
    }
    return stored;
}

// What writeMp4() stores: INITIALKEY goes to the KEY atom rekordbox reads, the rest as mp4Values() allows.
Mp4Fields mp4Fields(const MetadataModel &model, const QString &path)
{
    Mp4Fields out;
    const QMap<QString, MetadataModel::Field> &fields = model.fields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        const QString &key = it.key();
        const QStringList &values = it.value().values;
        if (key == keys::InitialKey) {
            out.musicalKey = values;
            continue;
        }
        const std::optional<QStringList> stored = mp4Values(key, values);
        if (!stored) {
            if (!path.isEmpty()) {
                qCDebug(lcConvert) << "MP4 can't store" << key << values.first() << "in" << path;
            }
            continue;
        }
        out.properties.insert(detail::toTagLib(key), detail::toTagLib(*stored));
    }
    return out;
}

Result<void> saveFailed(const QString &path)
{
    return makeError(ErrorCode::TagError,
                     u"TagLib couldn't save the tags of %1 (disk full or not writable?)"_s.arg(path));
}

TagLib::ID3v2::Version id3Version(const TagWriteOptions &options)
{
    return options.id3Version == Id3Version::V23 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4;
}

Result<void> writeMpeg(TagLib::IOStream &stream, const QString &path, const MetadataModel &model,
                       const TagWriteOptions &options)
{
    TagLib::MPEG::File file(&stream, /*readProperties=*/false);
    if (!file.isValid()) {
        return makeError(ErrorCode::TagError, u"%1 is not an MPEG audio file TagLib can tag"_s.arg(path));
    }
    file.strip(TagLib::MPEG::File::AllTags); // ID3v1, APE and the old ID3v2 go: the tag is rebuilt from the model
    fillId3v2(file.ID3v2Tag(true), model, options, path);
    if (!file.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, id3Version(options),
                   TagLib::File::DoNotDuplicate)) {
        return saveFailed(path);
    }
    return {};
}

void clearId3v2(TagLib::ID3v2::Tag *tag)
{
    const TagLib::ID3v2::FrameList frames = tag->frameList();
    for (TagLib::ID3v2::Frame *frame : frames) {
        tag->removeFrame(frame, true);
    }
}

Result<void> writeAiff(TagLib::IOStream &stream, const QString &path, const MetadataModel &model,
                       const TagWriteOptions &options)
{
    TagLib::RIFF::AIFF::File file(&stream, /*readProperties=*/false);
    if (!file.isValid()) {
        return makeError(ErrorCode::TagError, u"%1 is not an AIFF file TagLib can tag"_s.arg(path));
    }
    clearId3v2(file.tag());
    fillId3v2(file.tag(), model, options, path);
    if (!file.save(id3Version(options))) {
        return saveFailed(path);
    }
    return {};
}

Result<void> writeWav(TagLib::IOStream &stream, const QString &path, const MetadataModel &model,
                      const TagWriteOptions &options)
{
    TagLib::RIFF::WAV::File file(&stream, /*readProperties=*/false);
    if (!file.isValid()) {
        return makeError(ErrorCode::TagError, u"%1 is not a WAV file TagLib can tag"_s.arg(path));
    }
    file.strip(TagLib::RIFF::WAV::File::AllTags);
    clearId3v2(file.ID3v2Tag());
    fillId3v2(file.ID3v2Tag(), model, options, path);
    // RIFF INFO is all rekordbox reads from WAV: single values, so the joined view.
    logRejected(file.InfoTag()->setProperties(toProperties(id3v23View(model, options), false)), path);
    if (!file.save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripOthers, id3Version(options))) {
        return saveFailed(path);
    }
    return {};
}

Result<void> writeFlac(TagLib::IOStream &stream, const QString &path, const MetadataModel &model)
{
    TagLib::FLAC::File file(&stream, /*readProperties=*/false);
    if (!file.isValid()) {
        return makeError(ErrorCode::TagError, u"%1 is not a FLAC file TagLib can tag"_s.arg(path));
    }
    file.strip(TagLib::FLAC::File::ID3v1 | TagLib::FLAC::File::ID3v2); // some tools prepend ID3 to FLAC
    TagLib::Ogg::XiphComment *comment = file.xiphComment(true);
    comment->removeAllFields();
    logRejected(comment->setProperties(toProperties(model, false)), path);
    file.removePictures();
    if (const std::optional<Picture> cover = model.frontCover()) {
        auto *picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType(detail::toTagLib(cover->mimeType.isEmpty() ? u"image/jpeg"_s : cover->mimeType));
        picture->setDescription(TagLib::String());
        picture->setData(detail::toTagLib(cover->data));
        QBuffer buffer;
        buffer.setData(cover->data);
        const QSize size = QImageReader(&buffer).size(); // header only
        if (size.isValid()) {
            picture->setWidth(size.width());
            picture->setHeight(size.height());
            picture->setColorDepth(24);
        }
        file.addPicture(picture);
    }
    if (!file.save()) {
        return saveFailed(path);
    }
    return {};
}

Result<void> writeMp4(TagLib::IOStream &stream, const QString &path, const MetadataModel &model)
{
    TagLib::MP4::File file(&stream, /*readProperties=*/false);
    if (!file.isValid() || file.tag() == nullptr) {
        return makeError(ErrorCode::TagError, u"%1 is not an MP4 file TagLib can tag"_s.arg(path));
    }
    TagLib::MP4::Tag *tag = file.tag();
    const TagLib::MP4::ItemMap items = tag->itemMap();
    for (const auto &[name, item] : items) {
        tag->removeItem(name);
    }
    const Mp4Fields fields = mp4Fields(model, path);
    logRejected(tag->setProperties(fields.properties), path);
    if (!fields.musicalKey.isEmpty()) {
        // rekordbox reads the key from this atom; TagLib's own mapping writes a lower-case one it ignores.
        tag->setItem(kMp4KeyAtom, TagLib::MP4::Item(detail::toTagLib(fields.musicalKey)));
    }
    if (const std::optional<Picture> cover = model.frontCover()) {
        const TagLib::MP4::CoverArt::Format format =
            cover->mimeType == u"image/png"_s ? TagLib::MP4::CoverArt::PNG : TagLib::MP4::CoverArt::JPEG;
        TagLib::MP4::CoverArtList list;
        list.append(TagLib::MP4::CoverArt(format, detail::toTagLib(cover->data)));
        tag->setItem("covr", TagLib::MP4::Item(list));
    }
    if (!file.save()) {
        return saveFailed(path);
    }
    return {};
}

// What TagLib's MP4 item factory stores for each property and gives back when the item is read.
TagLib::PropertyMap mp4RoundTrip(const Mp4Fields &fields)
{
    const TagLib::MP4::ItemFactory *factory = TagLib::MP4::ItemFactory::instance();
    TagLib::PropertyMap read;
    for (const auto &[key, values] : fields.properties) {
        const auto [name, item] = factory->itemFromProperty(key, values);
        if (name.isEmpty() || !item.isValid()) {
            continue; // setProperties() rejects it as well
        }
        const auto [readKey, readValues] = factory->itemToProperty(name, item);
        if (!readKey.isEmpty()) {
            read.insert(readKey, readValues);
        }
    }
    if (!fields.musicalKey.isEmpty()) {
        read.insert("KEY", detail::toTagLib(fields.musicalKey)); // read back from the freeform atom's name
    }
    return read;
}

} // namespace

MetadataModel id3v23View(const MetadataModel &model, const TagWriteOptions &options)
{
    MetadataModel view;
    const QMap<QString, MetadataModel::Field> &fields = model.fields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        const QString &key = it.key();
        QStringList values = it.value().values;
        if (values.size() > 1 && !keepsSeparateValues(key)) {
            values = QStringList{values.join(isArtistKey(key) ? options.artistSeparator : options.otherSeparator)};
        }
        // v2.3 dates: TagLib writes TYER (+ TDAT for a full date); TORY holds only the year.
        if (key == keys::OriginalDate) {
            values = QStringList{values.first().left(4)};
        } else if (key == keys::Date) {
            const QString &date = values.first();
            const bool fullDate = date.size() >= 10 && date[4] == u'-' && date[7] == u'-';
            values = QStringList{fullDate ? date.left(10) : date.left(4)};
        }
        view.set(key, values, it.value().provenance);
    }
    view.setPictures(model.pictures());
    return view;
}

MetadataModel tagsAsWritten(const MetadataModel &model, audio::Container container, const TagWriteOptions &options)
{
    TagLib::PropertyMap written;
    switch (container) {
    case audio::Container::MpegAudio:
    case audio::Container::Aiff:
    case audio::Container::Wav: {
        TagLib::ID3v2::Tag tag;
        fillId3v2Text(&tag, model, options, QString());
        written = tag.properties();
        break;
    }
    case audio::Container::Flac: {
        TagLib::Ogg::XiphComment comment;
        comment.setProperties(toProperties(model, false));
        written = comment.properties();
        break;
    }
    case audio::Container::Mp4:
        written = mp4RoundTrip(mp4Fields(model, QString()));
        break;
    case audio::Container::Ogg:
    case audio::Container::Other:
    case audio::Container::Unknown: {
        MetadataModel unchanged = model;
        unchanged.clearPictures();
        return unchanged;
    }
    }
    TagReadOptions unbounded;
    unbounded.maxValueChars = std::numeric_limits<qint64>::max();
    unbounded.maxLyricsChars = std::numeric_limits<qint64>::max();
    const MetadataModel read = detail::modelFromProperties(written, unbounded, Provenance::Unknown, QString());
    // Same values as a file would give back, with the provenance the model had for each key.
    MetadataModel predicted;
    const QMap<QString, MetadataModel::Field> &fields = read.fields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        predicted.set(it.key(), it.value().values, model.provenance(it.key()));
    }
    return predicted;
}

Result<void> writeTags(const QString &path, const MetadataModel &model, const TagWriteOptions &options)
{
    if (!QFileInfo(path).isFile()) {
        return makeError(ErrorCode::OpenFailed, u"%1 is not a file"_s.arg(path));
    }
    const audio::Container container = detail::detectContainer(path);
    const QByteArray nativePath = QFile::encodeName(path);
    TagLib::FileStream stream(nativePath.constData(), /*openReadOnly=*/false);
    if (!stream.isOpen() || stream.readOnly()) {
        return makeError(ErrorCode::OpenFailed, u"can't open %1 for writing"_s.arg(path));
    }
    switch (container) {
    case audio::Container::MpegAudio:
        return writeMpeg(stream, path, model, options);
    case audio::Container::Aiff:
        return writeAiff(stream, path, model, options);
    case audio::Container::Wav:
        return writeWav(stream, path, model, options);
    case audio::Container::Flac:
        return writeFlac(stream, path, model);
    case audio::Container::Mp4:
        return writeMp4(stream, path, model);
    case audio::Container::Ogg:
    case audio::Container::Other:
    case audio::Container::Unknown:
        break;
    }
    return makeError(ErrorCode::Unsupported, u"Selecta writes tags only to MP3, AIFF, WAV, FLAC and M4A files"_s);
}

} // namespace selecta::convert
