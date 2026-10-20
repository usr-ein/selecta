// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchFilter.h"

#include <QCoreApplication>

#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

quint16 bitOf(SearchFormat format)
{
    return static_cast<quint16>(1U << static_cast<unsigned>(format));
}

QString translated(const char *text)
{
    return QCoreApplication::translate("selecta::core::SearchFilter", text);
}

} // namespace

SearchFormat searchFormatOf(match::Codec codec)
{
    switch (codec) {
    case match::Codec::Flac:
        return SearchFormat::Flac;
    case match::Codec::Mp3:
        return SearchFormat::Mp3;
    case match::Codec::Wav:
        return SearchFormat::Wav;
    case match::Codec::Aiff:
        return SearchFormat::Aiff;
    case match::Codec::Alac:
        return SearchFormat::Alac;
    case match::Codec::Aac:
        return SearchFormat::Aac;
    case match::Codec::Vorbis:
        return SearchFormat::Ogg;
    case match::Codec::Opus:
        return SearchFormat::Opus;
    default:
        break;
    }
    return SearchFormat::Other;
}

QString searchFormatName(SearchFormat format)
{
    switch (format) {
    case SearchFormat::Flac:
        return u"FLAC"_s;
    case SearchFormat::Mp3:
        return u"MP3"_s;
    case SearchFormat::Wav:
        return u"WAV"_s;
    case SearchFormat::Aiff:
        return u"AIFF"_s;
    case SearchFormat::Alac:
        return u"ALAC/M4A"_s;
    case SearchFormat::Aac:
        return u"AAC"_s;
    case SearchFormat::Ogg:
        return u"OGG"_s;
    case SearchFormat::Opus:
        return u"OPUS"_s;
    case SearchFormat::Other:
        break;
    }
    return translated("Other");
}

QString hiddenReasonLabel(HiddenReason reason)
{
    switch (reason) {
    case HiddenReason::NotAudio:
        return translated("Not audio");
    case HiddenReason::NotThisRelease:
        return translated("Not this release");
    case HiddenReason::Format:
        return translated("Format");
    case HiddenReason::Bitrate:
        return translated("Bitrate");
    case HiddenReason::Resolution:
        return translated("Bit depth or sample rate");
    case HiddenReason::UnknownQuality:
        return translated("Quality not reported");
    case HiddenReason::Implausible:
        return translated("Size doesn't match the quality");
    case HiddenReason::Length:
        return translated("Length");
    case HiddenReason::Size:
        return translated("Size");
    case HiddenReason::NoFreeSlot:
        return translated("No free slot");
    case HiddenReason::QueueTooLong:
        return translated("Long queue");
    case HiddenReason::ExcludedWord:
        return translated("Excluded words");
    case HiddenReason::Owned:
        return translated("Already have");
    case HiddenReason::None:
        break;
    }
    return {};
}

bool SearchFilter::allows(SearchFormat format) const
{
    return formats == 0 || (formats & bitOf(format)) != 0;
}

void SearchFilter::setAllowed(SearchFormat format, bool allowed)
{
    if (allowed) {
        formats |= bitOf(format);
    } else {
        formats &= static_cast<quint16>(~bitOf(format));
    }
}

