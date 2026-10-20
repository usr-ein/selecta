// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/SearchResults.h"

#include "match/Matcher.h"

#include <QCoreApplication>

#include <map>

using namespace Qt::StringLiterals;

namespace selecta::core {

QString ownershipLabel(Ownership ownership)
{
    switch (ownership) {
    case Ownership::InCollection:
        return QCoreApplication::translate("selecta::core::Ownership", "In collection");
    case Ownership::Upgrade:
        return QCoreApplication::translate("selecta::core::Ownership", "Upgrade");
    case Ownership::LikelyInCollection:
        return QCoreApplication::translate("selecta::core::Ownership", "Likely in collection");
    case Ownership::InInbox:
        return QCoreApplication::translate("selecta::core::Ownership", "In Inbox");
    case Ownership::Queued:
        return QCoreApplication::translate("selecta::core::Ownership", "Queued");
    case Ownership::DownloadedThisSession:
        return QCoreApplication::translate("selecta::core::Ownership", "Downloaded this session");
    case Ownership::DownloadedBefore:
        return QCoreApplication::translate("selecta::core::Ownership", "Downloaded before");
    case Ownership::OtherVersion:
        return QCoreApplication::translate("selecta::core::Ownership", "Other version owned");
    case Ownership::None:
        break;
    }
    return {};
}

bool isOwned(Ownership ownership)
{
    return ownership != Ownership::None && ownership != Ownership::Upgrade && ownership != Ownership::OtherVersion;
}

bool SearchResult::fitsRelease() const
{
    return fit && fit->fits &&
           (fit->version == match::VersionCompatibility::Same ||
            fit->version == match::VersionCompatibility::ProbablySame);
}

QString remoteFolderOf(const QString &path)
{
    const qsizetype separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return separator > 0 ? path.left(separator) : QString();
}

QString remoteNameOf(const QString &path)
{
    const qsizetype separator = std::max(path.lastIndexOf(u'\\'), path.lastIndexOf(u'/'));
    return separator >= 0 ? path.mid(separator + 1) : path;
}

QString decodeRemotePath(const QByteArray &raw)
{
    const QString utf8 = QString::fromUtf8(raw);
    return utf8.contains(QChar::ReplacementCharacter) ? QString::fromLatin1(raw) : utf8;
}

match::RemoteAttributes remoteAttributesOf(const SearchHit &hit)
{
    match::RemoteAttributes attributes;
    attributes.bitrateKbps = static_cast<int>(hit.attributes.bitrate.value_or(0));
    attributes.durationSec = static_cast<int>(hit.attributes.duration.value_or(0));
    attributes.vbr = hit.attributes.vbr;
    attributes.sampleRateHz = static_cast<int>(hit.attributes.sampleRate.value_or(0));
    attributes.bitDepth = static_cast<int>(hit.attributes.bitDepth.value_or(0));
    return attributes;
}

match::ReleaseReference referenceOf(const meta::Release &release)
{
    match::ReleaseReference reference;
    reference.artists = release.artists;
    reference.title = release.title;
    reference.album = release.album;
    reference.durationMs = release.durationMs.value_or(0);
    return reference;
}

FileChoice fileChoiceOf(const SearchResult &result)
{
    FileChoice choice;
    choice.peer = result.hit.peer;
    choice.remotePath = result.hit.path;
    choice.size = static_cast<qint64>(result.hit.size);
    choice.extension = result.hit.extension;
    const slsk::FileAttributes &attributes = result.hit.attributes;
    if (attributes.bitrate) {
        choice.bitrate = static_cast<int>(*attributes.bitrate);
    }
    if (attributes.duration) {
        choice.durationSec = static_cast<int>(*attributes.duration);
    }
    choice.vbr = attributes.vbr;
    if (attributes.sampleRate) {
        choice.sampleRate = static_cast<int>(*attributes.sampleRate);
    }
    if (attributes.bitDepth) {
        choice.bitDepth = static_cast<int>(*attributes.bitDepth);
    }
    choice.freeSlot = result.hit.freeSlot;
    choice.queueLength = static_cast<int>(result.hit.queueLength);
    choice.fit = result.fit ? result.fit->fit : 0.0;
    choice.fitsRelease = result.fitsRelease();
    return choice;
}

SearchResult filterableResult(const SearchHit &hit)
{
    SearchResult result;
    result.hit = hit;
    result.path = decodeRemotePath(hit.path);
    result.folder = remoteFolderOf(result.path);
    result.name = remoteNameOf(result.path);
    result.parsed.kind = match::RemotePathParser::fileKind(hit.extension);
    result.quality = match::assessRemoteQuality(hit.extension, hit.size, remoteAttributesOf(hit));
    return result;
}

QList<SearchResult> analyzeHits(const QList<SearchHit> &hits, int firstIndex,
                                const std::shared_ptr<const match::MatchIndex> &index,
                                const std::optional<match::ReleaseReference> &release)
{
    QList<SearchResult> results;
    results.reserve(hits.size());
    std::map<std::pair<QString, QString>, QList<qsizetype>> byFolder; // (peer, folder) → results
    for (qsizetype i = 0; i < hits.size(); ++i) {
        const SearchHit &hit = hits.at(i);
        SearchResult result;
        result.index = firstIndex + static_cast<int>(i);
        result.hit = hit;
        result.path = decodeRemotePath(hit.path);
        result.folder = remoteFolderOf(result.path);
        result.name = remoteNameOf(result.path);
        result.quality = match::assessRemoteQuality(hit.extension, hit.size, remoteAttributesOf(hit));
        byFolder[{hit.peer, result.folder}].append(i);
        results.append(std::move(result));
    }

    // Each folder once, with its siblings: they confirm the naming pattern and the track numbers.
    match::ParseContext context;
    if (index) {
        context.knownArtists = &index->artists();
    }
    for (const auto &[key, members] : byFolder) {
        QStringList names;
        names.reserve(members.size());
        for (const qsizetype member : members) {
            names.append(results.at(member).name);
        }
        const QList<match::ParsedRemote> parsed = match::RemotePathParser::parseFolder(key.second, names, context);
        for (qsizetype i = 0; i < members.size() && i < parsed.size(); ++i) {
            results[members.at(i)].parsed = parsed.at(i);
        }
    }

    scoreResults(results, release);
    matchResults(results, index);
    return results;
}

void scoreResults(QList<SearchResult> &results, const std::optional<match::ReleaseReference> &release)
{
    if (!release) {
        for (SearchResult &result : results) {
            result.fit.reset();
        }
        return;
    }
    const match::ReleaseScorer scorer(*release);
    for (SearchResult &result : results) {
        if (result.isAudio()) {
            result.fit = scorer.score(result.parsed, result.durationSec());
        } else {
            result.fit.reset();
        }
    }
}

void matchResults(QList<SearchResult> &results, const std::shared_ptr<const match::MatchIndex> &index)
{
    const match::Matcher matcher(index);
    for (SearchResult &result : results) {
        result.ownership = Ownership::None;
        result.ownedId = 0;
        result.historyIds.clear();
        result.ownershipDetail.clear();
        if (!result.isAudio() || !index) {
            continue;
        }
        const match::RemoteFile file{.peer = result.hit.peer,
                                     .path = result.path,
                                     .size = result.hit.size,
                                     .attributes = remoteAttributesOf(result.hit)};
        const match::MatchResult found = matcher.match(result.parsed, file);
        result.historyIds = found.history;
        if (found.collection.isSet()) {
            result.ownedId = found.collection.id;
            result.ownedSource = match::Source::Collection;
            // Only a sure match claims more than "likely": an unclear version may be another track to a DJ.
            if (found.collection.tier != match::Tier::Exact) {
                result.ownership = Ownership::LikelyInCollection;
            } else if (found.collection.relation == match::Relation::Upgrade) {
                result.ownership = Ownership::Upgrade;
            } else {
                result.ownership = Ownership::InCollection;
            }
        } else if (found.inbox.isSet()) {
            result.ownedId = found.inbox.id;
            result.ownedSource = match::Source::Inbox;
            result.ownership = Ownership::InInbox;
        } else if (!found.history.isEmpty()) {
            result.ownership = Ownership::DownloadedBefore; // the engine tells this session's downloads apart
        } else if (found.otherVersion.isSet()) {
            result.ownedId = found.otherVersion.id;
            result.ownedSource = found.otherVersion.source;
            result.ownership = Ownership::OtherVersion;
        }
    }
}

} // namespace selecta::core
