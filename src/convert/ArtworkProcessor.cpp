// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/ArtworkProcessor.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

constexpr double kNearSquareLow = 0.9;
constexpr double kNearSquareHigh = 1.1;
constexpr int kMaxJpegSegments = 4096; // a real JPEG has a few dozen markers before its scan

struct JpegFacts
{
    bool sawFrame = false;
    bool baseline = false;
    int components = 0;
    bool iccProfile = false;
    bool truncated = false; // the scan never reaches an end-of-image marker
};

bool isStartOfFrame(quint8 marker)
{
    // SOF0…SOF15 except DHT (C4), JPG (C8) and DAC (CC).
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

// Walks the JPEG marker segments up to the first scan (bounded by the data and a segment count): the frame type, the
// component count and an ICC profile, none of which QImageReader reports without decoding.
JpegFacts scanJpeg(QByteArrayView data)
{
    JpegFacts facts;
    const auto byteAt = [&data](qsizetype i) { return static_cast<quint8>(data[i]); };
    if (data.size() < 4 || byteAt(0) != 0xFF || byteAt(1) != 0xD8) {
        return facts;
    }
    qsizetype pos = 2;
    for (int segments = 0; segments < kMaxJpegSegments && pos + 4 <= data.size(); ++segments) {
        if (byteAt(pos) != 0xFF) {
            break; // not a marker where one must be
        }
        while (pos < data.size() && byteAt(pos) == 0xFF) {
            ++pos; // fill bytes
        }
        if (pos + 3 > data.size()) {
            break;
        }
        const quint8 marker = byteAt(pos);
        ++pos;
        if (marker == 0xDA) {
            // The entropy-coded scan begins; a complete file ends it with EOI. (libjpeg decodes a cut file without
            // complaint, filling the missing part with grey.)
            facts.truncated = data.lastIndexOf(QByteArrayView("\xFF\xD9", 2)) < pos;
            break;
        }
        if (marker == 0xD9) {
            break;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue; // standalone markers carry no length
        }
        const auto length = static_cast<qsizetype>((quint32{byteAt(pos)} << 8U) | byteAt(pos + 1));
        if (length < 2 || pos + length > data.size()) {
            break;
        }
        const QByteArrayView payload = data.sliced(pos + 2, length - 2);
        if (isStartOfFrame(marker) && payload.size() >= 6 && !facts.sawFrame) {
            facts.sawFrame = true;
            facts.baseline = marker == 0xC0 && static_cast<quint8>(payload[0]) == 8;
            facts.components = static_cast<quint8>(payload[5]);
        } else if (marker == 0xE2 && payload.startsWith(QByteArrayView("ICC_PROFILE\0", 12))) {
            facts.iccProfile = true;
        }
        pos += length;
    }
    return facts;
}

// A QBuffer reading `data` in place (no copy); the view must outlive the buffer.
class ViewBuffer
{
public:
    explicit ViewBuffer(QByteArrayView data)
        : m_bytes(QByteArray::fromRawData(data.data(), data.size()))
    {
        m_buffer.setBuffer(&m_bytes);
        m_buffer.open(QIODevice::ReadOnly);
    }
    QBuffer *device() { return &m_buffer; }

private:
    QByteArray m_bytes;
    QBuffer m_buffer;
};

bool isNearSquare(QSize size)
{
    if (size.width() == size.height() || size.height() <= 0) {
        return false;
    }
    const double ratio = static_cast<double>(size.width()) / size.height();
    return ratio >= kNearSquareLow && ratio <= kNearSquareHigh;
}

Result<void> checkBounds(QSize size, const ArtworkOptions &options)
{
    if (!size.isValid() || size.isEmpty()) {
        return makeError(ErrorCode::Corrupt, u"the image header gives no size"_s);
    }
    if (size.width() > options.maxInputDimension || size.height() > options.maxInputDimension ||
        qint64{size.width()} * size.height() > options.maxInputPixels) {
        return makeError(ErrorCode::TooLarge,
                         u"a %1×%2 image is too large to embed"_s.arg(size.width()).arg(size.height()));
    }
    return {};
}

// RGB without alpha, in sRGB: transparency over white (a cover's natural background), colour spaces converted, CMYK
// turned into RGB.
QImage toOpaqueSrgb(QImage image)
{
    const QColorSpace srgb(QColorSpace::SRgb);
    const QColorSpace space = image.colorSpace();
    if (space.isValid() && space != srgb) {
        if (space.colorModel() == QColorSpace::ColorModel::Rgb) {
            image.convertToColorSpace(srgb);
        } else if (QImage converted = image.convertedToColorSpace(srgb, QImage::Format_RGB32); !converted.isNull()) {
            image = std::move(converted); // CMYK or grey with a profile: the conversion changes the pixel format
        }
    }
    if (image.hasAlphaChannel()) {
        QImage opaque(image.size(), QImage::Format_RGB32);
        opaque.fill(Qt::white);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, image);
        painter.end();
        image = std::move(opaque);
    } else {
        image = image.convertToFormat(QImage::Format_RGB32);
    }
    image.setColorSpace(QColorSpace()); // no ICC profile in the output: players assume sRGB
    return image;
}

} // namespace

