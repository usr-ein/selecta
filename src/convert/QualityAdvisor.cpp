// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/QualityAdvisor.h"

#include "audio/ByteSource.h"
#include "audio/Decoder.h"

#include <QStringList>

#include <algorithm>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

using audio::Codec;
using audio::Container;
using audio::QualityFinding;

constexpr int kV0ClassKbps = 220;  // a VBR MP3 averaging this or more is V0/V1 class
constexpr int kTopLossyKbps = 256; // lossy files claiming this or more should keep the band above ~18 kHz
constexpr int kLossyCutoffLimitHz = 17800;
constexpr double kFlacSizeRatio = 0.6; // FLAC and ALAC typically take 50–70 % of PCM (conversion §6.2)
// LAME's average bitrates for -V0 … -V9 (LAME's documentation), for size estimates and "below target" decisions.
constexpr std::array<int, 10> kVbrAverageKbps = {245, 225, 190, 175, 165, 130, 115, 100, 85, 65};

bool has(const SourceQuality &source, QualityFinding finding)
{
    return source.checks && source.checks->hasFinding(finding);
}

// The bitrate a lossy file really delivers: what the checks suggest for a suspect MP3 (0 when they can't say, which
// is below any target), else its own.
int effectiveKbps(const SourceQuality &source)
{
    if (source.checks && (source.checks->hasFinding(QualityFinding::LikelyUpconvertedMp3) ||
                          source.checks->hasFinding(QualityFinding::LikelyReencoded))) {
        return source.checks->likelySourceKbps.value_or(0);
    }
    return source.bitrateKbps;
}

bool isWarning(Notice notice)
{
    return notice != Notice::Resample && notice != Notice::Dither && notice != Notice::LosslessReduction;
}

enum class Target { Mp3Top, Mp3Other, Lossless, Keep };

Target targetKind(const EncodeSettings &target)
{
    switch (target.format) {
    case OutputFormat::Mp3:
        return target.mp3.mode == Mp3Mode::Cbr && target.mp3.bitrateKbps >= 320 ? Target::Mp3Top : Target::Mp3Other;
    case OutputFormat::Aiff:
    case OutputFormat::Flac:
    case OutputFormat::Wav:
    case OutputFormat::Alac:
        return Target::Lossless;
    case OutputFormat::KeepAudio:
        break;
    }
    return Target::Keep;
}

int mp3Kbps(Mp3Settings mp3)
{
    if (mp3.mode == Mp3Mode::Cbr) {
        return mp3.bitrateKbps;
    }
    return kVbrAverageKbps[static_cast<std::size_t>(std::clamp(mp3.vbrQuality, 0, 9))];
}

audio::StreamInfo asStreamInfo(const SourceQuality &source)
{
    audio::StreamInfo info;
    info.container = source.container;
    info.codec = source.codec;
    info.sampleRate = source.sampleRate;
    info.channels = source.channels;
    info.bitsPerSample = source.bitDepth;
    info.lossless = source.lossless;
    return info;
}

QString kiloHertz(int hz)
{
    return hz % 1000 == 0 ? u"%1 kHz"_s.arg(hz / 1000) : u"%1 kHz"_s.arg(hz / 1000.0, 0, 'f', 1);
}

QString formatName(const SourceQuality &source)
{
    switch (source.codec) {
    case Codec::Mp3:
        return u"MP3"_s;
    case Codec::Mp2:
        return u"MP2"_s;
    case Codec::Mp1:
        return u"MP1"_s;
    case Codec::Flac:
        return source.container == Container::Ogg ? u"Ogg FLAC"_s : u"FLAC"_s;
    case Codec::Alac:
        return u"ALAC"_s;
    case Codec::Aac:
        return u"AAC"_s;
    case Codec::Vorbis:
        return u"Ogg Vorbis"_s;
    case Codec::Opus:
        return u"Opus"_s;
    case Codec::Pcm:
        if (source.container == Container::Aiff) {
            return u"AIFF"_s;
        }
        return source.container == Container::Wav ? u"WAV"_s : u"PCM"_s;
    case Codec::Other:
    case Codec::Unknown:
        break;
    }
    const QString name = source.codecName.toLower();
    if (name.startsWith(u"wma"_s)) {
        return u"WMA"_s;
    }
    if (name == u"ape"_s) {
        return u"Monkey's Audio"_s;
    }
    if (name == u"wavpack"_s) {
        return u"WavPack"_s;
    }
    if (name.startsWith(u"dsd"_s)) {
        return u"DSD"_s;
    }
    if (name == u"tta"_s) {
        return u"TTA"_s;
    }
    return name.isEmpty() ? u"this format"_s : name.toUpper();
}

