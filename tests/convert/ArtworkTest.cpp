// SPDX-License-Identifier: GPL-3.0-or-later
// C3: the embedded cover (baseline sRGB JPEG ≤ 800 px, near-square crop, never upscaled, bounded decoding of untrusted
// images) and the folder cover ranking.
#include "ConvertTestUtil.h"
#include "convert/ArtworkFinder.h"
#include "convert/ArtworkProcessor.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QtEndian>

#include <gtest/gtest.h>
#include <zlib.h>

using namespace Qt::StringLiterals;
using namespace selecta::convert;
using namespace selecta::testing;
using selecta::audio::ErrorCode;

namespace {

QImage gradient(int width, int height, QImage::Format format = QImage::Format_RGB32)
{
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.setPixel(x, y, qRgb((x * 255) / std::max(1, width - 1), (y * 255) / std::max(1, height - 1), 96));
        }
    }
    return format == QImage::Format_RGB32 ? image : image.convertToFormat(format);
}

struct WriteOptions
{
    int quality = 90;
    bool progressive = false;
};

QByteArray encode(const QImage &image, const char *format, WriteOptions options = {})
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format);
    writer.setQuality(options.quality);
    writer.setProgressiveScanWrite(options.progressive);
    if (!writer.write(image)) {
        ADD_FAILURE() << "couldn't write the test image: " << writer.errorString().toStdString();
        return {};
    }
    return bytes;
}

QImage decode(const QByteArray &bytes)
{
    return QImage::fromData(bytes);
}

void appendBe32(QByteArray &out, quint32 value)
{
    const quint32 big = qToBigEndian(value);
    out.append(reinterpret_cast<const char *>(&big), sizeof(big));
}

void appendChunk(QByteArray &png, const char *type, const QByteArray &data)
{
    appendBe32(png, static_cast<quint32>(data.size()));
    const QByteArray body = QByteArray(type) + data;
    png.append(body);
    appendBe32(png, static_cast<quint32>(
                        crc32(0, reinterpret_cast<const Bytef *>(body.constData()), static_cast<uInt>(body.size()))));
}

// A PNG whose header claims `width` × `height` pixels, followed by a token IDAT: enough for QImageReader::size().
QByteArray pngClaiming(quint32 width, quint32 height)
{
    QByteArray png = QByteArray::fromHex("89504E470D0A1A0A");
    QByteArray header;
    appendBe32(header, width);
    appendBe32(header, height);
    header.append(QByteArray::fromHex("0802000000")); // 8-bit RGB, no interlace
    appendChunk(png, "IHDR", header);
    appendChunk(png, "IDAT", QByteArray::fromHex("789C030000000001"));
    appendChunk(png, "IEND", {});
    return png;
}

// `jpeg` with an EXIF APP1 segment recording `orientation` (6: rotate a quarter turn clockwise to display). Qt's own
// writer applies a transformation to the pixels instead of recording it, so the segment is built by hand.
QByteArray withExifOrientation(const QByteArray &jpeg, quint16 orientation)
{
    QByteArray tiff = QByteArray::fromHex("4D4D002A00000008"); // big-endian TIFF header, IFD0 at offset 8
    tiff.append(QByteArray::fromHex("0001"));                  // one entry
    tiff.append(QByteArray::fromHex("011200030000000100"));    // Orientation, SHORT, count 1, value…
    tiff.append(static_cast<char>(orientation & 0xFFU));
    tiff.append(QByteArray::fromHex("0000"));     // …padded to 4 bytes
    tiff.append(QByteArray::fromHex("00000000")); // no next IFD
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    QByteArray segment = QByteArray::fromHex("FFE1");
    const auto length = static_cast<quint16>(payload.size() + 2);
    segment.append(static_cast<char>(length >> 8U));
    segment.append(static_cast<char>(length & 0xFFU));
    segment.append(payload);
    return jpeg.left(2) + segment + jpeg.mid(2);
}

