// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchFilter.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonValue>

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
    case HiddenReason::TooSlow:
        return translated("Slow peer");
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
    if (filter.minSpeedKBps > 0 && std::cmp_less(result.hit.averageSpeed, qint64{filter.minSpeedKBps} * 1000)) {
        return HiddenReason::TooSlow;
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

namespace {

// JSON keys: short, stable, never renamed (saved presets and Wishlist searches carry them).
constexpr auto kFormats = "formats"_L1;
constexpr auto kBitrateRule = "bitrateRule"_L1;
constexpr auto kBitrate = "bitrateKbps"_L1;
constexpr auto kCbrOnly = "cbrOnly"_L1;
constexpr auto kBitDepth = "minBitDepth"_L1;
constexpr auto kSampleRates = "sampleRates"_L1;
constexpr auto kMinLength = "minDurationSec"_L1;
constexpr auto kMaxLength = "maxDurationSec"_L1;
constexpr auto kMinSize = "minSizeBytes"_L1;
constexpr auto kMaxSize = "maxSizeBytes"_L1;
constexpr auto kFreeSlot = "freeSlotOnly"_L1;
constexpr auto kMaxQueue = "maxQueue"_L1;
constexpr auto kMinSpeed = "minSpeedKBps"_L1;
constexpr auto kExcluded = "excludedWords"_L1;
constexpr auto kStrict = "strict"_L1;
constexpr auto kOwned = "owned"_L1;
constexpr auto kOnlyRelease = "onlyThisRelease"_L1;
constexpr int kMaxExcludedWords = 50;
constexpr int kMaxSampleRates = 16;

// An integer in [low, high], or `fallback`.
qint64 bounded(const QJsonValue &value, qint64 fallback, qint64 low, qint64 high)
{
    if (!value.isDouble()) {
        return fallback;
    }
    const double number = value.toDouble();
    if (number < static_cast<double>(low) || number > static_cast<double>(high)) {
        return fallback;
    }
    return static_cast<qint64>(number);
}

SearchFilter mp3OrLossless()
{
    SearchFilter filter;
    for (const SearchFormat format :
         {SearchFormat::Flac, SearchFormat::Mp3, SearchFormat::Wav, SearchFormat::Aiff, SearchFormat::Alac}) {
        filter.setAllowed(format, true);
    }
    filter.bitrateRule = SearchFilter::BitrateRule::AtLeast;
    filter.bitrateKbps = 320;
    filter.strict = true;
    return filter;
}

SearchFilter losslessOnly(int minBitDepth)
{
    SearchFilter filter;
    for (const SearchFormat format : {SearchFormat::Flac, SearchFormat::Wav, SearchFormat::Aiff, SearchFormat::Alac}) {
        filter.setAllowed(format, true);
    }
    filter.minBitDepth = minBitDepth;
    filter.strict = true;
    return filter;
}

} // namespace

QJsonObject filterToJson(const SearchFilter &filter)
{
    QJsonArray rates;
    for (const int rate : filter.sampleRates) {
        rates.append(rate);
    }
    return QJsonObject{
        {kFormats, filter.formats},
        {kBitrateRule, static_cast<int>(filter.bitrateRule)},
        {kBitrate, filter.bitrateKbps},
        {kCbrOnly, filter.cbrOnly},
        {kBitDepth, filter.minBitDepth},
        {kSampleRates, rates},
        {kMinLength, filter.minDurationSec},
        {kMaxLength, filter.maxDurationSec},
        {kMinSize, filter.minSizeBytes},
        {kMaxSize, filter.maxSizeBytes},
        {kFreeSlot, filter.freeSlotOnly},
        {kMaxQueue, filter.maxQueue},
        {kMinSpeed, filter.minSpeedKBps},
        {kExcluded, QJsonArray::fromStringList(filter.excludedWords)},
        {kStrict, filter.strict},
        {kOwned, static_cast<int>(filter.owned)},
        {kOnlyRelease, filter.onlyThisRelease},
    };
}

SearchFilter filterFromJson(const QJsonObject &json)
{
    constexpr qint64 kDay = qint64{24} * 3600;
    constexpr auto kTerabyte = qint64{1'000'000'000'000};
    SearchFilter filter;
    filter.formats = static_cast<quint16>(bounded(json.value(kFormats), 0, 0, 0xFFFF));
    filter.bitrateRule = static_cast<SearchFilter::BitrateRule>(
        bounded(json.value(kBitrateRule), 0, 0, static_cast<int>(SearchFilter::BitrateRule::AtMost)));
    filter.bitrateKbps = static_cast<int>(bounded(json.value(kBitrate), filter.bitrateKbps, 8, 2000));
    filter.cbrOnly = json.value(kCbrOnly).toBool(false);
    filter.minBitDepth = static_cast<int>(bounded(json.value(kBitDepth), 0, 0, 32));
    const QJsonArray rates = json.value(kSampleRates).toArray();
    for (const auto &rate : rates) {
        const auto hz = static_cast<int>(bounded(rate, 0, 0, 768'000));
        if (hz > 0 && filter.sampleRates.size() < kMaxSampleRates && !filter.sampleRates.contains(hz)) {
            filter.sampleRates.append(hz);
        }
    }
    filter.minDurationSec = static_cast<int>(bounded(json.value(kMinLength), 0, 0, kDay));
    filter.maxDurationSec = static_cast<int>(bounded(json.value(kMaxLength), 0, 0, kDay));
    filter.minSizeBytes = bounded(json.value(kMinSize), 0, 0, kTerabyte);
    filter.maxSizeBytes = bounded(json.value(kMaxSize), 0, 0, kTerabyte);
    filter.freeSlotOnly = json.value(kFreeSlot).toBool(false);
    filter.maxQueue = static_cast<int>(bounded(json.value(kMaxQueue), 0, 0, 1'000'000));
    filter.minSpeedKBps = static_cast<int>(bounded(json.value(kMinSpeed), 0, 0, 1'000'000));
    const QJsonArray words = json.value(kExcluded).toArray();
    for (const auto &word : words) {
        const QString text = word.toString().trimmed().left(100);
        if (!text.isEmpty() && filter.excludedWords.size() < kMaxExcludedWords) {
            filter.excludedWords.append(text);
        }
    }
    filter.strict = json.value(kStrict).toBool(false);
    filter.owned = static_cast<SearchFilter::OwnedResults>(
        bounded(json.value(kOwned), 0, 0, static_cast<int>(SearchFilter::OwnedResults::Hide)));
    filter.onlyThisRelease = json.value(kOnlyRelease).toBool(true);
    return filter;
}

QList<FilterPreset> builtInPresets()
{
    return {
        FilterPreset{.id = kMp3OrLosslessPreset.toString(),
                     .name = translated("MP3 320 or lossless"),
                     .filter = mp3OrLossless(),
                     .builtIn = true},
        FilterPreset{.id = kLosslessPreset.toString(),
                     .name = translated("Lossless only"),
                     .filter = losslessOnly(0),
                     .builtIn = true},
        FilterPreset{.id = kHiResPreset.toString(),
                     .name = translated("Hi-res (24-bit lossless)"),
                     .filter = losslessOnly(24),
                     .builtIn = true},
        FilterPreset{.id = kAnyQualityPreset.toString(),
                     .name = translated("Any quality"),
                     .filter = SearchFilter{},
                     .builtIn = true},
    };
}

std::optional<FilterPreset> builtInPreset(QStringView id)
{
    const QList<FilterPreset> presets = builtInPresets();
    for (const FilterPreset &preset : presets) {
        if (preset.id == id) {
            return preset;
        }
    }
    return std::nullopt;
}

} // namespace selecta::core