// Rows of conversion §6.2.
SourceClass classify(const SourceQuality &source)
{
    if (source.lossless) {
        const bool playable =
            (source.codec == Codec::Flac || source.codec == Codec::Pcm || source.codec == Codec::Alac) &&
            source.container != Container::Ogg && source.container != Container::Other &&
            source.container != Container::Unknown;
        if (!playable) {
            return SourceClass::OtherLossless;
        }
        if (has(source, QualityFinding::LikelyFromLossy) || has(source, QualityFinding::LikelyPadded24Bit) ||
            has(source, QualityFinding::LikelyUpsampled)) {
            return SourceClass::LosslessSuspect;
        }
        return source.bitDepth > 16 || source.sampleRate > 48000 ? SourceClass::LosslessHiRes
                                                                 : SourceClass::LosslessGenuine;
    }
    switch (source.codec) {
    case Codec::Mp3: {
        const bool suspect =
            has(source, QualityFinding::LikelyUpconvertedMp3) || has(source, QualityFinding::LikelyReencoded);
        const bool high = source.constantBitrate ? source.bitrateKbps >= 320 : source.bitrateKbps >= kV0ClassKbps;
        return high && !suspect ? SourceClass::Mp3High : SourceClass::Mp3Lower;
    }
    case Codec::Aac:
        return SourceClass::Aac;
    default:
        return SourceClass::LossyUnplayable;
    }
}

struct Decision
{
    Action action = Action::Convert;
    QList<Notice> notices;
    Badge badge = Badge::None;
};

void addUnique(QList<Notice> &notices, Notice notice)
{
    if (!notices.contains(notice)) {
        notices.append(notice);
    }
}

// Lossless rows (genuine, hi-res, suspect, other).
Decision losslessSource(const SourceQuality &source, SourceClass sourceClass, Target target,
                        const EncodeSettings &settings, ResolvedOutput resolved, const HardwareProfile &hardware)
{
    Decision decision;
    const bool suspect = sourceClass == SourceClass::LosslessSuspect;
    const bool other = sourceClass == SourceClass::OtherLossless;
    if (target == Target::Mp3Top || target == Target::Mp3Other) {
        decision.action = suspect ? Action::Ask : Action::Convert; // a second-generation lossy file, if suspect
        if (suspect) {
            decision.notices.append(Notice::Suspect);
        }
        if (other) {
            decision.notices.append(Notice::NotPlayable); // why it's converted
        }
        if (resolved.resample) {
            decision.notices.append(Notice::Resample);
        }
        return decision;
    }
    if (resolved.upsample || resolved.upDepth) {
        if (other) {
            decision.action = Action::Refuse;
            return decision;
        }
        decision.action = Action::Ask;
        if (resolved.upsample) {
            decision.notices.append(Notice::Upsample);
        }
        if (resolved.upDepth) {
            decision.notices.append(Notice::UpDepth);
        }
        return decision;
    }
    decision.action = Action::Convert;
    // Reducing a padded 24-bit file to 16 bits loses nothing (the quantiser leaves samples already on the grid alone).
    const bool lossless16 = has(source, QualityFinding::LikelyPadded24Bit) && resolved.bitDepth == 16;
    if (suspect) {
        decision.notices.append(Notice::Suspect); // shown; the conversion itself is harmless
    }
    if (lossless16) {
        decision.notices.append(Notice::LosslessReduction);
    }
    if (other) {
        decision.notices.append(Notice::NotPlayable);
    }
    if (resolved.resample) {
        decision.notices.append(Notice::Resample);
    }
    if (resolved.dither && !lossless16) {
        decision.notices.append(Notice::Dither);
    }
    if (resolved.sampleRate > hardware.maxSampleRate || resolved.bitDepth > hardware.maxBitDepth) {
        decision.notices.append(Notice::HardwareRate);
        decision.action = Action::Ask;
    }
    if (settings.format == OutputFormat::Wav) {
        decision.notices.append(Notice::WavTags);
    }
    return decision;
}