// A real JPEG whose frame header is patched to claim `width` × `height` pixels.
QByteArray jpegClaiming(quint16 width, quint16 height)
{
    QByteArray jpeg = encode(gradient(64, 64), "jpeg");
    const qsizetype sof = jpeg.indexOf(QByteArray::fromHex("FFC0"));
    if (sof < 0) {
        ADD_FAILURE() << "no SOF0 in the test JPEG";
        return {};
    }
    // FF C0, length (2), precision (1), height (2), width (2).
    qToBigEndian(height, jpeg.data() + sof + 5);
    qToBigEndian(width, jpeg.data() + sof + 7);
    return jpeg;
}

bool isCompliant(const QByteArray &bytes, int maxSide = 800)
{
    const std::optional<ImageFacts> facts = inspectImage(bytes);
    return facts && facts->format == "jpeg" && facts->baselineJpeg && facts->jpegComponents == 3 &&
           !facts->hasColorProfile && std::max(facts->size.width(), facts->size.height()) <= maxSide;
}

} // namespace

TEST(Artwork, InspectReadsTheHeaderOnly)
{
    const std::optional<ImageFacts> baseline = inspectImage(encode(gradient(320, 200), "jpeg"));
    ASSERT_TRUE(baseline.has_value());
    EXPECT_EQ(baseline->format, "jpeg");
    EXPECT_EQ(baseline->size, QSize(320, 200));
    EXPECT_TRUE(baseline->baselineJpeg);
    EXPECT_EQ(baseline->jpegComponents, 3);
    EXPECT_FALSE(baseline->hasColorProfile);

    const std::optional<ImageFacts> progressive = inspectImage(encode(gradient(64, 64), "jpeg", {.progressive = true}));
    ASSERT_TRUE(progressive.has_value());
    EXPECT_FALSE(progressive->baselineJpeg);

    const std::optional<ImageFacts> grey = inspectImage(encode(gradient(64, 64, QImage::Format_Grayscale8), "jpeg"));
    ASSERT_TRUE(grey.has_value());
    EXPECT_EQ(grey->jpegComponents, 1);

    QImage p3 = gradient(64, 64);
    p3.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    EXPECT_TRUE(inspectImage(encode(p3, "jpeg"))->hasColorProfile);

    const std::optional<ImageFacts> png = inspectImage(encode(gradient(30, 40), "png"));
    ASSERT_TRUE(png.has_value());
    EXPECT_EQ(png->format, "png");
    EXPECT_EQ(png->size, QSize(30, 40));
    EXPECT_FALSE(png->baselineJpeg);

    EXPECT_FALSE(inspectImage(QByteArray("not an image at all")).has_value());
    EXPECT_FALSE(inspectImage(QByteArray()).has_value());
    EXPECT_EQ(inspectImage(pngClaiming(100000, 100000))->size, QSize(100000, 100000)); // no pixels decoded
}

TEST(Artwork, ACompliantJpegIsKeptByteForByte)
{
    const QByteArray jpeg = encode(gradient(600, 600), "jpeg");
    const Result<ProcessedArtwork> result = processArtwork(jpeg);
    ASSERT_TRUE(result.ok()) << result.error().toString().toStdString();
    EXPECT_FALSE(result.value().reencoded);
    EXPECT_EQ(result.value().data, jpeg);
    EXPECT_EQ(result.value().size, QSize(600, 600));
    EXPECT_EQ(result.value().mimeType, u"image/jpeg"_s);
    // A wide JPEG is fine too: only near-square ones are cropped.
    const QByteArray wide = encode(gradient(800, 450), "jpeg");
    EXPECT_FALSE(processArtwork(wide).value().reencoded);
}

