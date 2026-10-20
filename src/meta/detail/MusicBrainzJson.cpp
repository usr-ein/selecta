// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/MusicBrainzJson.h"

#include "match/Normalizer.h"
#include "meta/detail/Identifiers.h"
#include "meta/detail/Json.h"

#include <QJsonArray>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail::musicbrainz {

namespace {

constexpr qint64 kMaxLengthMs = qint64{24} * 3600 * 1000;
constexpr qint64 kLengthToleranceMs = 3000; // ±3 s (D23)

// The squashed artist key of match's Normalizer, so "Röyksopp" = "Royksopp" and "Daft Punk" = "DaftPunk".
QString artistKey(const QString &name)
{
    return match::Normalizer::squash(match::Normalizer::normalize(name, match::FieldKind::Artist));
}

QString mbid(const QJsonObject &object)
{
    const QString id = json::text(object, "id"_L1, 64);
    return isValidMbid(id) ? id : QString();
}

bool isCompilationOrMix(const ReleaseInfo &release)
{
    return std::ranges::any_of(release.secondaryTypes, [](const QString &type) {
        return type.compare(u"Compilation"_s, Qt::CaseInsensitive) == 0 ||
               type.compare(u"DJ-mix"_s, Qt::CaseInsensitive) == 0;
    });
}

bool wantsCompilation(const Release &release)
{
    return release.albumType == u"compilation"_s || release.albumType == u"compile"_s;
}

bool typeMatches(const ReleaseInfo &candidate, const Release &release)
{
    const QString type = candidate.primaryType.toLower();
    if (release.albumType == u"single"_s || release.albumType == u"ep"_s) {
        return type == u"single"_s || type == u"ep"_s; // Spotify files EPs as singles
    }
    if (release.albumType == u"album"_s || wantsCompilation(release)) {
        return type == u"album"_s;
    }
    return true;
}

// Earliest by date; undated releases last; ties keep MusicBrainz's order.
std::optional<ReleaseInfo> earliest(const QList<ReleaseInfo> &candidates,
                                    const std::function<bool(const ReleaseInfo &)> &eligible)
{
    std::optional<ReleaseInfo> best;
    for (const ReleaseInfo &candidate : candidates) {
        if (!eligible(candidate)) {
            continue;
        }
        if (!best.has_value()) {
            best = candidate;
            continue;
        }
        const bool candidateDated = !candidate.date.isEmpty();
        const bool bestDated = !best->date.isEmpty();
        if (candidateDated && (!bestDated || candidate.date < best->date)) {
            best = candidate;
        }
    }
    return best;
}

} // namespace

QList<Recording> recordings(const QJsonObject &object)
{
    QList<Recording> list;
    const QJsonArray array = json::array(object, "recordings"_L1);
    for (const auto &value : array) {
        if (list.size() >= kMaxRecordings) {
            break;
        }
        const QJsonObject item = value.toObject();
        Recording recording;
        recording.id = mbid(item);
        if (recording.id.isEmpty()) {
            continue;
        }
        recording.title = json::text(item, "title"_L1);
        if (const std::optional<qint64> length = json::integer(item, "length"_L1);
            length.has_value() && *length > 0 && *length < kMaxLengthMs) {
            recording.lengthMs = length;
        }
        recording.score = static_cast<int>(std::clamp<qint64>(json::integer(item, "score"_L1).value_or(0), 0, 100));
        const QJsonArray credits = json::array(item, "artist-credit"_L1);
        for (const auto &creditValue : credits) {
            if (recording.artists.size() >= json::kMaxListItems) {
                break;
            }
            const QJsonObject credit = creditValue.toObject();
            const QJsonObject artist = json::object(credit, "artist"_L1);
            QString name = json::text(credit, "name"_L1);
            if (name.isEmpty()) {
                name = json::text(artist, "name"_L1);
            }
            if (name.isEmpty()) {
                continue;
            }
            recording.artists.append(name);
            recording.artistIds.append(mbid(artist));
        }
        list.append(recording);
    }
    return list;
}