// Lossy rows (MP3, AAC, unplayable).
Decision lossySource(const SourceQuality &source, SourceClass sourceClass, Target target,
                     const EncodeSettings &settings, ResolvedOutput resolved, AdviceContext context)
{
    Decision decision;
    const bool toMp3 = target == Target::Mp3Top || target == Target::Mp3Other;
    const bool higher = resolved.upsample || resolved.bitDepth > 16;
    if (!toMp3 && higher) {
        decision.action = Action::Refuse; // ⛔: a lossy file can't fill a higher rate or depth
        return decision;
    }
    if (sourceClass == SourceClass::LossyUnplayable) {
        // rekordbox can't play it: converting is the only way in, with confirmation either way (D-5).
        decision.action = Action::Ask;
        decision.notices = {Notice::NotPlayable, toMp3 ? Notice::Generation : Notice::LossyToLossless};
    } else if (sourceClass == SourceClass::Mp3High && target == Target::Mp3Top) {
        decision.action = Action::KeepAsIs; // CONV-2: an MP3 at the target isn't re-encoded
    } else if (context == AdviceContext::Filing) {
        // The arrival pipeline doesn't re-encode lossy audio it can't improve: kept with fresh tags, and marked.
        decision.action = Action::KeepAsIs;
        if (sourceClass == SourceClass::Aac) {
            decision.badge = Badge::FormatDiffers;
        } else if (toMp3) {
            decision.badge = effectiveKbps(source) < mp3Kbps(settings.mp3) ? Badge::BelowTarget : Badge::None;
        } else {
            decision.badge = sourceClass == SourceClass::Mp3High ? Badge::FormatDiffers : Badge::BelowTarget;
        }
    } else if (!toMp3) {
        decision.action = Action::Ask;
        decision.notices = {Notice::LossyToLossless};
    } else if (sourceClass == SourceClass::Mp3Lower && mp3Kbps(settings.mp3) > effectiveKbps(source)) {
        decision.action = Action::Ask;
        decision.notices = {Notice::LossyUp};
    } else {
        decision.action = Action::Ask;
        decision.notices = {Notice::Generation};
    }
    if (decision.action != Action::KeepAsIs && resolved.resample) {
        addUnique(decision.notices, Notice::Resample);
    }
    if (decision.action != Action::KeepAsIs && settings.format == OutputFormat::Wav) {
        addUnique(decision.notices, Notice::WavTags);
    }
    return decision;
}

double kbpsOf(int sampleRate, int channels, int bits)
{
    return static_cast<double>(sampleRate) * channels * bits / 1000.0;
}

double sizeFactor(const SourceQuality &source, const EncodeSettings &target, ResolvedOutput resolved)
{
    double sourceKbps = source.bitrateKbps;
    if (sourceKbps <= 0.0 && source.lossless) {
        sourceKbps = kbpsOf(source.sampleRate, source.channels, source.bitDepth);
    }
    if (sourceKbps <= 0.0) {
        return 0.0;
    }
    const int channels = std::clamp(source.channels, 1, 2);
    double outputKbps = sourceKbps;
    switch (target.format) {
    case OutputFormat::Mp3:
        outputKbps = mp3Kbps(target.mp3);
        break;
    case OutputFormat::Aiff:
    case OutputFormat::Wav:
        outputKbps = kbpsOf(resolved.sampleRate, channels, resolved.bitDepth);
        break;
    case OutputFormat::Flac:
    case OutputFormat::Alac:
        outputKbps = kbpsOf(resolved.sampleRate, channels, resolved.bitDepth) * kFlacSizeRatio;
        break;
    case OutputFormat::KeepAudio:
        break;
    }
    return outputKbps / sourceKbps;
}