TEST(Artwork, LargeImagesAreScaledDownToBaselineJpeg)
{
    const Result<ProcessedArtwork> big = processArtwork(encode(gradient(3000, 3000), "jpeg"));
    ASSERT_TRUE(big.ok());
    EXPECT_TRUE(big.value().reencoded);
    EXPECT_EQ(big.value().size, QSize(800, 800));
    EXPECT_TRUE(isCompliant(big.value().data));
    EXPECT_EQ(decode(big.value().data).size(), QSize(800, 800));

    const Result<ProcessedArtwork> wide = processArtwork(encode(gradient(1600, 800), "png"));
    ASSERT_TRUE(wide.ok());
    EXPECT_EQ(wide.value().size, QSize(800, 400)); // aspect ratio kept, no crop
    EXPECT_TRUE(isCompliant(wide.value().data));

    ArtworkOptions smaller;
    smaller.maxDimension = 600;
    EXPECT_EQ(processArtwork(encode(gradient(1000, 1000), "jpeg"), smaller).value().size, QSize(600, 600));
}

TEST(Artwork, NearSquareImagesAreCentreCropped)
{
    // 1000 × 950 (ratio 1.05): cropped to 950 × 950 around the centre, then scaled to 800.
    QImage image = gradient(1000, 950);
    for (int y = 0; y < 950; ++y) {
        for (int x = 0; x < 25; ++x) {
            image.setPixel(x, y, qRgb(255, 0, 0)); // a red band that the crop must remove
        }
    }
    const Result<ProcessedArtwork> result = processArtwork(encode(image, "jpeg"));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().size, QSize(800, 800));
    const QColor left = decode(result.value().data).pixelColor(2, 400);
    EXPECT_LT(left.red(), 200); // the band is gone

    // Small near-square images are cropped but never scaled up.
    const Result<ProcessedArtwork> small = processArtwork(encode(gradient(500, 480), "jpeg"));
    ASSERT_TRUE(small.ok());
    EXPECT_TRUE(small.value().reencoded);
    EXPECT_EQ(small.value().size, QSize(480, 480));

    ArtworkOptions noCrop;
    noCrop.cropNearSquare = false;
    const Result<ProcessedArtwork> kept = processArtwork(encode(gradient(500, 480), "jpeg"), noCrop);
    EXPECT_FALSE(kept.value().reencoded);
}

TEST(Artwork, NeverUpscaled)
{
    const Result<ProcessedArtwork> tiny = processArtwork(encode(gradient(100, 100), "png"));
    ASSERT_TRUE(tiny.ok());
    EXPECT_TRUE(tiny.value().reencoded); // PNG → JPEG
    EXPECT_EQ(tiny.value().size, QSize(100, 100));
    EXPECT_TRUE(isCompliant(tiny.value().data));
}

TEST(Artwork, NonCompliantJpegsAreReencoded)
{
    // Progressive, CMYK, greyscale, colour-managed, rotated by EXIF, or over 500 KB: each becomes a plain baseline
    // sRGB JPEG.
    QImage p3 = gradient(400, 400);
    p3.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QList<QByteArray> inputs = {
        encode(gradient(400, 400), "jpeg", {.progressive = true}),
        encode(gradient(400, 400).convertToFormat(QImage::Format_CMYK8888), "jpeg"),
        encode(gradient(400, 400, QImage::Format_Grayscale8), "jpeg"),
        encode(p3, "jpeg"),
    };
    for (qsizetype i = 0; i < inputs.size(); ++i) {
        SCOPED_TRACE(i);
        ASSERT_FALSE(inputs[i].isEmpty());
        const Result<ProcessedArtwork> result = processArtwork(inputs[i]);
        ASSERT_TRUE(result.ok()) << result.error().toString().toStdString();
        EXPECT_TRUE(result.value().reencoded);
        EXPECT_TRUE(isCompliant(result.value().data));
        EXPECT_EQ(result.value().size, QSize(400, 400));
    }
    EXPECT_EQ(inspectImage(inputs[1])->jpegComponents, 4); // the input really was CMYK

    // EXIF orientation is applied, so the output needs none: 400 × 200 rotated a quarter turn is 200 × 400.
    const QByteArray rotated = withExifOrientation(encode(gradient(400, 200), "jpeg"), 6);
    QBuffer probe;
    probe.setData(rotated);
    probe.open(QIODevice::ReadOnly);
    ASSERT_EQ(QImageReader(&probe).transformation(), QImageIOHandler::TransformationRotate90); // Qt reads the segment
    const Result<ProcessedArtwork> upright = processArtwork(rotated);
    ASSERT_TRUE(upright.ok());
    EXPECT_TRUE(upright.value().reencoded);
    EXPECT_EQ(upright.value().size, QSize(200, 400));
    EXPECT_EQ(QImage::fromData(upright.value().data).size(), QSize(200, 400));

    // Over the 500 KB keep limit (noise compresses badly).
    QImage noise(790, 790, QImage::Format_RGB32);
    quint32 state = 12345;
    for (int y = 0; y < noise.height(); ++y) {
        for (int x = 0; x < noise.width(); ++x) {
            state = (state * 1664525U) + 1013904223U;
            noise.setPixel(x, y, state | 0xFF000000U);
        }
    }
    const QByteArray heavy = encode(noise, "jpeg", {.quality = 100});
    ASSERT_GT(heavy.size(), 500 * 1024);
    const Result<ProcessedArtwork> lighter = processArtwork(heavy);
    ASSERT_TRUE(lighter.ok());
    EXPECT_TRUE(lighter.value().reencoded);
    EXPECT_LT(lighter.value().data.size(), heavy.size());
}