QList<ReleaseInfo> releases(const QJsonObject &object)
{
    QList<ReleaseInfo> list;
    const QJsonArray array = json::array(object, "releases"_L1);
    for (const auto &value : array) {
        if (list.size() >= kMaxReleases) {
            break;
        }
        const QJsonObject item = value.toObject();
        ReleaseInfo release;
        release.id = mbid(item);
        if (release.id.isEmpty()) {
            continue;
        }
        release.title = json::text(item, "title"_L1);
        release.date = releaseDate(json::text(item, "date"_L1, 32)).first;
        release.status = json::text(item, "status"_L1, 64);
        release.barcode = normalizedBarcode(json::text(item, "barcode"_L1, 32));
        const QJsonObject group = json::object(item, "release-group"_L1);
        release.releaseGroupId = mbid(group);
        release.primaryType = json::text(group, "primary-type"_L1, 64);
        const QJsonArray secondary = json::array(group, "secondary-types"_L1);
        for (const auto &type : secondary) {
            if (type.isString() && release.secondaryTypes.size() < 16) {
                release.secondaryTypes.append(json::cleanText(type.toString(), 64));
            }
        }
        const QJsonArray labels = json::array(item, "label-info"_L1);
        for (const auto &labelValue : labels) {
            if (release.labels.size() >= 16) {
                break;
            }
            const QJsonObject info = labelValue.toObject();
            LabelInfo label;
            label.label = json::text(json::object(info, "label"_L1), "name"_L1, 300);
            label.catalogNumber = json::text(info, "catalog-number"_L1, 100);
            if (!label.label.isEmpty() || !label.catalogNumber.isEmpty()) {
                release.labels.append(label);
            }
        }
        list.append(release);
    }
    return list;
}

QList<Genre> genres(const QJsonObject &object)
{
    QList<Genre> list;
    const QJsonArray array = json::array(object, "genres"_L1);
    for (const auto &value : array) {
        const QJsonObject item = value.toObject();
        Genre genre;
        genre.name = json::text(item, "name"_L1, 100);
        genre.count = static_cast<int>(
            std::clamp<qint64>(json::integer(item, "count"_L1).value_or(0), 0, std::numeric_limits<int>::max()));
        if (!genre.name.isEmpty() && genre.count > 0) {
            list.append(genre);
        }
        if (list.size() >= qsizetype{kMaxGenres} * 4) {
            break;
        }
    }
    std::ranges::stable_sort(list, [](const Genre &a, const Genre &b) { return a.count > b.count; });
    if (list.size() > kMaxGenres) {
        list.resize(kMaxGenres);
    }
    return list;
}

namespace {

// A genre's votes at each level, the first level's first: compared lexicographically.
QList<int> votesOf(const QString &name, const QList<QList<Genre>> &levels)
{
    QList<int> votes;
    votes.reserve(levels.size());
    for (const QList<Genre> &level : levels) {
        const auto found = std::ranges::find(level, name, &Genre::name);
        votes.append(found == level.end() ? 0 : found->count);
    }
    return votes;
}

QList<std::pair<QList<int>, QString>> rankedWithVotes(const QList<QList<Genre>> &levels)
{
    QList<std::pair<QList<int>, QString>> ranked;
    if (levels.isEmpty()) {
        return ranked;
    }
    for (const Genre &genre : levels.first()) {
        ranked.append({votesOf(genre.name, levels), genre.name});
    }
    std::ranges::stable_sort(ranked, [](const auto &a, const auto &b) { return a.first > b.first; });
    return ranked;
}

} // namespace

QStringList rankedGenres(const QList<QList<Genre>> &levels)
{
    QStringList names;
    const QList<std::pair<QList<int>, QString>> ranked = rankedWithVotes(levels);
    for (const auto &[votes, name] : ranked) {
        names.append(name);
    }
    return names;
}

bool leadingGenresTied(const QList<QList<Genre>> &levels)
{
    const QList<std::pair<QList<int>, QString>> ranked = rankedWithVotes(levels);
    return ranked.size() >= 2 && ranked.at(0).first == ranked.at(1).first;
}

