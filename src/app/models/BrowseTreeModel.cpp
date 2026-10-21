// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/models/BrowseTreeModel.h"

#include "app/Format.h"
#include "app/Icons.h"
#include "app/models/SearchResultsModel.h"

#include <QCollator>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kRoot = 0;

// A browse knows nothing of the peer's slots, queue or speed: those filters can't judge its files.
core::SearchFilter browseFilter(core::SearchFilter filter)
{
    filter.freeSlotOnly = false;
    filter.maxQueue = 0;
    filter.minSpeedKBps = 0;
    return filter;
}

const QCollator &collator()
{
    static const QCollator instance = [] {
        QCollator c;
        c.setNumericMode(true); // "Track 2" before "Track 10", as the Finder sorts
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return instance;
}

} // namespace

BrowseTreeModel::BrowseTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_nodes.append(Node{});
}

void BrowseTreeModel::reset(const QList<core::SearchResult> &results)
{
    beginResetModel();
    m_results = results;
    rebuild();
    endResetModel();
}

void BrowseTreeModel::setFilter(const core::SearchFilter &filter)
{
    if (filter == m_filter) {
        return;
    }
    m_filter = filter;
    // Nothing moves: the rows only change their look.
    Q_EMIT layoutAboutToBeChanged();
    restyle();
    Q_EMIT layoutChanged();
}

void BrowseTreeModel::restyle()
{
    const core::SearchFilter filter = browseFilter(m_filter);
    m_hidden.clear();
    m_hidden.reserve(m_results.size());
    m_counts = {};
    for (const core::SearchResult &result : std::as_const(m_results)) {
        const core::HiddenReason reason = filter.hiddenReason(result, false);
        m_hidden.append(reason);
        m_counts.count(reason);
    }
}

void BrowseTreeModel::rebuild()
{
    m_nodes.clear();
    m_nodes.append(Node{}); // the root
    m_nodeOfResult.clear();
    m_nodeOfResult.reserve(m_results.size());
    m_resultOfPath.clear();
    m_resultOfPath.reserve(m_results.size());
    QHash<std::pair<int, QString>, int> folders; // (parent node, name) → node: linear in the path's length
    static const QRegularExpression separators(u"[\\\\/]"_s);
    for (qsizetype i = 0; i < m_results.size(); ++i) {
        const core::SearchResult &result = m_results.at(i);
        const QStringList parts = result.path.split(separators, Qt::SkipEmptyParts);
        int parent = kRoot;
        for (qsizetype p = 0; p + 1 < parts.size(); ++p) {
            auto found = folders.constFind({parent, parts.at(p)});
            if (found == folders.cend()) {
                Node folder;
                folder.name = parts.at(p);
                folder.parent = parent;
                const auto id = static_cast<int>(m_nodes.size());
                m_nodes.append(folder);
                m_nodes[parent].children.append(id);
                found = folders.insert({parent, parts.at(p)}, id);
            }
            parent = found.value();
        }
        Node file;
        file.name = parts.isEmpty() ? result.path : parts.constLast();
        file.parent = parent;
        file.result = static_cast<int>(i);
        const auto id = static_cast<int>(m_nodes.size());
        m_nodes.append(file);
        m_nodes[parent].children.append(id);
        m_nodeOfResult.append(id);
        m_resultOfPath.insert(result.hit.path, static_cast<int>(i)); // a duplicate path keeps its last file
        // Every folder above it counts it.
        const auto size = static_cast<qint64>(result.hit.size);
        for (int up = parent; up != kRoot; up = m_nodes.at(up).parent) {
            m_nodes[up].size += size;
            ++m_nodes[up].files;
        }
    }
    m_folderCount = static_cast<int>(folders.size());
    restyle();
    sortChildren(kRoot);
}

void BrowseTreeModel::sortChildren(int node)
{
    // Depth first with a stack of its own: a hostile list can nest as deep as its paths are long.
    QList<int> pending{node};
    while (!pending.isEmpty()) {
        const int current = pending.takeLast();
        QList<int> &children = m_nodes[current].children;
        std::stable_sort(children.begin(), children.end(), [this](int a, int b) { return lessThan(a, b); });
        for (qsizetype row = 0; row < children.size(); ++row) {
            Node &child = m_nodes[children.at(row)];
            child.row = static_cast<int>(row);
            if (child.result < 0) {
                pending.append(children.at(row));
            }
        }
    }
}

