// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/SearchResultsModel.h"

#include "app/Format.h"
#include "app/Icons.h"

#include <QFont>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>

#include <algorithm>
#include <cstdlib>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kSameRecordingSeconds = 3; // ±3 s is the same recording (SRCH-18)

// Quality rank for ordering: a file whose size contradicts its declared quality ranks lowest (SRCH-7).
int effectiveRank(const core::SearchResult &result)
{
    return result.quality.plausible ? result.quality.rank : 1;
}

QIcon ownershipIcon(core::Ownership ownership)
{
    switch (ownership) {
    case core::Ownership::InCollection:
        return icons::symbol(u"checkmark.circle"_s, u"emblem-default"_s);
    case core::Ownership::Upgrade:
        return icons::symbol(u"arrow.up.circle"_s, u"go-up"_s);
    case core::Ownership::LikelyInCollection:
        return icons::symbol(u"questionmark.circle"_s, u"dialog-question"_s);
    case core::Ownership::InInbox:
        return icons::symbol(u"tray"_s, u"mail-folder-inbox"_s);
    case core::Ownership::Queued:
        return icons::symbol(u"hourglass"_s, u"appointment-soon"_s);
    case core::Ownership::DownloadedThisSession:
    case core::Ownership::DownloadedBefore:
        return icons::symbol(u"clock.arrow.circlepath"_s, u"document-open-recent"_s);
    case core::Ownership::OtherVersion:
        return icons::symbol(u"square.on.square"_s, u"edit-copy"_s);
    case core::Ownership::None:
        break;
    }
    return {};
}

QString lastComponents(const QString &folder, int count)
{
    QStringList parts = folder.split(QRegularExpression(u"[\\\\/]"_s), Qt::SkipEmptyParts);
    while (parts.size() > count) {
        parts.removeFirst();
    }
    return parts.join(u" › "_s);
}

QString availabilityText(const core::SearchHit &hit)
{
    const QString speed = format::speed(hit.averageSpeed);
    QString slot =
        hit.freeSlot ? SearchResultsModel::tr("Free") : SearchResultsModel::tr("Queue %1").arg(hit.queueLength);
    return speed.isEmpty() ? slot : slot + u" · "_s + speed;
}

} // namespace

SearchResultsModel::SearchResultsModel(QObject *parent)
    : QAbstractItemModel(parent)
{}

QString SearchResultsModel::statusText(core::Ownership ownership)
{
    // Short enough for the column; the tooltip and the inspector have the whole story.
    switch (ownership) {
    case core::Ownership::InCollection:
        return tr("In collection");
    case core::Ownership::Upgrade:
        return tr("Upgrade");
    case core::Ownership::LikelyInCollection:
        return tr("Likely owned");
    case core::Ownership::InInbox:
        return tr("In Inbox");
    case core::Ownership::Queued:
        return tr("Queued");
    case core::Ownership::DownloadedThisSession:
        return tr("This session");
    case core::Ownership::DownloadedBefore:
        return tr("Downloaded");
    case core::Ownership::OtherVersion:
        return tr("Other version");
    case core::Ownership::None:
        break;
    }
    return {};
}

QString SearchResultsModel::titleText(const core::SearchResult &result)
{
    const match::ParsedRemote &parsed = result.parsed;
    if (parsed.title.value.isEmpty()) {
        return result.name;
    }
    return parsed.artist.value.isEmpty() ? parsed.title.value : parsed.artist.value + u" — "_s + parsed.title.value;
}

QString SearchResultsModel::qualityText(const core::SearchResult &result)
{
    QString text = format::quality(core::fileChoiceOf(result));
    const match::RemoteQuality &quality = result.quality;
    const bool declared = quality.quality.bitrateKbps > 0 || quality.quality.bitDepth > 0;
    if (!declared && quality.estimatedBitrateKbps > 0) {
        text += u" ≈"_s + QString::number(quality.estimatedBitrateKbps); // estimated, and labelled so (SRCH-6)
    }
    if (!quality.plausible) {
        text.prepend(u"⚠ "_s);
    }
    return text;
}

void SearchResultsModel::reset(const QList<core::SearchResult> &results)
{
    m_results = results;
    rebuild();
}

