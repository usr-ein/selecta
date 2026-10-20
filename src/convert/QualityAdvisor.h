// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/QualityCheck.h"
#include "audio/StreamInfo.h"
#include "convert/EncodeSettings.h"
#include "convert/MetadataModel.h"

#include <QList>
#include <QString>

#include <optional>

namespace selecta::convert {

// What a source really is (conversion §6.1): stream facts from the Decoder plus, when run, the quality checks.
struct SourceQuality
{
    audio::Container container = audio::Container::Unknown;
    audio::Codec codec = audio::Codec::Unknown;
    QString codecName; // FFmpeg's codec name ("wmav2", "ape", …), for formats the Codec enum doesn't name
    bool lossless = false;
    int sampleRate = 0;
    int bitDepth = 0;          // declared, lossless only
    int effectiveBitDepth = 0; // 16 for padded 24-bit (exact check), else bitDepth
    int channels = 0;
    int bitrateKbps = 0; // average
    bool constantBitrate = false;
    QString encoder;                            // e.g. "LAME3.100"
    std::optional<audio::QualityReport> checks; // spectral and bit-depth checks, when analyzeQuality() ran
};

// Builds a SourceQuality from what a Decoder found and, optionally, analyzeQuality()'s report.
SourceQuality makeSourceQuality(const audio::StreamInfo &info, const std::optional<audio::QualityReport> &checks = {});

// Opens the file, reads its stream facts and, with `runChecks`, decodes it for the quality checks. Blocks: call from
// a worker. Failure codes: those of Decoder::open()/read() and analyzeQuality().
Result<SourceQuality> probeSourceQuality(const QString &path, bool runChecks,
                                         const audio::CancellationFlag *cancel = nullptr);

// A file's verdict for the inspector (Get tracks, after arrival): is it plausibly what it claims to be, a 320 kbps MP3
// that really is one, a lossless file that isn't a transcode? From the stream facts and the quality checks.
struct QualityVerdict
{
    enum class Kind {
        Genuine,   // the checks ran and found nothing wrong at the claimed quality
        Suspect,   // a check found that the file is likely not what it claims (the reason says why)
        Unchecked, // the checks didn't run, or couldn't judge (too short, silent, no measurable spectrum)
    };
    Kind kind = Kind::Unchecked;
    QString claimed; // what the file claims: "MP3 320 kbps CBR", "FLAC 24-bit / 96 kHz", "AAC ≈ 256 kbps"
    QString reason;  // one or two sentences, "likely" wording: "Likely upconverted from ≈ 128 kbps (…)."
};

// Pure function.
QualityVerdict judgeQuality(const SourceQuality &source);
// "MP3 320 kbps CBR", "MP3 VBR ≈ 245 kbps", "FLAC 16-bit / 44.1 kHz", "Ogg Vorbis ≈ 192 kbps"…
QString describeSource(const SourceQuality &source);

// The rows of the warning matrix (conversion §6.2).
enum class SourceClass {
    LosslessGenuine, // 16-bit, 44.1–48 kHz
    LosslessHiRes,   // 24-bit and/or above 48 kHz
    LosslessSuspect, // likely from lossy, padded 24-bit or upsampled
    Mp3High,         // MP3 320 CBR or V0-class VBR, not suspect
    Mp3Lower,        // other MP3s, and "320" files that are likely upconverted
    Aac,             // AAC/M4A (rekordbox plays it)
    LossyUnplayable, // Ogg Vorbis, Opus, WMA… (rekordbox can't play them)
    OtherLossless,   // APE, WavPack, DSD…
};

enum class Action {
    Convert,  // proceed (show the info notices)
    KeepAsIs, // don't re-encode: the original audio is filed with fresh tags (CONV-2); see the badge
    Ask,      // a conversion without quality gain: needs explicit confirmation (CONV-6)
    Refuse,   // not offered at all (⛔), e.g. lossy to higher-rate lossless
};

// Short ids follow conversion §6.3.
enum class Notice {
    LossyToLossless,   // W-LOSSY-TO-LOSSLESS
    LossyUp,           // W-LOSSY-UP
    Generation,        // W-GENERATION
    Upsample,          // W-UPSAMPLE
    UpDepth,           // W-UPDEPTH
    Suspect,           // W-SUSPECT
    NotPlayable,       // W-NOT-PLAYABLE
    HardwareRate,      // W-HW-RATE
    WavTags,           // W-WAV-TAGS
    Resample,          // I-RESAMPLE
    Dither,            // I-DITHER
    LosslessReduction, // info: reducing padded 24-bit to 16-bit loses nothing
};

// Why a file kept as it is (KeepAsIs) isn't what the target asks for.
enum class Badge {
    None,
    BelowTarget,   // a lossy file below the target's bitrate
    FormatDiffers, // another format (AAC, or MP3 for a lossless target), or an MP3 at another rate than the target's
};

enum class AdviceContext {
    Filing, // the arrival pipeline (D29): lossy sources it can't improve are kept as they are
    Manual, // the converter view (CONV-3): everything is offered, behind confirmations
};

// The players lossless outputs must play on (D-3).
struct HardwareProfile
{
    QString name = QStringLiteral("CDJ-3000 / CDJ-2000NXS2");
    int maxSampleRate = 96000;
    int maxBitDepth = 24;
};

struct Advice
{
    Action action = Action::Convert;
    SourceClass sourceClass = SourceClass::LosslessGenuine;
    QList<Notice> notices; // warnings first, then information
    Badge badge = Badge::None;
    // The file Selecta writes: what the target makes of the source, or, for KeepAsIs, the source itself.
    int outputSampleRate = 0;
    int outputBitDepth = 0;  // 0 for MP3 (and a kept lossy file)
    double sizeFactor = 0.0; // estimated output size ÷ source size; 0 when unknown; 1 for KeepAsIs

    bool needsConfirmation() const { return action == Action::Ask; }
    bool hasNotice(Notice notice) const { return notices.contains(notice); }
};

// The quality advisor (CONV-2, CONV-6): the warning matrix of conversion §6.2. Never upsamples automatically: a rate
// above the source's, chosen or forced (MP3 from below 44.1 kHz, other than 32 kHz), needs confirmation; lossy → lossy
// and lossy → lossless only with explicit confirmation; a lossy source below the target is kept with a "below target"
// badge, an MP3 is kept as it is only at the target's rate. Pure function.
Advice advise(const SourceQuality &source, const EncodeSettings &target, AdviceContext context,
              const HardwareProfile &hardware = {});

struct NoticeText
{
    QString shortText; // e.g. "No quality gain — about 4.4× larger"
    QString detail;
};

// UI copy for a notice (conversion §6.3), with the numbers filled in.
NoticeText describeNotice(Notice notice, const SourceQuality &source, const EncodeSettings &target,
                          const Advice &advice, const HardwareProfile &hardware = {});

} // namespace selecta::convert