std::optional<ImageFacts> inspectImage(QByteArrayView image)
{
    if (image.isEmpty()) {
        return std::nullopt;
    }
    ViewBuffer buffer(image);
    const QImageReader reader(buffer.device());
    ImageFacts facts;
    facts.format = reader.format();
    if (facts.format.isEmpty()) {
        return std::nullopt;
    }
    facts.size = reader.size();
    if (facts.format == "jpeg") {
        const JpegFacts jpeg = scanJpeg(image);
        facts.baselineJpeg = jpeg.baseline;
        facts.jpegComponents = jpeg.components;
        facts.hasColorProfile = jpeg.iccProfile;
    }
    return facts;
}

Picture ProcessedArtwork::toPicture(Provenance provenance) const
{
    Picture picture;
    picture.data = data;
    picture.mimeType = mimeType;
    picture.type = Picture::kTypeFrontCover;
    picture.provenance = provenance;
    return picture;
}

Result<ProcessedArtwork> processArtwork(QByteArrayView image, const ArtworkOptions &options)
{
    if (image.isEmpty()) {
        return makeError(ErrorCode::Corrupt, u"the image is empty"_s);
    }
    if (image.size() > options.maxInputBytes) {
        return makeError(ErrorCode::TooLarge,
                         u"a %1-byte image is too large to embed"_s.arg(static_cast<qint64>(image.size())));
    }
    const std::optional<ImageFacts> facts = inspectImage(image);
    if (!facts) {
        return makeError(ErrorCode::Unsupported, u"not an image format Selecta can read"_s);
    }
    if (const Result<void> bounded = checkBounds(facts->size, options); !bounded.ok()) {
        return bounded.error();
    }
    if (facts->format == "jpeg" && scanJpeg(image).truncated) {
        return makeError(ErrorCode::Corrupt, u"the JPEG is cut short"_s);
    }

    // Only now are pixels decoded: the encoded size and the header dimensions have passed the bounds.
    ViewBuffer buffer(image);
    QImageReader reader(buffer.device());
    reader.setAutoTransform(true); // EXIF orientation applied, so the output needs none
    const bool rotated = reader.transformation() != QImageIOHandler::TransformationNone;
    QImage decoded = reader.read();
    if (decoded.isNull()) {
        return makeError(ErrorCode::Corrupt, u"the image doesn't decode: %1"_s.arg(reader.errorString()));
    }
    // The header may undersell what the decoder produced (some formats store several sizes).
    if (const Result<void> bounded = checkBounds(decoded.size(), options); !bounded.ok()) {
        return bounded.error();
    }

    const QSize size = decoded.size();
    const bool crop = options.cropNearSquare && isNearSquare(size);
    const bool compliant = facts->format == "jpeg" && facts->baselineJpeg && facts->jpegComponents == 3 &&
                           !facts->hasColorProfile && !rotated && image.size() <= options.maxKeepBytes &&
                           std::max(size.width(), size.height()) <= options.maxDimension && !crop;
    if (compliant) {
        ProcessedArtwork kept;
        kept.data = image.toByteArray();
        kept.size = size;
        return kept; // byte for byte: no generation loss on artwork that already fits
    }

    QImage cover = toOpaqueSrgb(std::move(decoded));
    if (crop) {
        const int side = std::min(size.width(), size.height());
        cover = cover.copy((size.width() - side) / 2, (size.height() - side) / 2, side, side);
    }
    if (std::max(cover.width(), cover.height()) > options.maxDimension) {
        cover = cover.scaled(options.maxDimension, options.maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QBuffer out;
    out.open(QIODevice::WriteOnly);
    QImageWriter writer(&out, "jpeg");
    writer.setQuality(options.jpegQuality);
    writer.setProgressiveScanWrite(false); // baseline: what every player decodes
    if (!writer.write(cover)) {
        return makeError(ErrorCode::EncoderError, u"the cover couldn't be encoded: %1"_s.arg(writer.errorString()));
    }
    ProcessedArtwork processed;
    processed.data = out.data();
    processed.size = cover.size();
    processed.reencoded = true;
    return processed;
}

} // namespace selecta::convert
