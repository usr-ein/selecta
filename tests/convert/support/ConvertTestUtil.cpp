// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConvertTestUtil.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainter>

using namespace Qt::StringLiterals;

namespace selecta::testing {

using convert::MetadataModel;
using convert::Picture;
using convert::Provenance;
namespace keys = convert::keys;

QString writeFile(const QString &directory, const QString &name, const QByteArray &bytes)
{
    QString path = QDir(directory).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return {};
    }
    return path;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray makeImage(int width, int height, const char *format, int quality)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, qRgb((x * 255) / std::max(1, width - 1), (y * 255) / std::max(1, height - 1), 96));
        }
    }
    // A mark in the top-left corner shows whether a crop kept the centre.
    for (int y = 0; y < std::min(height, 8); ++y) {
        for (int x = 0; x < std::min(width, 8); ++x) {
            image.setPixel(x, y, qRgb(255, 255, 255));
        }
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    writer.setQuality(quality);
    if (!writer.write(image)) {
        return {};
    }
    return bytes;
}

MetadataModel richModel()
{
    MetadataModel model;
    const Provenance p = Provenance::Release;
    model.set(keys::Title, u"Night Shift (Ünïcödé Mix) — 東京"_s, p);
    model.set(keys::Artist, QStringList{u"Kerri Vale"_s, u"AC/DC Tribute Band"_s}, p);
    model.set(keys::Album, u"Night Shift EP"_s, p);
    model.set(keys::AlbumArtist, u"Kerri Vale"_s, p);
    model.set(keys::TrackNumber, u"3/12"_s, p);
    model.set(keys::DiscNumber, u"1/2"_s, p);
    model.set(keys::Date, u"2024-03-15"_s, p);
    model.set(keys::Genre, u"Deep House"_s, p);
    model.set(keys::Composer, u"K. Vale"_s, p);
    model.set(keys::Bpm, u"127.98"_s, p);
    model.set(keys::InitialKey, u"8A"_s, p);
    model.set(keys::Label, u"Fictional Records"_s, p);
    model.set(keys::Isrc, u"XX0002400001"_s, p);
    model.set(keys::CatalogNumber, u"FIC-001"_s, p);
    model.set(keys::Comment, u"Synthetic test file"_s, p);
    model.set(keys::Remixer, u"Some Remixer"_s, p);
    model.set(keys::Mood, u"Driving"_s, p);
    model.set(keys::DiscSubtitle, u"Club Mixes"_s, p);
    model.set(keys::ReleaseDate, u"2024-03-01"_s, p);
    model.set(keys::SourceUrl, u"https://example.invalid/track/123"_s, p);
    // TSOP in ID3v2.3 survives only while TagLib keeps its iTunes workarounds (NO_ITUNES_HACKS off, the default).
    model.set(u"ARTISTSORT", u"Vale, Kerri"_s, p);
    model.set(u"MYCUSTOMFIELD", u"custom value"_s, p);
    Picture cover;
    cover.data = makeImage(300, 300, "jpeg");
    cover.mimeType = u"image/jpeg"_s;
    cover.provenance = p;
    model.setFrontCover(cover);
    return model;
}

} // namespace selecta::testing