bool BrowseTreeModel::lessThan(int a, int b) const
{
    const Node &left = m_nodes.at(a);
    const Node &right = m_nodes.at(b);
    const bool leftFolder = left.result < 0;
    const bool rightFolder = right.result < 0;
    if (leftFolder != rightFolder) {
        return leftFolder; // folders first, whichever the order
    }
    const bool ascending = m_sortOrder == Qt::AscendingOrder;
    const auto byName = [&] {
        const int compared = collator().compare(left.name, right.name);
        return ascending ? compared < 0 : compared > 0;
    };
    const auto by = [&](auto key) {
        const auto l = key(left);
        const auto r = key(right);
        if (l == r) {
            return collator().compare(left.name, right.name) < 0; // ties by name, always A to Z
        }
        return ascending ? l < r : l > r;
    };
    if (leftFolder) {
        return m_sortColumn == SizeColumn ? by([](const Node &n) { return n.size; }) : byName();
    }
    switch (m_sortColumn) {
    case StatusColumn:
        return by([this](const Node &n) { return static_cast<int>(m_results.at(n.result).ownership); });
    case QualityColumn:
        return by([this](const Node &n) {
            const core::SearchResult &result = m_results.at(n.result);
            return result.quality.plausible ? result.quality.rank : 1;
        });
    case LengthColumn:
        return by([this](const Node &n) { return m_results.at(n.result).durationSec(); });
    case SizeColumn:
        return by([this](const Node &n) { return m_results.at(n.result).hit.size; });
    default:
        return byName();
    }
}

void BrowseTreeModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= ColumnCount) {
        return;
    }
    Q_EMIT layoutAboutToBeChanged();
    const QModelIndexList before = persistentIndexList();
    QList<int> nodes;
    nodes.reserve(before.size());
    for (const QModelIndex &index : before) {
        nodes.append(static_cast<int>(index.internalId()));
    }
    m_sortColumn = column;
    m_sortOrder = order;
    sortChildren(kRoot);
    QModelIndexList after;
    after.reserve(before.size());
    for (qsizetype i = 0; i < before.size(); ++i) {
        after.append(indexOfNode(nodes.at(i), before.at(i).column()));
    }
    changePersistentIndexList(before, after);
    Q_EMIT layoutChanged();
}

const core::SearchResult *BrowseTreeModel::result(int resultIndex) const
{
    return resultIndex >= 0 && resultIndex < m_results.size() ? &m_results.at(resultIndex) : nullptr;
}

const core::SearchResult *BrowseTreeModel::resultAt(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != this) {
        return nullptr;
    }
    return result(m_nodes.at(static_cast<qsizetype>(index.internalId())).result);
}

core::HiddenReason BrowseTreeModel::hiddenReason(int resultIndex) const
{
    return resultIndex >= 0 && resultIndex < m_hidden.size() ? m_hidden.at(resultIndex) : core::HiddenReason::None;
}

QModelIndex BrowseTreeModel::indexOfNode(int node, int column) const
{
    if (node <= kRoot || node >= m_nodes.size()) {
        return {};
    }
    return createIndex(m_nodes.at(node).row, column, static_cast<quintptr>(node));
}

QString BrowseTreeModel::pathOf(int node) const
{
    QStringList parts;
    for (int up = node; up > kRoot; up = m_nodes.at(up).parent) {
        parts.prepend(m_nodes.at(up).name);
    }
    return parts.join(u'\\');
}

QModelIndex BrowseTreeModel::indexOfResult(int resultIndex, int column) const
{
    return resultIndex >= 0 && resultIndex < m_nodeOfResult.size()
               ? indexOfNode(m_nodeOfResult.at(resultIndex), column)
               : QModelIndex();
}

QModelIndex BrowseTreeModel::indexOfPath(const QByteArray &rawPath, int column) const
{
    const auto found = m_resultOfPath.constFind(rawPath);
    return found == m_resultOfPath.cend() ? QModelIndex() : indexOfResult(found.value(), column);
}

QList<int> BrowseTreeModel::filesIn(const QModelIndex &folder) const
{
    if (!folder.isValid() || folder.model() != this) {
        return {};
    }
    QList<int> files;
    for (const int child : m_nodes.at(static_cast<qsizetype>(folder.internalId())).children) {
        if (const int index = m_nodes.at(child).result; index >= 0) {
            files.append(index);
        }
    }
    return files;
}

QModelIndex BrowseTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column < 0 || column >= ColumnCount || row < 0) {
        return {};
    }
    const int node = parent.isValid() ? static_cast<int>(parent.internalId()) : kRoot;
    if (node < 0 || node >= m_nodes.size()) {
        return {};
    }
    const QList<int> &children = m_nodes.at(node).children;
    return row < children.size() ? createIndex(row, column, static_cast<quintptr>(children.at(row))) : QModelIndex();
}

QModelIndex BrowseTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return {};
    }
    return indexOfNode(m_nodes.at(static_cast<qsizetype>(child.internalId())).parent, 0);
}

int BrowseTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0) {
        return 0;
    }
    const int node = parent.isValid() ? static_cast<int>(parent.internalId()) : kRoot;
    return static_cast<int>(m_nodes.at(node).children.size());
}

int BrowseTreeModel::columnCount(const QModelIndex & /*parent*/) const
{
    return ColumnCount;
}