void SearchResultsModel::append(const QList<core::SearchResult> &results)
{
    for (const core::SearchResult &result : results) {
        if (result.index == m_results.size()) {
            m_results.append(result);
        } else if (result.index >= 0 && result.index < m_results.size()) {
            m_results[result.index] = result;
        }
    }
    m_stale = !results.isEmpty() || m_stale;
}

void SearchResultsModel::update(const QList<core::SearchResult> &results)
{
    for (const core::SearchResult &result : results) {
        if (result.index >= 0 && result.index < m_results.size()) {
            m_results[result.index] = result;
        }
    }
    rebuild();
}

void SearchResultsModel::refresh()
{
    if (m_stale) {
        rebuild();
    }
}

void SearchResultsModel::setFilter(const core::SearchFilter &filter)
{
    if (filter == m_filter) {
        return;
    }
    m_filter = filter;
    rebuild();
}

void SearchResultsModel::setReleaseChosen(bool chosen, qint64 releaseDurationMs)
{
    if (chosen == m_releaseChosen && releaseDurationMs == m_releaseDurationMs) {
        return;
    }
    m_releaseChosen = chosen;
    m_releaseDurationMs = chosen ? releaseDurationMs : 0;
    rebuild();
}

void SearchResultsModel::setGrouped(bool grouped)
{
    if (grouped == m_grouped) {
        return;
    }
    m_grouped = grouped;
    rebuild();
}

void SearchResultsModel::sort(int column, Qt::SortOrder order)
{
    m_sortColumn = column >= 0 && column < ColumnCount ? column : kBestMatch;
    m_sortOrder = order;
    rebuild();
}

namespace {

// "Best match" (SRCH-18): fit to the release first (length, then names and version), then quality, then
// availability, then the peer we took least from. nullopt when the two are equal on all of it.
std::optional<bool> bestMatchBefore(const core::SearchResult &x, const core::SearchResult &y, bool releaseChosen)
{
    if (releaseChosen) {
        if (x.fitsRelease() != y.fitsRelease()) {
            return x.fitsRelease();
        }
        if (x.fit && y.fit) {
            if (const int identity = match::ReleaseScorer::compareIdentity(*x.fit, *y.fit); identity != 0) {
                return identity < 0;
            }
        }
    }
    if (effectiveRank(x) != effectiveRank(y)) {
        return effectiveRank(x) > effectiveRank(y);
    }
    if (x.hit.freeSlot != y.hit.freeSlot) {
        return x.hit.freeSlot;
    }
    if (x.hit.queueLength != y.hit.queueLength) {
        return x.hit.queueLength < y.hit.queueLength;
    }
    if (x.peerFilesToday != y.peerFilesToday) {
        return x.peerFilesToday < y.peerFilesToday; // spread the load over peers (FAIR-5)
    }
    return std::nullopt;
}

std::optional<bool> availabilityBefore(const core::SearchHit &x, const core::SearchHit &y)
{
    if (x.freeSlot != y.freeSlot) {
        return x.freeSlot;
    }
    if (x.queueLength != y.queueLength) {
        return x.queueLength < y.queueLength;
    }
    if (x.averageSpeed != y.averageSpeed) {
        return x.averageSpeed > y.averageSpeed;
    }
    return std::nullopt;
}

// A column's ascending order; nullopt for a tie.
std::optional<bool> columnBefore(int column, const core::SearchResult &x, const core::SearchResult &y)
{
    switch (column) {
    case SearchResultsModel::StatusColumn:
        if (x.ownership == y.ownership) {
            return std::nullopt;
        }
        // Something owned first, in badge order; nothing known last.
        if ((x.ownership == core::Ownership::None) != (y.ownership == core::Ownership::None)) {
            return y.ownership == core::Ownership::None;
        }
        return x.ownership < y.ownership;
    case SearchResultsModel::TitleColumn:
        // Files of one folder in track order, as the album plays; anything else by name.
        if (x.folder == y.folder && x.hit.peer == y.hit.peer && x.parsed.track.isSet() && y.parsed.track.isSet()) {
            const int xDisc = x.parsed.disc.isSet() ? x.parsed.disc.value : 0;
            const int yDisc = y.parsed.disc.isSet() ? y.parsed.disc.value : 0;
            if (xDisc != yDisc || x.parsed.track.value != y.parsed.track.value) {
                return xDisc != yDisc ? xDisc < yDisc : x.parsed.track.value < y.parsed.track.value;
            }
        }
        if (const int order =
                QString::localeAwareCompare(SearchResultsModel::titleText(x), SearchResultsModel::titleText(y));
            order != 0) {
            return order < 0;
        }
        return std::nullopt;
    case SearchResultsModel::QualityColumn:
        if (effectiveRank(x) != effectiveRank(y)) {
            return effectiveRank(x) > effectiveRank(y);
        }
        if (x.quality.quality.bitrateKbps != y.quality.quality.bitrateKbps) {
            return x.quality.quality.bitrateKbps > y.quality.quality.bitrateKbps;
        }
        return std::nullopt;
    case SearchResultsModel::LengthColumn:
        return x.durationSec() == y.durationSec() ? std::nullopt
                                                  : std::optional<bool>(x.durationSec() < y.durationSec());
    case SearchResultsModel::PeerColumn:
        if (x.peerFilesToday != y.peerFilesToday) {
            return x.peerFilesToday < y.peerFilesToday; // fewest downloads from that peer first (SRCH-10)
        }
        if (const int order = QString::compare(x.hit.peer, y.hit.peer, Qt::CaseInsensitive); order != 0) {
            return order < 0;
        }
        return std::nullopt;
    case SearchResultsModel::AvailabilityColumn:
        return availabilityBefore(x.hit, y.hit);
    default:
        return std::nullopt;
    }
}

} // namespace

