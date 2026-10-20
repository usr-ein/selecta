// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Matcher.h"

#include "match/Duration.h"
#include "match/detail/IndexData.h"
#include "match/detail/Scoring.h"

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace {

// Exact history hits: the same (size, basename) or the same (peer, path) as a past download (note §1, T20/T21).
QList<quint64> historyHits(const detail::IndexData &data, const RemoteFile &file, QStringList *explanation)
{
    QList<quint32> indexes;
    const QString basename = detail::basenameKey(file.path);
    indexes += data.historyBySizeName.values(detail::historySizeNameKey(file.size, basename));
    if (!file.peer.isEmpty()) {
        indexes += data.historyByPeerPath.values(detail::historyPeerPathKey(file.peer, file.path));
    }
    QList<quint64> ids;
    for (const quint32 index : std::as_const(indexes)) {
        const detail::HistoryRecord &record = data.history[index];
        // The keys are exact strings; check the fields anyway so a key collision can never claim a match.
        const bool sameFile = record.size == file.size && record.basenameKey == basename;
        const bool samePath = !file.peer.isEmpty() && record.peer == file.peer && record.path == file.path;
        if ((sameFile || samePath) && !ids.contains(record.id)) {
            ids.append(record.id);
        }
    }
    std::ranges::sort(ids);
    if (explanation != nullptr) {
        explanation->append(
            u"history: %1 past download(s) with the same size and name, or peer and path"_s.arg(ids.size()));
    }
    return ids;
}

} // namespace

Matcher::Matcher(std::shared_ptr<const MatchIndex> index)
    : m_index(std::move(index))
{}

MatchResult Matcher::match(const ParsedRemote &parsed, const RemoteFile &file, MatchOptions options) const
{
    MatchResult result;
    QStringList *explanation = options.explain ? &result.explanation : nullptr;
    if (!m_index) {
        if (explanation != nullptr) {
            explanation->append(u"no library snapshot: nothing can match"_s);
        }
        return result;
    }
    const detail::IndexData &data = m_index->data();
    result.generation = data.generation;
    result.history = historyHits(data, file, explanation);
    if (isMixLength(file.attributes.durationSec)) {
        result.mix = true;
        if (explanation != nullptr) {
            explanation->append(u"longer than 20 minutes: a mix or a set, no track matching"_s);
        }
        return result;
    }
    if (parsed.kind != FileKind::Audio || parsed.unparseable) {
        if (explanation != nullptr) {
            explanation->append(u"no usable title in the name: history only"_s);
        }
        return result;
    }
    const detail::Query query = detail::prepareRemote(parsed, file);
    const QList<quint32> candidates = detail::retrieve(data, query, kMaxCandidates);
    if (explanation != nullptr) {
        explanation->append(
            u"parsed: artist '%1', title '%2', version %3; %4 candidate(s)"_s
                .arg(query.keys.artist.field.key, query.keys.title.field.key,
                     query.keys.title.version.key().isEmpty() ? u"unspecified"_s : query.keys.title.version.key())
                .arg(candidates.size()));
    }
    const detail::BestMatches best = detail::bestOf(data, query, candidates, options.explain, explanation);
    if (best.collection) {
        result.collection = detail::toRecordMatch(data, *best.collection);
    }
    if (best.inbox) {
        result.inbox = detail::toRecordMatch(data, *best.inbox);
    }
    if (best.otherVersion) {
        result.otherVersion = detail::toRecordMatch(data, *best.otherVersion);
    }
    if (explanation != nullptr) {
        for (const RecordMatch *match : {&result.collection, &result.inbox, &result.otherVersion}) {
            if (match->isSet()) {
                explanation->append(match->explanation);
            }
        }
        if (!result.collection.isSet() && !result.inbox.isSet()) {
            explanation->append(u"not owned"_s);
        }
    }
    return result;
}

QList<RecordMatch> Matcher::matchLocal(const LocalTrackQuery &query, qsizetype limit, MatchOptions options) const
{
    QList<RecordMatch> matches;
    if (!m_index || limit <= 0) {
        return matches;
    }
    const detail::IndexData &data = m_index->data();
    const detail::Query prepared = detail::prepareLocal(query);
    if (prepared.title.isEmpty()) {
        return matches;
    }
    QList<detail::Scored> kept;
    const QList<quint32> candidates = detail::retrieve(data, prepared, kMaxCandidates);
    for (const quint32 candidate : candidates) {
        if (query.excludeAlbumId != 0 && data.records[candidate].albumId == query.excludeAlbumId) {
            continue;
        }
        detail::Scored scored = detail::score(data, prepared, candidate, options.explain);
        if (scored.tier != Tier::None || scored.relation == Relation::OtherVersion) {
            kept.append(std::move(scored));
        }
    }
    std::ranges::stable_sort(
        kept, [&data](const detail::Scored &a, const detail::Scored &b) { return detail::better(a, b, data); });
    for (const detail::Scored &scored : std::as_const(kept)) {
        if (matches.size() >= limit) {
            break;
        }
        matches.append(detail::toRecordMatch(data, scored));
    }
    return matches;
}

} // namespace selecta::match
