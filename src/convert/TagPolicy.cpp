// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/TagPolicy.h"

#include <algorithm>
#include <array>
#include <span>

namespace selecta::convert {
namespace {

// Dropped when the audio is re-encoded: Selecta sets its own encoder fields, and a length or iTunes' gapless and
// normalisation data would describe the source's encoding, not the new one.
constexpr std::array<QStringView, 9> kEncodingFields = {u"ENCODEDBY",       u"ENCODING", u"ENCODER",
                                                        u"ENCODERSETTINGS", u"LENGTH",   u"ITUNNORM",
                                                        u"ITUNSMPB",        u"ITUNPGAP", u"ITUNES_CDDB_1"};
constexpr std::array<QStringView, 3> kGainPrefixes = {u"REPLAYGAIN_", u"MP3GAIN_", u"R128_"};
// Other DJ apps' private cues, grids and overviews (Serato, Traktor), whatever container they came in.
constexpr std::array<QStringView, 2> kDjAppMarkers = {u"SERATO", u"TRAKTOR"};

bool isEncodingField(QStringView key)
{
    if (std::ranges::find(kEncodingFields, key) != kEncodingFields.end()) {
        return true;
    }
    // COMMENT:<description>: iTunes keeps its internal data in comments with these descriptions.
    constexpr QStringView commentPrefix = u"COMMENT:";
    return key.startsWith(commentPrefix) &&
           std::ranges::find(kEncodingFields, key.sliced(commentPrefix.size())) != kEncodingFields.end();
}

bool startsWithAny(QStringView key, std::span<const QStringView> prefixes)
{
    return std::ranges::any_of(prefixes, [key](QStringView prefix) { return key.startsWith(prefix); });
}

bool namesDjApp(QStringView key)
{
    return std::ranges::any_of(kDjAppMarkers, [key](QStringView marker) { return key.contains(marker); });
}

} // namespace

bool isDeniedTagKey(QStringView rawKey, const TagPolicyOptions &options)
{
    const QString key = rawKey.toString().toUpper();
    if (options.transcoding && (isEncodingField(key) || startsWithAny(key, kGainPrefixes))) {
        return true;
    }
    return (options.transcoding || options.stripDjAppData) && namesDjApp(key);
}

MetadataModel applyTagPolicy(const MetadataModel &source, const TagPolicyOptions &options)
{
    MetadataModel out;
    const QMap<QString, MetadataModel::Field> &fields = source.fields();
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (!isDeniedTagKey(it.key(), options)) {
            out.set(it.key(), it.value().values, it.value().provenance);
        }
    }
    out.setPictures(source.pictures());
    // Selecta's own encoder fields only describe audio it encoded (D-10).
    if (options.transcoding && !options.encodedBy.isEmpty()) {
        out.set(keys::EncodedBy, options.encodedBy, Provenance::Selecta);
    }
    if (options.transcoding && !options.encoderSettings.isEmpty()) {
        out.set(keys::Encoding, options.encoderSettings, Provenance::Selecta);
    }
    return out;
}

} // namespace selecta::convert