namespace {

bool bitrateFits(SearchFilter::BitrateRule rule, int kbps, int wanted)
{
    switch (rule) {
    case SearchFilter::BitrateRule::Equal:
        return kbps == wanted;
    case SearchFilter::BitrateRule::AtLeast:
        return kbps >= wanted;
    case SearchFilter::BitrateRule::AtMost:
        return kbps <= wanted;
    case SearchFilter::BitrateRule::Any:
        break;
    }
    return true;
}

// The format, the lossy and lossless rules and strict mode (SRCH-2…4, SRCH-6, SRCH-7). A rule can't be checked against
// what the peer didn't say: such files fall to strict mode.
HiddenReason qualityReason(const SearchFilter &filter, const SearchResult &result)
{
    const match::AudioQuality &quality = result.quality.quality;
    if (!filter.allows(searchFormatOf(quality.codec))) {
        return HiddenReason::Format;
    }
    const bool lossless = match::isLossless(quality.codec);
    bool ruleUnknown = false;
    if (lossless) {
        if (filter.minBitDepth > 0) {
            if (quality.bitDepth <= 0) {
                ruleUnknown = true;
            } else if (quality.bitDepth < filter.minBitDepth) {
                return HiddenReason::Resolution;
            }
        }
        if (!filter.sampleRates.isEmpty()) {
            if (quality.sampleRateHz <= 0) {
                ruleUnknown = true;
            } else if (!filter.sampleRates.contains(quality.sampleRateHz)) {
                return HiddenReason::Resolution;
            }
        }
    } else if (filter.bitrateRule != SearchFilter::BitrateRule::Any) {
        if (quality.bitrateKbps <= 0) {
            ruleUnknown = true;
        } else if (!bitrateFits(filter.bitrateRule, quality.bitrateKbps, filter.bitrateKbps) ||
                   (filter.cbrOnly && quality.vbr.value_or(false))) {
            return HiddenReason::Bitrate;
        }
    }
    if (!filter.strict) {
        return HiddenReason::None;
    }
    if (!result.quality.plausible) {
        return HiddenReason::Implausible;
    }
    return !result.quality.established || ruleUnknown ? HiddenReason::UnknownQuality : HiddenReason::None;
}

// Length, size, the peer's availability, excluded words and what the user already has (SRCH-5).
HiddenReason otherReason(const SearchFilter &filter, const SearchResult &result)
{
    const int duration = result.durationSec();
    if (duration > 0 && ((filter.minDurationSec > 0 && duration < filter.minDurationSec) ||
                         (filter.maxDurationSec > 0 && duration > filter.maxDurationSec))) {
        return HiddenReason::Length;
    }
    const auto size = static_cast<qint64>(result.hit.size);
    if ((filter.minSizeBytes > 0 && size < filter.minSizeBytes) ||
        (filter.maxSizeBytes > 0 && size > filter.maxSizeBytes)) {
        return HiddenReason::Size;
    }
    if (filter.freeSlotOnly && !result.hit.freeSlot) {
        return HiddenReason::NoFreeSlot;
    }
    if (filter.maxQueue > 0 && std::cmp_greater(result.hit.queueLength, filter.maxQueue)) {
        return HiddenReason::QueueTooLong;
    }
    if (!filter.excludedWords.isEmpty()) {
        const QString path = result.path.toCaseFolded();
        const bool excluded = std::ranges::any_of(filter.excludedWords, [&path](const QString &word) {
            const QString folded = word.trimmed().toCaseFolded();
            return !folded.isEmpty() && path.contains(folded);
        });
        if (excluded) {
            return HiddenReason::ExcludedWord;
        }
    }
    if (filter.owned == SearchFilter::OwnedResults::Hide && isOwned(result.ownership)) {
        return HiddenReason::Owned;
    }
    return HiddenReason::None;
}

} // namespace

HiddenReason SearchFilter::hiddenReason(const SearchResult &result, bool releaseChosen) const
{
    if (!result.isAudio()) {
        return HiddenReason::NotAudio;
    }
    if (releaseChosen && onlyThisRelease && !result.fitsRelease()) {
        return HiddenReason::NotThisRelease;
    }
    if (const HiddenReason reason = qualityReason(*this, result); reason != HiddenReason::None) {
        return reason;
    }
    return otherReason(*this, result);
}

SearchFilter presetFilter(QualityWanted wanted)
{
    SearchFilter filter;
    switch (wanted) {
    case QualityWanted::Mp3_320OrLossless:
        for (const SearchFormat format :
             {SearchFormat::Flac, SearchFormat::Mp3, SearchFormat::Wav, SearchFormat::Aiff, SearchFormat::Alac}) {
            filter.setAllowed(format, true);
        }
        filter.bitrateRule = SearchFilter::BitrateRule::AtLeast;
        filter.bitrateKbps = 320;
        filter.strict = true;
        break;
    case QualityWanted::LosslessOnly:
        for (const SearchFormat format :
             {SearchFormat::Flac, SearchFormat::Wav, SearchFormat::Aiff, SearchFormat::Alac}) {
            filter.setAllowed(format, true);
        }
        filter.strict = true;
        break;
    case QualityWanted::Any:
        break;
    }
    return filter;
}

QString qualityWantedName(QualityWanted wanted)
{
    switch (wanted) {
    case QualityWanted::Mp3_320OrLossless:
        return translated("MP3 320 or lossless");
    case QualityWanted::LosslessOnly:
        return translated("Lossless only");
    case QualityWanted::Any:
        break;
    }
    return translated("Any quality");
}

} // namespace selecta::core
