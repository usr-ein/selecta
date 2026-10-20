// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/TagPolicy.h"

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::convert {
namespace {

// Never copied: Selecta sets its own encoder fields, and a stale length or iTunes' gapless and normalisation data
// would describe another encoding.
constexpr std::array<QStringView, 9> kAlwaysDenied = {u"ENCODEDBY",       u"ENCODING", u"ENCODER",
                                                      u"ENCODERSETTINGS", u"LENGTH",   u"ITUNNORM",
                                                      u"ITUNSMPB",        u"ITUNPGAP", u"ITUNES_CDDB_1"};
constexpr std::array<QStringView, 3> kGainPrefixes = {u"REPLAYGAIN_", u"MP3GAIN_", u"R128_"};
// Other DJ apps' private cues, grids and overviews (Serato, Traktor), whatever container they came in.
constexpr std::array<QStringView, 2> kDjAppMarkers = {u"SERATO", u"TRAKTOR"};

bool startsWithAny(const QString &key, std::span<const QStringView> prefixes)
{
    return std::ranges::any_of(prefixes, [&key](QStringView prefix) { return key.startsWith(prefix); });
}

} // namespace

bool isDeniedTagKey(QStringView rawKey, const TagPolicyOptions &options)
{
    const QString key = rawKey.toString().toUpper();
    if (std::ranges::find(kAlwaysDenied, QStringView(key)) != kAlwaysDenied.end()) {
        return true;
    }
    // COMMENT:<description> frames: iTunes keeps its internal data in comments with these descriptions.
    if (key.startsWith(u"COMMENT:"_s)) {
        const QString description = key.mid(8);
        if (std::ranges::find(kAlwaysDenied, QStringView(description)) != kAlwaysDenied.end()) {
            return true;
        }
    }
    if (options.transcoding && startsWithAny(key, kGainPrefixes)) {
        return true;
    }
    if ((options.transcoding || options.stripDjAppData) &&
        std::ranges::any_of(kDjAppMarkers, [&key](QStringView marker) { return key.contains(marker); })) {
        return true;
    }
    return false;
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
    if (!options.encodedBy.isEmpty()) {
        out.set(keys::EncodedBy, options.encodedBy, Provenance::Selecta);
    }
    if (!options.encoderSettings.isEmpty()) {
        out.set(keys::Encoding, options.encoderSettings, Provenance::Selecta);
    }
    return out;
}

} // namespace selecta::convert
