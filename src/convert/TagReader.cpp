// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/TagReader.h"

#include "convert/detail/TagLibUtil.h"

#include <QFile>
#include <QFileInfo>

#include <array>
#include <taglib/fileref.h>
#include <taglib/tfilestream.h>
#include <taglib/tpicturetype.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

constexpr int kMaxFields = 512;

struct Alias
{
    QStringView from;
    QStringView to;
};

// Conversion §9.1, TagReader aliases (plus a few equivalents from Picard and mediafile).
constexpr std::array<Alias, 11> kAliases = {{
    {u"ALBUM ARTIST", keys::AlbumArtist},
    {u"ALBUM_ARTIST", keys::AlbumArtist},
    {u"KEY", keys::InitialKey},
    {u"ORGANIZATION", keys::Label},
    {u"PUBLISHER", keys::Label},
    {u"DESCRIPTION", keys::Comment},
    {u"UNSYNCEDLYRICS", keys::Lyrics},
    {u"ORIGINALYEAR", keys::OriginalDate},
    {u"CATALOGID", keys::CatalogNumber},
    {u"DISCOGS_CATALOG", keys::CatalogNumber},
    {u"YEAR", keys::Date},
}};

// "n" plus a separate total becomes "n/N" (conversion §3: ID3 TRCK/TPOS carry both).
void foldTotal(MetadataModel &model, QStringView numberKey, std::initializer_list<QStringView> totalKeys)
{
    QString total;
    for (const QStringView key : totalKeys) {
        if (total.isEmpty()) {
            total = model.value(key);
        }
        model.remove(key);
    }
    const QString number = model.value(numberKey);
    if (total.isEmpty() || number.isEmpty() || number.contains(u'/')) {
        return;
    }
    model.set(numberKey, number + u'/' + total, model.provenance(numberKey));
}

QString bounded(const QString &value, qint64 limit)
{
    return value.size() > limit ? value.left(static_cast<qsizetype>(limit)) : value;
}

} // namespace

QString canonicalTagKey(QStringView rawKey)
{
    QString key = rawKey.toString().trimmed().toUpper();
    for (const Alias &alias : kAliases) {
        if (key == alias.from) {
            return alias.to.toString();
        }
    }
    return key;
}

Result<MetadataModel> readTags(const QString &path, const TagReadOptions &options)
{
    if (!QFileInfo(path).isFile()) {
        return makeError(ErrorCode::OpenFailed, u"%1 is not a readable file"_s.arg(path));
    }
    const QByteArray nativePath = QFile::encodeName(path);
    TagLib::FileStream stream(nativePath.constData(), /*openReadOnly=*/true);
    if (!stream.isOpen()) {
        return makeError(ErrorCode::OpenFailed, u"can't open %1"_s.arg(path));
    }
    const TagLib::FileRef file(&stream, /*readAudioProperties=*/false);
    if (file.isNull()) {
        return makeError(ErrorCode::Unsupported, u"no tags TagLib can read in %1"_s.arg(path));
    }

    MetadataModel model;
    const TagLib::PropertyMap properties = file.properties();
    int fields = 0;
    // Canonical keys first; an alias only fills a field the file doesn't have under its canonical name (a file with
    // both DATE and YEAR keeps DATE).
    for (const bool aliasPass : {false, true}) {
        for (const auto &[rawKey, rawValues] : properties) {
            const QString raw = detail::fromTagLib(rawKey).trimmed().toUpper();
            const QString key = canonicalTagKey(raw);
            const bool alias = key != raw;
            if (alias != aliasPass || (alias && model.contains(key))) {
                continue;
            }
            if (++fields > kMaxFields) {
                qCDebug(lcConvert) << "ignoring tags beyond" << kMaxFields << "fields in" << path;
                break;
            }
            const qint64 limit = key == keys::Lyrics ? options.maxLyricsChars : options.maxValueChars;
            QStringList values;
            for (const TagLib::String &value : rawValues) {
                values.append(bounded(detail::fromTagLib(value), limit));
            }
            model.set(key, values, Provenance::Embedded);
        }
    }
    foldTotal(model, keys::TrackNumber, {u"TRACKTOTAL", u"TOTALTRACKS"});
    foldTotal(model, keys::DiscNumber, {u"DISCTOTAL", u"TOTALDISCS"});

    QList<Picture> pictures;
    const TagLib::List<TagLib::VariantMap> complex = file.complexProperties("PICTURE");
    for (const TagLib::VariantMap &entry : complex) {
        if (pictures.size() >= options.maxPictures) {
            qCDebug(lcConvert) << "ignoring pictures beyond" << options.maxPictures << "in" << path;
            break;
        }
        const TagLib::ByteVector data = entry.value("data").toByteVector();
        if (data.isEmpty() || std::cmp_greater(data.size(), options.maxPictureBytes)) {
            qCDebug(lcConvert) << "skipping a picture of" << data.size() << "bytes in" << path;
            continue;
        }
        Picture picture;
        picture.data = detail::fromTagLib(data);
        picture.mimeType = detail::fromTagLib(entry.value("mimeType").toString());
        picture.description = bounded(detail::fromTagLib(entry.value("description").toString()), 1024);
        picture.type = TagLib::Utils::pictureTypeFromString(entry.value("pictureType").toString());
        picture.provenance = Provenance::Embedded;
        pictures.append(picture);
    }
    model.setPictures(pictures);
    return model;
}

} // namespace selecta::convert