QString factorText(double factor)
{
    return factor > 0.0 ? u"%1×"_s.arg(factor, 0, 'f', factor < 10.0 ? 1 : 0) : u"much"_s;
}

QString suspectDetail(const SourceQuality &source)
{
    if (!source.checks) {
        return {};
    }
    const audio::QualityReport &report = *source.checks;
    QStringList parts;
    if (report.hasFinding(QualityFinding::LikelyFromLossy)) {
        parts.append(
            report.likelySourceKbps
                ? u"Its spectrum stops at about %1, typical of a %2 kbps MP3. Converting it cannot improve it."_s
                      .arg(kiloHertz(report.cutoffHz))
                      .arg(*report.likelySourceKbps)
                : u"Its spectrum stops at about %1, typical of a lossy file. Converting it cannot improve it."_s.arg(
                      kiloHertz(report.cutoffHz)));
    }
    if (report.hasFinding(QualityFinding::LikelyPadded24Bit)) {
        parts.append(u"This %1-bit file contains only %2 bits of real data."_s.arg(report.sourceBitDepth)
                         .arg(report.effectiveBitDepth));
    }
    if (report.hasFinding(QualityFinding::LikelyUpsampled)) {
        parts.append(u"This %1 file has no content above %2; it was probably upsampled."_s.arg(
            kiloHertz(report.sampleRate), kiloHertz(report.contentBandwidthHz)));
    }
    return parts.join(u' ');
}

} // namespace

SourceQuality makeSourceQuality(const audio::StreamInfo &info, const std::optional<audio::QualityReport> &checks)
{
    SourceQuality quality;
    quality.container = info.container;
    quality.codec = info.codec;
    quality.codecName = info.codecName;
    quality.lossless = info.lossless;
    quality.sampleRate = info.sampleRate;
    quality.bitDepth = info.lossless ? info.bitsPerSample : 0;
    quality.effectiveBitDepth = checks && checks->effectiveBitDepth > 0 ? checks->effectiveBitDepth : quality.bitDepth;
    quality.channels = info.channels;
    quality.bitrateKbps = info.bitrateKbps;
    quality.constantBitrate = info.mpeg && info.mpeg->constantBitrate();
    quality.encoder = info.lameTag ? info.lameTag->encoder : QString();
    quality.checks = checks;
    return quality;
}

Result<SourceQuality> probeSourceQuality(const QString &path, bool runChecks, const audio::CancellationFlag *cancel)
{
    Result<std::shared_ptr<audio::FileSource>> opened = audio::FileSource::open(path);
    if (!opened.ok()) {
        return opened.error();
    }
    const std::shared_ptr<audio::FileSource> source = opened.takeValue();
    audio::DecoderOptions options;
    options.channels = 0;
    audio::Decoder decoder;
    switch (decoder.open(source, options)) {
    case audio::Decoder::OpenStatus::Ready:
        break;
    case audio::Decoder::OpenStatus::Failed:
        return decoder.error();
    case audio::Decoder::OpenStatus::NeedMoreData:
    case audio::Decoder::OpenStatus::NeedsCompleteFile:
        return makeError(ErrorCode::Internal, u"the decoder wants more of %1, which is complete"_s.arg(path));
    }
    if (!runChecks) {
        return makeSourceQuality(decoder.info());
    }
    Result<audio::QualityReport> checks = audio::analyzeQuality(source, {}, cancel);
    if (!checks.ok()) {
        return checks.error();
    }
    return makeSourceQuality(decoder.info(), checks.takeValue());
}

