// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QSize>
#include <QString>

#include <optional>

namespace selecta::convert {

struct ArtworkOptions
{
    int maxDimension = 800; // longest side of the embedded cover (CDJ limit, D-2)
    int jpegQuality = 90;
    qint64 maxKeepBytes = qint64{500} * 1024; // a compliant JPEG above this is re-encoded anyway
    bool cropNearSquare = true;               // centre-crop to square when the aspect ratio is within 0.9–1.1
    // Bounds for untrusted images, checked before any pixel is decoded: the encoded size first, then the dimensions
    // from the image header (QImageReader::size()). QImageReader's process-wide allocation limit is the app's to set
    // (in main()); this code never changes it.
    qint64 maxInputBytes = qint64{20} * 1024 * 1024;
    int maxInputDimension = 10000; // pixels on either side
    qint64 maxInputPixels = qint64{50} * 1000 * 1000;
};

// Facts read from the image header without decoding the pixels.
struct ImageFacts
{
    QByteArray format; // "jpeg", "png", … as QImageReader names it
    QSize size;
    bool baselineJpeg = false;    // JPEG with a baseline (SOF0) frame
    int jpegComponents = 0;       // 3 for YCbCr/RGB, 4 for CMYK/YCCK, 1 for grey
    bool hasColorProfile = false; // an embedded ICC profile
};

std::optional<ImageFacts> inspectImage(QByteArrayView image);

struct ProcessedArtwork
{
    QByteArray data; // JPEG bytes
    QString mimeType = QStringLiteral("image/jpeg");
    QSize size;
    bool reencoded = false; // false: the input was already compliant and is returned byte for byte

    Picture toPicture(Provenance provenance) const;
};

// The embedded cover (conversion §5.3, C3): a baseline sRGB JPEG at quality 90, at most 800 px on its longest side.
// An input that is already a baseline 3-component JPEG without a colour profile, within the size limits and not near-
// square-but-not-square (when cropping), is returned unchanged; anything else (PNG, progressive or CMYK JPEG, ICC
// profiles, oversize) is decoded with QImageReader once its encoded size and header dimensions have passed the bounds,
// converted to sRGB, centre-cropped to square when its ratio is within 0.9–1.1, scaled down (never up) and re-encoded.
// Metadata (EXIF, ICC) is dropped.
//
// Threading: reentrant (QImage is); call from a worker for large images. Failure codes: TooLarge (bytes, dimensions,
// pixel count), Unsupported (a format this build can't decode), Corrupt (unreadable header or pixels), EncoderError.
Result<ProcessedArtwork> processArtwork(QByteArrayView image, const ArtworkOptions &options = {});

} // namespace selecta::convert