TEST(Artwork, TransparencyBecomesWhite)
{
    QImage logo(200, 200, QImage::Format_ARGB32);
    logo.fill(Qt::transparent);
    QPainter painter(&logo);
    painter.fillRect(75, 75, 50, 50, Qt::red);
    painter.end();
    const Result<ProcessedArtwork> result = processArtwork(encode(logo, "png"));
    ASSERT_TRUE(result.ok());
    const QImage out = decode(result.value().data);
    EXPECT_GT(qGray(out.pixel(5, 5)), 240);    // white, not black
    EXPECT_GT(qRed(out.pixel(100, 100)), 200); // still red in the middle
    EXPECT_LT(qGreen(out.pixel(100, 100)), 60);
}

TEST(Artwork, UntrustedInputIsBoundedBeforeDecoding)
{
    // Absurd dimensions in the header are refused before a single pixel is allocated.
    Result<ProcessedArtwork> result = processArtwork(pngClaiming(100000, 100000));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::TooLarge);
    result = processArtwork(pngClaiming(9000, 9000)); // 81 megapixels, sides within the limit
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::TooLarge);
    result = processArtwork(jpegClaiming(60000, 60000));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::TooLarge);

    ArtworkOptions tight;
    tight.maxInputBytes = 1000;
    result = processArtwork(encode(gradient(300, 300), "jpeg"), tight);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::TooLarge);

    // Truncated and corrupt data, garbage, nothing.
    const QByteArray jpeg = encode(gradient(300, 300), "jpeg");
    result = processArtwork(jpeg.left(jpeg.size() / 3));
    EXPECT_FALSE(result.ok());
    QByteArray scrambled = jpeg;
    for (qsizetype i = scrambled.size() / 2; i < scrambled.size() - 2; i += 7) {
        scrambled[i] = static_cast<char>(0xFF);
    }
    result = processArtwork(scrambled); // decodes to something or fails: never crashes, never hangs
    if (result.ok()) {
        EXPECT_TRUE(isCompliant(result.value().data));
    }
    result = processArtwork(QByteArray("definitely not an image"));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Unsupported);
    result = processArtwork(QByteArray());
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
    result = processArtwork(pngClaiming(64, 64)); // a valid header over garbage pixels
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, ErrorCode::Corrupt);
}

TEST(Artwork, ToPictureIsAFrontCover)
{
    const Result<ProcessedArtwork> result = processArtwork(makeImage(300, 300, "jpeg"));
    ASSERT_TRUE(result.ok());
    const Picture picture = result.value().toPicture(Provenance::Folder);
    EXPECT_TRUE(picture.isFrontCover());
    EXPECT_EQ(picture.data, result.value().data);
    EXPECT_EQ(picture.mimeType, u"image/jpeg"_s);
    EXPECT_TRUE(picture.description.isEmpty());
    EXPECT_EQ(picture.provenance, Provenance::Folder);
}

