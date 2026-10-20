// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/TagWriter.h"

#include "convert/detail/TagLibUtil.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <array>
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
    for (const auto &[key, values] : rejected) {
        qCDebug(lcConvert) << "TagLib can't store" << detail::fromTagLib(key) << "in" << path;
    }
}

void fillId3v2(TagLib::ID3v2::Tag *tag, const MetadataModel &model, const TagWriteOptions &options, const QString &path)
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

Result<void> saveFailed(const QString &path)
{
    return makeError(ErrorCode::TagError,
                     u"TagLib couldn't save the tags of %1 (disk full or not writable?)"_s.arg(path));
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
    const TagLib::ID3v2::Version version =
        options.id3Version == Id3Version::V23 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4;
    if (!file.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers, version, TagLib::File::DoNotDuplicate)) {
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
    if (!file.save(options.id3Version == Id3Version::V23 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4)) {
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
    const TagLib::ID3v2::Version version =
        options.id3Version == Id3Version::V23 ? TagLib::ID3v2::v3 : TagLib::ID3v2::v4;
    if (!file.save(TagLib::RIFF::WAV::File::AllTags, TagLib::File::StripOthers, version)) {
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
    MetadataModel rest = model;
    const QStringList key = rest.values(keys::InitialKey);
    rest.remove(keys::InitialKey);
    logRejected(tag->setProperties(toProperties(rest, false)), path);
    if (!key.isEmpty()) {
        // rekordbox reads the key from this atom; TagLib's own mapping writes a lower-case one it ignores.
        tag->setItem(kMp4KeyAtom, TagLib::MP4::Item(detail::toTagLib(key)));
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
