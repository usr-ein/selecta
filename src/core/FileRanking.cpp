// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/FileRanking.h"

#include "match/Quality.h"
#include "match/QueryPlanner.h"
#include "match/ReleaseScorer.h"
#include "match/RemotePathParser.h"

#include <QHash>
#include <QSet>

#include <map>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

match::ReleaseReference referenceFor(const GetLine &line)
{
    match::ReleaseReference reference;
    if (line.release && line.releaseState != GetLine::ReleaseState::None) {
        reference.artists = line.release->artists;
        reference.title = line.release->title;
        reference.album = line.release->album;
        reference.durationMs = line.release->durationMs.value_or(0);
    } else {
        if (!line.artist.isEmpty()) {
            reference.artists = {line.artist};
        }
        reference.title = line.title;
        reference.durationMs = line.durationHintMs.value_or(0);
    }
    return reference;
}

// The folder part of a virtual path ("@@share\Artist\Album"), with either separator.
QString folderOf(const QString &path)
{
    const qsizetype separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return separator > 0 ? path.left(separator) : QString();
}

QString nameOf(const QString &path)
{
    const qsizetype separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return separator >= 0 ? path.mid(separator + 1) : path;
}

} // namespace

QString FileRanking::query(const GetLine &line, const QStringList &excludedPhrases, QString *why)
{
    const match::QueryPlanner planner(excludedPhrases);
    const match::QueryPlan plan = planner.plan(referenceFor(line), match::QueryStep::Default, line.raw);
    if (!plan.isReady()) {
        if (why) {
            *why =
                plan.status == match::QueryStatus::Blocked
                    ? QStringLiteral("The server excludes \"%1\", so this can't be searched.").arg(plan.blockedPhrase)
                    : QStringLiteral("Nothing selective to search for in this line.");
        }
        return {};
    }
    return plan.text;
}

RankedChoices FileRanking::rank(const GetLine &line, const SearchOutcome &outcome, const SearchFilter &filter,
                                const PeerLoad &peerLoad)
{
    RankedChoices result;
    const match::ReleaseScorer scorer(referenceFor(line));

    // Parse each response folder once, with its siblings (RemotePathParser::parseFolder).
    struct Entry
    {
        const SearchHit *hit = nullptr;
        QString path;
        match::RemoteQuality quality;
    };
    std::map<std::pair<QString, QString>, QList<Entry>> byFolder; // (peer, folder) → audio files
    for (const SearchHit &hit : outcome.hits) {
        const match::FileKind kind = match::RemotePathParser::fileKind(hit.extension);
        if (kind != match::FileKind::Audio) {
            continue;
        }
        ++result.considered;
        SearchResult filterable = filterableResult(hit);
        filterable.parsed.kind = kind; // the hit's own extension decided it above
        if (filter.hiddenReason(filterable, false) != HiddenReason::None) {
            continue; // the release decides fit below: the filter judges quality and the peer
        }
        const Entry entry{.hit = &hit, .path = std::move(filterable.path), .quality = filterable.quality};
        ++result.candidates;
        byFolder[{hit.peer, folderOf(entry.path)}].append(entry);
    }

    QList<match::RankedFile> ranked;
    QList<const Entry *> entries;
    QList<match::ReleaseFit> fits;
    for (const auto &[key, files] : byFolder) {
        QStringList names;
        names.reserve(files.size());
        for (const Entry &entry : files) {
            names.append(nameOf(entry.path));
        }
        const QList<match::ParsedRemote> parsed = match::RemotePathParser::parseFolder(key.second, names);
        for (qsizetype i = 0; i < files.size() && i < parsed.size(); ++i) {
            const Entry &entry = files.at(i);
            const match::ReleaseFit fit =
                scorer.score(parsed.at(i), static_cast<int>(entry.hit->attributes.duration.value_or(0)));
            if (fit.version == match::VersionCompatibility::OtherVersion || fit.fit <= 0.0) {
                continue; // another version, or nothing to do with the release
            }
            ranked.append(match::RankedFile{.fit = fit,
                                            .qualityRank = entry.quality.rank,
                                            .freeSlot = entry.hit->freeSlot,
                                            .queueLength = static_cast<int>(entry.hit->queueLength),
                                            .peer = entry.hit->peer,
                                            .path = entry.path,
                                            .peerLoad = peerLoad ? peerLoad(entry.hit->peer) : 0});
            entries.append(&entry);
            fits.append(fit);
        }
    }

    const QList<qsizetype> order = match::ReleaseScorer::rank(ranked);
    QSet<QString> peersTaken;
    for (const qsizetype index : order) {
        const Entry *entry = entries.at(index);
        const match::ReleaseFit &fit = fits.at(index);
        if (peersTaken.contains(entry->hit->peer)) {
            continue; // one file per peer: alternatives should be other peers (BATCH-7)
        }
        peersTaken.insert(entry->hit->peer);
        FileChoice choice;
        choice.peer = entry->hit->peer;
        choice.remotePath = entry->hit->path;
        choice.size = static_cast<qint64>(entry->hit->size);
        choice.extension = entry->hit->extension;
        if (entry->hit->attributes.bitrate) {
            choice.bitrate = static_cast<int>(*entry->hit->attributes.bitrate);
        }
        if (entry->hit->attributes.duration) {
            choice.durationSec = static_cast<int>(*entry->hit->attributes.duration);
        }
        choice.vbr = entry->hit->attributes.vbr;
        if (entry->hit->attributes.sampleRate) {
            choice.sampleRate = static_cast<int>(*entry->hit->attributes.sampleRate);
        }
        if (entry->hit->attributes.bitDepth) {
            choice.bitDepth = static_cast<int>(*entry->hit->attributes.bitDepth);
        }
        choice.freeSlot = entry->hit->freeSlot;
        choice.queueLength = static_cast<int>(entry->hit->queueLength);
        choice.fit = fit.fit;
        choice.fitsRelease = fit.fits && (fit.version == match::VersionCompatibility::Same ||
                                          fit.version == match::VersionCompatibility::ProbablySame);
        result.files.append(choice);
        if (result.files.size() >= kMaxChoices) {
            break;
        }
    }
    return result;
}

} // namespace selecta::core
