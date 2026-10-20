// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/FileRanking.h"

#include "match/Quality.h"
#include "match/QueryPlanner.h"
#include "match/ReleaseScorer.h"
#include "match/RemotePathParser.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>

#include <map>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

QString translated(const char *text)
{
    return QCoreApplication::translate("selecta::core::FileRanking", text);
}

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

// An audio file of the results, with what the filter said of it.
struct Entry
{
    const SearchHit *hit = nullptr;
    QString path;
    match::RemoteQuality quality;
    QString shortfall; // empty when the filter took it
};

using Folders = std::map<std::pair<QString, QString>, QList<Entry>>; // (peer, folder) → audio files

FileChoice choiceOf(const Entry &entry, const match::ReleaseFit &fit)
{
    const SearchHit &hit = *entry.hit;
    FileChoice choice;
    choice.peer = hit.peer;
    choice.remotePath = hit.path;
    choice.size = static_cast<qint64>(hit.size);
    choice.extension = hit.extension;
    if (hit.attributes.bitrate) {
        choice.bitrate = static_cast<int>(*hit.attributes.bitrate);
    }
    if (hit.attributes.duration) {
        choice.durationSec = static_cast<int>(*hit.attributes.duration);
    }
    choice.vbr = hit.attributes.vbr;
    if (hit.attributes.sampleRate) {
        choice.sampleRate = static_cast<int>(*hit.attributes.sampleRate);
    }
    if (hit.attributes.bitDepth) {
        choice.bitDepth = static_cast<int>(*hit.attributes.bitDepth);
    }
    choice.freeSlot = hit.freeSlot;
    choice.queueLength = static_cast<int>(hit.queueLength);
    choice.fit = fit.fit;
    choice.fitsRelease = fit.fits && (fit.version == match::VersionCompatibility::Same ||
                                      fit.version == match::VersionCompatibility::ProbablySame);
    choice.shortfall = entry.shortfall;
    return choice;
}

// Which scored files a ranking keeps.
enum class Keep {
    Related,  // anything of the release, to check if it doesn't fit: the files that passed the preset
    FitsOnly, // only files that fit the release (length, name and version agree): near misses, "of this track"
};