Qt::ItemFlags BrowseTreeModel::flags(const QModelIndex &index) const
{
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

QVariant BrowseTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this) {
        return {};
    }
    const auto id = static_cast<int>(index.internalId());
    const Node &node = m_nodes.at(id);
    QVariant value = node.result >= 0 ? fileData(m_results.at(node.result), index.column(), role)
                                      : folderData(id, index.column(), role);
    if (role == Qt::ToolTipRole && value.typeId() == QMetaType::QString) {
        return format::plainToolTip(value.toString()); // the peer's names and paths, as they are
    }
    return value;
}

QVariant BrowseTreeModel::fileData(const core::SearchResult &result, int column, int role) const
{
    const core::HiddenReason hidden = hiddenReason(result.index);
    switch (role) {
    case ResultIndexRole:
        return result.index;
    case IsFolderRole:
        return false;
    case HiddenReasonRole:
        return static_cast<int>(hidden);
    case Qt::ForegroundRole:
        if (hidden != core::HiddenReason::None ||
            (m_filter.owned == core::SearchFilter::OwnedResults::Dim && core::isOwned(result.ownership))) {
            return QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text);
        }
        return {};
    case Qt::DecorationRole:
        if (column == NameColumn) {
            return result.isAudio() ? icons::symbol(u"music.note"_s, u"audio-x-generic"_s)
                                    : icons::symbol(u"doc"_s, u"text-x-generic"_s);
        }
        return column == StatusColumn ? QVariant(SearchResultsModel::statusIcon(result.ownership)) : QVariant();
    case Qt::TextAlignmentRole:
        return column == LengthColumn || column == SizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                                              : QVariant();
    case Qt::ToolTipRole: {
        QString tip;
        if (column == NameColumn) {
            tip = result.path;
        } else if (column == StatusColumn && result.ownership != core::Ownership::None) {
            tip = result.ownershipDetail.isEmpty()
                      ? core::ownershipLabel(result.ownership)
                      : core::ownershipLabel(result.ownership) + u": "_s + result.ownershipDetail;
        }
        if (hidden != core::HiddenReason::None) {
            const QString why = tr("Your search filters would hide this file: %1").arg(core::hiddenReasonLabel(hidden));
            tip = tip.isEmpty() ? why : tip + u'\n' + why;
        }
        return tip.isEmpty() ? QVariant() : QVariant(tip);
    }
    case Qt::AccessibleTextRole: {
        const QString text = fileData(result, column, Qt::DisplayRole).toString();
        if (column == NameColumn && hidden != core::HiddenReason::None) {
            return tr("%1, outside your filters").arg(text);
        }
        return text;
    }
    case Qt::DisplayRole:
        break;
    default:
        return {};
    }
    switch (column) {
    case NameColumn:
        return m_nodes.at(m_nodeOfResult.at(result.index)).name;
    case StatusColumn:
        return SearchResultsModel::statusText(result.ownership);
    case QualityColumn:
        return result.isAudio() ? SearchResultsModel::qualityText(result) : QString();
    case LengthColumn:
        return result.durationSec() > 0 ? format::length(qint64{result.durationSec()} * 1000) : QString();
    case SizeColumn:
        return format::size(static_cast<qint64>(result.hit.size));
    default:
        return {};
    }
}

QVariant BrowseTreeModel::folderData(int node, int column, int role) const
{
    const Node &folder = m_nodes.at(node);
    switch (role) {
    case ResultIndexRole:
        return -1;
    case IsFolderRole:
        return true;
    case Qt::DecorationRole:
        return column == NameColumn ? QVariant(icons::symbol(u"folder"_s, u"folder"_s)) : QVariant();
    case Qt::TextAlignmentRole:
        return column == SizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    case Qt::ToolTipRole:
        if (column != NameColumn) {
            return {};
        }
        return folder.files == 1 ? tr("%1\n1 file").arg(pathOf(node))
                                 : tr("%1\n%2 files").arg(pathOf(node)).arg(folder.files);
    case Qt::AccessibleTextRole:
        if (column == NameColumn) {
            return folder.files == 1 ? tr("Folder %1, 1 file").arg(folder.name)
                                     : tr("Folder %1, %2 files").arg(folder.name).arg(folder.files);
        }
        return folderData(node, column, Qt::DisplayRole);
    case Qt::DisplayRole:
        if (column == NameColumn) {
            return folder.name;
        }
        return column == SizeColumn ? QVariant(format::size(folder.size)) : QVariant();
    default:
        return {};
    }
}

QVariant BrowseTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case NameColumn:
        return tr("Name");
    case StatusColumn:
        return tr("Status");
    case QualityColumn:
        return tr("Quality");
    case LengthColumn:
        return tr("Length");
    case SizeColumn:
        return tr("Size");
    default:
        return {};
    }
}

} // namespace selecta::app