bool SearchResultsModel::lessThan(int a, int b) const
{
    const core::SearchResult &x = m_results.at(a);
    const core::SearchResult &y = m_results.at(b);
    if (m_sortColumn != kBestMatch) {
        if (const std::optional<bool> before = columnBefore(m_sortColumn, x, y)) {
            return m_sortOrder == Qt::AscendingOrder ? *before : !*before;
        }
    }
    // Best match, whatever the order; ties of a column too. Then arrival.
    return bestMatchBefore(x, y, m_releaseChosen).value_or(a < b);
}

void SearchResultsModel::rebuild()
{
    beginResetModel();
    m_stale = false;
    m_counts = {};
    m_shown.clear();
    m_folders.clear();
    for (const core::SearchResult &result : std::as_const(m_results)) {
        const core::HiddenReason reason = m_filter.hiddenReason(result, m_releaseChosen);
        m_counts.count(reason);
        if (reason == core::HiddenReason::None) {
            m_shown.append(result.index);
        }
    }
    std::ranges::stable_sort(m_shown, [this](int a, int b) { return lessThan(a, b); });
    if (m_grouped) {
        // Folders in the order of their best file; files in display order inside.
        QHash<QString, qsizetype> folderOf;
        for (const int index : std::as_const(m_shown)) {
            const core::SearchResult &result = m_results.at(index);
            const QString key = result.hit.peer + u'\n' + result.folder;
            auto found = folderOf.find(key);
            if (found == folderOf.end()) {
                found = folderOf.insert(key, m_folders.size());
                m_folders.append(Folder{.peer = result.hit.peer, .path = result.folder, .members = {}, .size = 0});
            }
            Folder &folder = m_folders[*found];
            folder.members.append(index);
            folder.size += static_cast<qint64>(result.hit.size);
        }
    }
    endResetModel();
}

const core::SearchResult *SearchResultsModel::result(int resultIndex) const
{
    return resultIndex >= 0 && resultIndex < m_results.size() ? &m_results.at(resultIndex) : nullptr;
}

const core::SearchResult *SearchResultsModel::resultAt(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != this) {
        return nullptr;
    }
    if (!m_grouped) {
        return index.row() < m_shown.size() ? result(m_shown.at(index.row())) : nullptr;
    }
    if (index.internalId() == 0) {
        return nullptr; // a folder row
    }
    const auto folder = static_cast<qsizetype>(index.internalId() - 1);
    if (folder >= m_folders.size() || index.row() >= m_folders.at(folder).members.size()) {
        return nullptr;
    }
    return result(m_folders.at(folder).members.at(index.row()));
}