// Each folder's files parsed once, with their siblings (RemotePathParser::parseFolder), and scored against the
// release; another version, or a file with nothing of the release, drops out (with Keep::FitsOnly, anything that
// doesn't fit it). Then "Best match" order (ReleaseScorer::rank), keeping each peer's best file, at most kMaxChoices.
// `kept` counts the files kept, every peer's.
QList<FileChoice> choose(const Folders &folders, const match::ReleaseScorer &scorer,
                         const FileRanking::PeerLoad &peerLoad, Keep keep, int *kept = nullptr)
{
    QList<match::RankedFile> ranked;
    QList<const Entry *> entries;
    QList<match::ReleaseFit> fits;
    for (const auto &[key, files] : folders) {
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
            const bool sameVersion = fit.version == match::VersionCompatibility::Same ||
                                     fit.version == match::VersionCompatibility::ProbablySame;
            if (keep == Keep::FitsOnly && !(fit.fits && sameVersion)) {
                continue; // perhaps the track, perhaps not: below the quality wanted too, it isn't worth a look
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
    if (kept != nullptr) {
        *kept = static_cast<int>(ranked.size());
    }

    QList<FileChoice> chosen;
    const QList<qsizetype> order = match::ReleaseScorer::rank(ranked);
    QSet<QString> peersTaken;
    for (const qsizetype index : order) {
        const Entry *entry = entries.at(index);
        if (peersTaken.contains(entry->hit->peer)) {
            continue; // one file per peer: alternatives should be other peers (BATCH-7)
        }
        peersTaken.insert(entry->hit->peer);
        chosen.append(choiceOf(*entry, fits.at(index)));
        if (chosen.size() >= FileRanking::kMaxChoices) {
            break;
        }
    }
    return chosen;
}

// The lossy rule's comparison (SearchFilter, SRCH-3).
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

QString bitrateShortfall(const SearchFilter &filter, const match::AudioQuality &quality)
{
    if (bitrateFits(filter.bitrateRule, quality.bitrateKbps, filter.bitrateKbps)) {
        return translated("VBR, not CBR"); // the rate fits: the CBR rule turned it down
    }
    switch (filter.bitrateRule) {
    case SearchFilter::BitrateRule::AtLeast:
        return translated("under %1 kbps").arg(filter.bitrateKbps);
    case SearchFilter::BitrateRule::AtMost:
        return translated("over %1 kbps").arg(filter.bitrateKbps);
    case SearchFilter::BitrateRule::Equal:
        return translated("not %1 kbps").arg(filter.bitrateKbps);
    case SearchFilter::BitrateRule::Any:
        break;
    }
    return translated("not the bitrate wanted");
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

QString FileRanking::shortfall(HiddenReason reason, const SearchFilter &filter, const SearchResult &result)
{
    const match::AudioQuality &quality = result.quality.quality;
    switch (reason) {
    case HiddenReason::Format:
        return translated("%1 isn't a format wanted").arg(searchFormatName(searchFormatOf(quality.codec)));
    case HiddenReason::Bitrate:
        return bitrateShortfall(filter, quality);
    case HiddenReason::Resolution:
        if (filter.minBitDepth > 0 && quality.bitDepth > 0 && quality.bitDepth < filter.minBitDepth) {
            return translated("under %1-bit").arg(filter.minBitDepth);
        }
        return translated("a sample rate not wanted");
    case HiddenReason::UnknownQuality:
        return translated("its quality isn't stated");
    case HiddenReason::Implausible:
        return translated("its size doesn't match the quality it states");
    case HiddenReason::Length:
        return filter.minDurationSec > 0 && result.durationSec() < filter.minDurationSec ? translated("too short")
                                                                                         : translated("too long");
    case HiddenReason::Size:
        return filter.minSizeBytes > 0 && std::cmp_less(result.hit.size, filter.minSizeBytes) ? translated("too small")
                                                                                              : translated("too big");
    case HiddenReason::NoFreeSlot:
        return translated("no free slot");
    case HiddenReason::QueueTooLong:
        return translated("a queue over %1").arg(filter.maxQueue);
    case HiddenReason::TooSlow:
        return translated("slower than %1 KB/s").arg(filter.minSpeedKBps);
    case HiddenReason::ExcludedWord:
        return translated("an excluded word in its name");
    case HiddenReason::Owned:
        return translated("you have it already");
    case HiddenReason::None:
    case HiddenReason::NotAudio:
    case HiddenReason::NotThisRelease:
        break;
    }
    return {};
}

RankedChoices FileRanking::rank(const GetLine &line, const SearchOutcome &outcome, const SearchFilter &filter,
                                const PeerLoad &peerLoad)
{
    RankedChoices result;
    const match::ReleaseScorer scorer(referenceFor(line));

    Folders passed; // what the filter takes
    Folders missed; // and what it turns down: near misses, if nothing passed
    for (const SearchHit &hit : outcome.hits) {
        const match::FileKind kind = match::RemotePathParser::fileKind(hit.extension);
        if (kind != match::FileKind::Audio) {
            continue;
        }
        ++result.considered;
        SearchResult filterable = filterableResult(hit);
        filterable.parsed.kind = kind; // the hit's own extension decided it above
        // The release decides fit below: the filter judges quality and the peer.
        const HiddenReason reason = filter.hiddenReason(filterable, false);
        QString why = shortfall(reason, filter, filterable);
        Entry entry{.hit = &hit,
                    .path = std::move(filterable.path),
                    .quality = filterable.quality,
                    .shortfall = std::move(why)};
        if (reason == HiddenReason::None) {
            ++result.candidates;
            passed[{hit.peer, folderOf(entry.path)}].append(std::move(entry));
        } else {
            if (entry.shortfall.isEmpty()) {
                entry.shortfall = hiddenReasonLabel(reason); // a reason without words of its own
            }
            missed[{hit.peer, folderOf(entry.path)}].append(std::move(entry));
        }
    }

    result.files = choose(passed, scorer, peerLoad, Keep::Related);
    if (result.files.isEmpty()) {
        result.nearMisses = choose(missed, scorer, peerLoad, Keep::FitsOnly, &result.belowQuality);
    }
    return result;
}

} // namespace selecta::core