Advice advise(const SourceQuality &source, const EncodeSettings &target, AdviceContext context,
              const HardwareProfile &hardware)
{
    Advice advice;
    advice.sourceClass = classify(source);
    const ResolvedOutput resolved = resolveOutput(asStreamInfo(source), target);
    advice.outputSampleRate = resolved.sampleRate;
    advice.outputBitDepth = resolved.bitDepth;
    advice.sizeFactor = sizeFactor(source, target, resolved);
    const Target kind = targetKind(target);

    Decision decision;
    if (kind == Target::Keep) {
        decision.action = Action::KeepAsIs;
        if (advice.sourceClass == SourceClass::LossyUnplayable || advice.sourceClass == SourceClass::OtherLossless) {
            decision.notices.append(Notice::NotPlayable);
        }
        if (advice.sourceClass == SourceClass::LosslessSuspect) {
            decision.notices.append(Notice::Suspect);
        }
        if (source.container == Container::Wav) {
            decision.notices.append(Notice::WavTags);
        }
    } else if (source.lossless) {
        decision = losslessSource(source, advice.sourceClass, kind, target, resolved, hardware);
    } else {
        decision = lossySource(source, advice.sourceClass, kind, target, resolved, context);
    }
    advice.action = decision.action;
    advice.badge = decision.badge;
    // Warnings first, then information, each group in the order it was added.
    std::ranges::stable_partition(decision.notices, isWarning);
    advice.notices = decision.notices;
    return advice;
}

NoticeText describeNotice(Notice notice, const SourceQuality &source, const EncodeSettings &target,
                          const Advice &advice, const HardwareProfile &hardware)
{
    const QString src = formatName(source);
    const QString dst = describeSettings(target);
    switch (notice) {
    case Notice::LossyToLossless:
        return {
            u"No quality gain — about %1 larger"_s.arg(factorText(advice.sizeFactor)),
            u"%1 is a lossy file. Converting it to %2 keeps exactly the sound of the %1 file; it cannot restore what "
            "the lossy encoding removed."_s.arg(src, dst)};
    case Notice::LossyUp:
        return {u"No quality gain — generation loss"_s,
                u"%1 at %2 kbps cannot become better at %3 kbps. Re-encoding adds a second round of compression "
                "artefacts. Selecta files it as-is and marks it 'below target'."_s.arg(src)
                    .arg(effectiveKbps(source) > 0 ? effectiveKbps(source) : source.bitrateKbps)
                    .arg(mp3Kbps(target.mp3))};
    case Notice::Generation:
        return {u"Generation loss"_s, u"Re-encoding a lossy file to another lossy format loses quality each time. Keep "
                                      "the original if you can."_s};
    case Notice::Upsample:
        return {u"No quality gain — larger file"_s,
                u"Raising %1 to %2 adds no information; the file becomes %3 larger."_s.arg(
                    kiloHertz(source.sampleRate), kiloHertz(advice.outputSampleRate), factorText(advice.sizeFactor))};
    case Notice::UpDepth:
        return {u"No quality gain"_s,
                u"Converting %1-bit to %2-bit only pads with zeros."_s.arg(source.bitDepth).arg(advice.outputBitDepth)};
    case Notice::Suspect:
        return {u"Source is probably not what it claims"_s, suspectDetail(source)};
    case Notice::NotPlayable:
        return {u"rekordbox can't play %1"_s.arg(src),
                u"rekordbox 7 plays MP3, AAC, WAV, AIFF, FLAC and ALAC. Choose MP3 320 (small, one more lossy "
                "generation) or AIFF (no further loss, large)."_s};
    case Notice::HardwareRate:
        return {u"May not play on your players"_s,
                u"%1 / %2-bit exceeds %3 (max %4 / %5-bit)."_s.arg(kiloHertz(advice.outputSampleRate))
                    .arg(advice.outputBitDepth)
                    .arg(hardware.name, kiloHertz(hardware.maxSampleRate))
                    .arg(hardware.maxBitDepth)};
    case Notice::WavTags:
        return {u"Tags limited in WAV"_s,
                u"rekordbox reads only basic RIFF INFO from WAV: no artwork, label or key. Use "
                "AIFF to keep them."_s};
    case Notice::Resample:
        return {
            u"Resampled to %1"_s.arg(kiloHertz(advice.outputSampleRate)),
            target.format == OutputFormat::Mp3
                ? u"MP3 cannot store %1; Selecta uses a high-quality SoX resampler."_s.arg(kiloHertz(source.sampleRate))
                : u"%1 becomes %2 (your players' limit or your choice); Selecta uses a high-quality SoX resampler."_s
                      .arg(kiloHertz(source.sampleRate), kiloHertz(advice.outputSampleRate))};
    case Notice::Dither:
        return {u"Reduced to 16-bit with dither"_s,
                u"TPDF dither avoids quantisation distortion when reducing bit depth."_s};
    case Notice::LosslessReduction:
        return {u"No loss: only 16 bits are real"_s,
                u"This %1-bit file contains only 16 bits of real data, so the 16-bit version is identical."_s.arg(
                    source.bitDepth)};
    }
    return {};
}