bool creditsArtist(const Recording &recording, const QStringList &artists)
{
    if (artists.isEmpty()) {
        return true; // nothing to check against
    }
    const QString mainKey = artistKey(artists.first());
    if (mainKey.isEmpty()) {
        return true;
    }
    QString credited;
    for (const QString &artist : recording.artists) {
        const QString key = artistKey(artist);
        if (key == mainKey) {
            return true;
        }
        credited += key;
    }
    return credited.contains(mainKey);
}

QList<Recording> fittingRecordings(const QList<Recording> &candidates, const Release &release)
{
    QList<std::pair<qint64, Recording>> fitting;
    for (const Recording &recording : candidates) {
        if (!creditsArtist(recording, release.artists)) {
            continue;
        }
        qint64 delta = std::numeric_limits<qint64>::max(); // unknown lengths sort last
        if (recording.lengthMs.has_value() && release.durationMs.has_value()) {
            delta = std::abs(*recording.lengthMs - *release.durationMs);
            if (delta > kLengthToleranceMs) {
                continue;
            }
        }
        fitting.append({delta, recording});
    }
    std::ranges::stable_sort(fitting, [](const auto &a, const auto &b) { return a.first < b.first; });
    QList<Recording> result;
    result.reserve(fitting.size());
    for (const auto &[delta, recording] : std::as_const(fitting)) {
        result.append(recording);
    }
    return result;
}

std::optional<ReleaseInfo> chooseRelease(const QList<ReleaseInfo> &candidates, const Release &release, QString *why)
{
    const auto explain = [why](const QString &text) {
        if (why != nullptr) {
            *why = text;
        }
    };
    const bool compilation = wantsCompilation(release);
    if (!release.upc.isEmpty()) {
        for (const ReleaseInfo &candidate : candidates) {
            if (candidate.barcode == release.upc) {
                explain(u"release chosen by barcode (%1)"_s.arg(release.upc));
                return candidate;
            }
        }
    }
    const auto official = [](const ReleaseInfo &candidate) {
        return candidate.status.compare(u"Official"_s, Qt::CaseInsensitive) == 0;
    };
    const QString albumKey = match::Normalizer::albumKey(release.album).key;
    const QString year = release.releaseDate.left(4);
    if (!albumKey.isEmpty()) {
        for (const ReleaseInfo &candidate : candidates) {
            // A bootleg or promo with the same title isn't the release; an unknown status is given the benefit.
            const bool trusted = official(candidate) || candidate.status.isEmpty();
            if (trusted && match::Normalizer::albumKey(candidate.title).key == albumKey &&
                (year.isEmpty() || candidate.date.left(4) == year) && (compilation || !isCompilationOrMix(candidate))) {
                explain(u"release chosen by title and year"_s);
                return candidate;
            }
        }
    }
    if (auto best = earliest(candidates, [&](const ReleaseInfo &candidate) {
            return official(candidate) && typeMatches(candidate, release) &&
                   (compilation || !isCompilationOrMix(candidate));
        })) {
        explain(u"earliest official %1 release"_s.arg(best->primaryType.toLower()));
        return best;
    }
    if (auto best = earliest(candidates, [&](const ReleaseInfo &candidate) {
            return official(candidate) && !isCompilationOrMix(candidate);
        })) {
        explain(u"earliest official release"_s);
        return best;
    }
    if (auto best =
            earliest(candidates, [&](const ReleaseInfo &candidate) { return !isCompilationOrMix(candidate); })) {
        explain(u"earliest release"_s);
        return best;
    }
    // Only compilations or DJ mixes carry it: their label is the compiler's, not the track's.
    explain(candidates.isEmpty() ? u"no release carries the recording"_s
                                 : u"only compilations or DJ mixes carry the recording: no label taken from them"_s);
    return std::nullopt;
}

LabelInfo labelOf(const ReleaseInfo &release)
{
    for (const LabelInfo &info : release.labels) {
        if (info.label.isEmpty() || info.label.compare(u"[no label]"_s, Qt::CaseInsensitive) == 0) {
            continue;
        }
        LabelInfo chosen = info;
        if (chosen.catalogNumber.compare(u"[none]"_s, Qt::CaseInsensitive) == 0) {
            chosen.catalogNumber.clear();
        }
        return chosen;
    }
    return {};
}

} // namespace selecta::meta::detail::musicbrainz