// ---------------------------------------------------------------------------------------------------------------
// Folder covers (conversion §5.1)

namespace {

std::optional<int> pick(const QList<CoverCandidate> &candidates, bool *guessed = nullptr)
{
    const std::optional<CoverChoice> choice = chooseFolderCover(candidates);
    if (!choice) {
        return std::nullopt;
    }
    if (guessed != nullptr) {
        *guessed = choice->guessed;
    }
    return choice->index;
}

CoverCandidate image(const QString &path, QSize size = {})
{
    return {path, size};
}

} // namespace

TEST(FolderCover, NamesDecide)
{
    bool guessed = true;
    EXPECT_EQ(pick({image(u"cover.jpg"_s)}, &guessed), 0);
    EXPECT_FALSE(guessed);
    EXPECT_EQ(pick({image(u"back.jpg"_s), image(u"Folder.JPG"_s)}), 1);
    EXPECT_EQ(pick({image(u"front.png"_s), image(u"cover.jpeg"_s)}), 1);       // cover, folder, front: in that order
    EXPECT_EQ(pick({image(u"Scans/cover.jpg"_s), image(u"folder.jpg"_s)}), 1); // the album folder first
    EXPECT_EQ(pick({image(u"Artist - Album - Back.jpg"_s), image(u"Artist - Album - Front.jpg"_s)}), 1);
    EXPECT_EQ(pick({image(u"AlbumArtSmall.jpg"_s), image(u"AlbumArt_{7B3C}_Large.jpg"_s)}), 1); // whole words only
    EXPECT_EQ(pick({image(u"covers/Front.jpg"_s)}, &guessed), 0); // subfolder names ignore case
    EXPECT_FALSE(guessed);
    EXPECT_EQ(pick({image(u"Artwork\\cover.png"_s)}), 0);                   // Windows separators
    EXPECT_EQ(pick({image(u"cover (2).jpg"_s), image(u"cover.jpg"_s)}), 1); // exact before a word match
}

TEST(FolderCover, ExclusionsAndElimination)
{
    EXPECT_FALSE(pick({image(u"cd.jpg"_s), image(u"inlay.jpg"_s), image(u"tray.png"_s)}).has_value());
    EXPECT_FALSE(pick({image(u"front cover back.jpg"_s)}).has_value()); // "back" vetoes it
    bool guessed = false;
    EXPECT_EQ(pick({image(u"scan001.jpg"_s), image(u"disc.jpg"_s)}, &guessed), 0); // the only plausible image
    EXPECT_TRUE(guessed);
    guessed = false;
    EXPECT_EQ(pick({image(u"a.jpg"_s, {500, 500}), image(u"b.jpg"_s, {1000, 1000}), image(u"c.jpg"_s, {2000, 800})},
                   &guessed),
              1); // the largest roughly square one
    EXPECT_TRUE(guessed);
    EXPECT_FALSE(pick({image(u"a.jpg"_s), image(u"b.jpg"_s)}).has_value()); // several, no sizes: no guess
}

TEST(FolderCover, OnlyImagesInTheAlbumFolderOrItsArtFolders)
{
    EXPECT_FALSE(pick({image(u"Other/cover.jpg"_s)}).has_value());
    EXPECT_FALSE(pick({image(u"Scans/Deep/cover.jpg"_s)}).has_value());
    EXPECT_FALSE(pick({image(u"../cover.jpg"_s)}).has_value());
    EXPECT_FALSE(pick({image(u"/etc/cover.jpg"_s)}).has_value());
    EXPECT_FALSE(pick({image(u"cover.txt"_s), image(u"notes.nfo"_s), image(u".jpg"_s)}).has_value());
    EXPECT_FALSE(pick({}).has_value());
    EXPECT_EQ(pick({image(u"./Cover.JPEG"_s)}), 0);
}