QString describeSource(const SourceQuality &source)
{
    const QString name = formatName(source);
    if (source.lossless) {
        return source.bitDepth > 0
                   ? u"%1 %2-bit / %3"_s.arg(name).arg(source.bitDepth).arg(kiloHertz(source.sampleRate))
                   : u"%1 / %2"_s.arg(name, kiloHertz(source.sampleRate));
    }
    if (source.codec == Codec::Mp3 && source.constantBitrate) {
        return u"MP3 %1 kbps CBR"_s.arg(source.bitrateKbps);
    }
    if (source.codec == Codec::Mp3) {
        return u"MP3 VBR ≈ %1 kbps"_s.arg(source.bitrateKbps);
    }
    return source.bitrateKbps > 0 ? u"%1 ≈ %2 kbps"_s.arg(name).arg(source.bitrateKbps) : name;
}

QualityVerdict judgeQuality(const SourceQuality &source)
{
    QualityVerdict verdict;
    verdict.claimed = describeSource(source);
    if (!source.checks) {
        verdict.reason = u"The quality checks haven't run."_s;
        return verdict;
    }
    const audio::QualityReport &report = *source.checks;
    if (!report.findings.isEmpty()) {
        QStringList reasons;
        for (const QualityFinding finding : report.findings) {
            reasons.append(audio::describeFinding(finding, report) + u'.');
        }
        verdict.kind = QualityVerdict::Kind::Suspect;
        verdict.reason = reasons.join(u' ');
        return verdict;
    }
    // Other lossy codecs get the MP3 rule: a file claiming 256 kbps or more should keep the band above ~18 kHz.
    if (!source.lossless && source.codec != Codec::Mp3 && report.cutoffHz > 0 && report.sampleRate >= 44100 &&
        source.bitrateKbps >= kTopLossyKbps && report.cutoffHz <= kLossyCutoffLimitHz) {
        verdict.kind = QualityVerdict::Kind::Suspect;
        verdict.reason = u"Likely from a lower bitrate (the spectrum stops at %1, low for %2 kbps)."_s
                             .arg(kiloHertz(report.cutoffHz))
                             .arg(source.bitrateKbps);
        return verdict;
    }
    const bool depthChecked = source.lossless && report.sourceBitDepth > 16 && report.sourceBitDepth <= 24;
    if (report.cutoffHz <= 0) {
        if (depthChecked) {
            verdict.kind = QualityVerdict::Kind::Genuine;
            verdict.reason = u"All %1 bits carry audio; the spectrum is too short, quiet or narrow to judge."_s.arg(
                report.sourceBitDepth);
            return verdict;
        }
        verdict.reason = u"Too short, too quiet or too band-limited to judge from its spectrum."_s;
        return verdict;
    }
    verdict.kind = QualityVerdict::Kind::Genuine;
    QString reason = u"The spectrum reaches %1"_s.arg(kiloHertz(report.cutoffHz));
    if (source.lossless) {
        reason += u": no sign of a lossy source."_s;
        if (depthChecked) {
            reason += u" All %1 bits carry audio."_s.arg(report.sourceBitDepth);
        }
        if (report.contentBandwidthHz > 0) {
            reason += u" Content up to %1."_s.arg(kiloHertz(report.contentBandwidthHz));
        }
    } else if (source.codec == Codec::Mp3) {
        reason += u", as expected for %1."_s.arg(verdict.claimed);
    } else {
        reason += u'.';
    }
    verdict.reason = std::move(reason);
    return verdict;
}

} // namespace selecta::convert