QModelIndex SearchResultsModel::indexOfResult(int resultIndex, int column) const
{
    if (!m_grouped) {
        const qsizetype row = m_shown.indexOf(resultIndex);
        return row < 0 ? QModelIndex() : index(static_cast<int>(row), column);
    }
    for (qsizetype folder = 0; folder < m_folders.size(); ++folder) {
        const qsizetype row = m_folders.at(folder).members.indexOf(resultIndex);
        if (row >= 0) {
            return index(static_cast<int>(row), column, index(static_cast<int>(folder), 0));
        }
    }
    return {};
}

QList<int> SearchResultsModel::resultsUnder(const QModelIndex &folder) const
{
    if (!m_grouped || !folder.isValid() || folder.internalId() != 0 || folder.row() >= m_folders.size()) {
        return {};
    }
    return m_folders.at(folder.row()).members;
}

QModelIndex SearchResultsModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount) {
        return {};
    }
    if (!parent.isValid()) {
        return row < (m_grouped ? m_folders.size() : m_shown.size()) ? createIndex(row, column, quintptr{0})
                                                                     : QModelIndex();
    }
    if (!m_grouped || parent.internalId() != 0 || parent.row() >= m_folders.size() ||
        row >= m_folders.at(parent.row()).members.size()) {
        return {};
    }
    return createIndex(row, column, static_cast<quintptr>(parent.row()) + 1);
}

QModelIndex SearchResultsModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0) {
        return {};
    }
    return createIndex(static_cast<int>(child.internalId() - 1), 0, quintptr{0});
}

int SearchResultsModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return static_cast<int>(m_grouped ? m_folders.size() : m_shown.size());
    }
    if (m_grouped && parent.column() == 0 && parent.internalId() == 0 && parent.row() < m_folders.size()) {
        return static_cast<int>(m_folders.at(parent.row()).members.size());
    }
    return 0;
}

int SearchResultsModel::columnCount(const QModelIndex & /*parent*/) const
{
    return ColumnCount;
}

Qt::ItemFlags SearchResultsModel::flags(const QModelIndex &index) const
{
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

QVariant SearchResultsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    if (const core::SearchResult *found = resultAt(index)) {
        return resultData(*found, index.column(), role);
    }
    if (m_grouped && index.internalId() == 0 && index.row() < m_folders.size()) {
        return folderData(m_folders.at(index.row()), index.column(), role);
    }
    return {};
}

QVariant SearchResultsModel::resultData(const core::SearchResult &result, int column, int role) const
{
    switch (role) {
    case ResultIndexRole:
        return result.index;
    case IsFolderRole:
        return false;
    case Qt::ForegroundRole:
        if (m_filter.owned == core::SearchFilter::OwnedResults::Dim && core::isOwned(result.ownership)) {
            return QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text);
        }
        return {};
    case Qt::DecorationRole:
        return column == StatusColumn ? QVariant(ownershipIcon(result.ownership)) : QVariant();
    case Qt::TextAlignmentRole:
        return column == LengthColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    case Qt::ToolTipRole:
        switch (column) {
        case StatusColumn:
            if (result.ownership == core::Ownership::None) {
                return {};
            }
            return result.ownershipDetail.isEmpty()
                       ? core::ownershipLabel(result.ownership)
                       : core::ownershipLabel(result.ownership) + u": "_s + result.ownershipDetail;
        case TitleColumn:
            return result.path;
        case QualityColumn:
            if (!result.quality.plausible) {
                return tr("The size suggests about %1 kbps, not what the peer says")
                    .arg(result.quality.estimatedBitrateKbps);
            }
            if (!result.quality.established) {
                return tr("The peer didn't say what quality this is; ≈ is estimated from its size and length");
            }
            return {};
        case PeerColumn:
            if (result.peerBackoffMs > 0) {
                return tr("Selecta is holding off %1 after a refusal").arg(result.hit.peer);
            }
            return result.peerFilesToday == 1
                       ? tr("1 download from %1 today").arg(result.hit.peer)
                       : tr("%1 downloads from %2 today").arg(result.peerFilesToday).arg(result.hit.peer);
        default:
            return {};
        }
    case Qt::AccessibleTextRole:
        return accessibleText(result, column);
    case Qt::AccessibleDescriptionRole:
        return resultData(result, column, Qt::ToolTipRole);
    case Qt::DisplayRole:
        break;
    default:
        return {};
    }
    switch (column) {
    case StatusColumn:
        return statusText(result.ownership);
    case TitleColumn: {
        const QString title = titleText(result);
        if (m_grouped && result.parsed.track.isSet()) {
            return u"%1  %2"_s.arg(result.parsed.track.value, 2, 10, u'0').arg(title);
        }
        return title;
    }
    case QualityColumn:
        return qualityText(result);
    case LengthColumn: {
        QString text = format::length(qint64{result.durationSec()} * 1000);
        if (m_releaseChosen && m_releaseDurationMs > 0 && result.durationSec() > 0) {
            // ±3 s is the same recording; beyond, say by how much (SRCH-18).
            const qint64 delta = result.durationSec() - ((m_releaseDurationMs + 500) / 1000);
            text += std::abs(delta) <= kSameRecordingSeconds ? u" ✓"_s : u" · "_s + format::lengthDelta(delta);
        }
        return text;
    }
    case PeerColumn:
        if (result.peerFilesToday > 0) {
            return u"%1 · %2"_s.arg(result.hit.peer).arg(result.peerFilesToday);
        }
        return result.hit.peer;
    case AvailabilityColumn:
        return availabilityText(result.hit);
    default:
        return {};
    }
}

QString SearchResultsModel::accessibleText(const core::SearchResult &result, int column) const
{
    switch (column) {
    case QualityColumn: {
        QString text = format::quality(core::fileChoiceOf(result));
        const match::RemoteQuality &quality = result.quality;
        if (quality.quality.bitrateKbps <= 0 && quality.quality.bitDepth <= 0 && quality.estimatedBitrateKbps > 0) {
            text += tr(", about %1 kbps").arg(quality.estimatedBitrateKbps);
        }
        return quality.plausible ? text : tr("%1, doubtful").arg(text);
    }
    case LengthColumn: {
        const QString length = format::length(qint64{result.durationSec()} * 1000);
        if (!m_releaseChosen || m_releaseDurationMs <= 0 || result.durationSec() <= 0) {
            return length;
        }
        const qint64 delta = result.durationSec() - ((m_releaseDurationMs + 500) / 1000);
        return std::abs(delta) <= kSameRecordingSeconds
                   ? tr("%1, same length as the release").arg(length)
                   : tr("%1, %2 against the release").arg(length, format::lengthDelta(delta));
    }
    case PeerColumn:
        if (result.peerFilesToday > 0) {
            return result.peerFilesToday == 1
                       ? tr("%1, 1 download today").arg(result.hit.peer)
                       : tr("%1, %2 downloads today").arg(result.hit.peer).arg(result.peerFilesToday);
        }
        return result.hit.peer;
    default:
        return resultData(result, column, Qt::DisplayRole).toString();
    }
}

QVariant SearchResultsModel::folderData(const Folder &folder, int column, int role) const
{
    switch (role) {
    case ResultIndexRole:
        return -1;
    case IsFolderRole:
        return true;
    case Qt::FontRole: {
        QFont font = QGuiApplication::font();
        font.setWeight(QFont::DemiBold);
        return font;
    }
    case Qt::ToolTipRole:
        return column == TitleColumn ? folder.path : QVariant();
    case Qt::DisplayRole:
        break;
    default:
        return {};
    }
    switch (column) {
    case TitleColumn: {
        const QString files = folder.members.size() == 1 ? tr("1 file") : tr("%1 files").arg(folder.members.size());
        return u"%1  ·  %2, %3"_s.arg(lastComponents(folder.path, 2), files, format::size(folder.size));
    }
    case QualityColumn: {
        QString first;
        for (const int member : folder.members) {
            const QString quality = format::quality(core::fileChoiceOf(m_results.at(member)));
            if (first.isEmpty()) {
                first = quality;
            } else if (quality != first) {
                return tr("Mixed");
            }
        }
        return first;
    }
    case PeerColumn:
        return folder.peer;
    case AvailabilityColumn:
        return folder.members.isEmpty() ? QVariant() : availabilityText(m_results.at(folder.members.first()).hit);
    default:
        return {};
    }
}

QVariant SearchResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case StatusColumn:
        return tr("Status");
    case TitleColumn:
        return tr("Title");
    case QualityColumn:
        return tr("Quality");
    case LengthColumn:
        return tr("Length");
    case PeerColumn:
        return tr("Peer · today");
    case AvailabilityColumn:
        return tr("Availability");
    default:
        return {};
    }
}

} // namespace selecta::app
