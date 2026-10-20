// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/SearchView.h"

#include "app/Format.h"
#include "app/models/SearchResultsModel.h"
#include "app/views/SearchInspector.h"
#include "core/EngineHost.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStackedWidget>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace selecta::app {
namespace {

constexpr auto kRefreshPace = 250ms;
constexpr int kReasonsShown = 4;
const std::array kBitrates{128, 160, 192, 224, 256, 320};
const std::array kSampleRates{44'100, 48'000, 88'200, 96'000, 176'400, 192'000};
const std::array kQueues{0, 5, 10, 20, 50};

QLabel *fieldLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

QTime timeOfSeconds(int seconds)
{
    return QTime(0, 0).addSecs(std::clamp(seconds, 0, 86'399));
}

int secondsOf(QTime time)
{
    return QTime(0, 0).secsTo(time);
}

QString candidateText(const meta::Candidate &candidate)
{
    const meta::Release &release = candidate.release;
    QStringList parts{format::release(release)};
    if (!release.album.isEmpty() && release.album != release.title) {
        parts.append(release.album);
    }
    if (release.releaseDate.size() >= 4) {
        parts.append(release.releaseDate.left(4));
    }
    return parts.join(u" · "_s);
}

} // namespace

SearchView::SearchView(core::EngineHost &engine, AppSettings &settings, QWidget *parent)
    : QWidget(parent)
    , m_engine(engine)
    , m_settings(settings)
    , m_state(settings.searchViewState())
    , m_model(new SearchResultsModel(this))
    , m_inspector(new SearchInspector)
    , m_pages(new QStackedWidget(this))
    , m_preset(new QComboBox(this))
    , m_onlyRelease(new QCheckBox(tr("Only this release"), this))
    , m_grouped(new QCheckBox(tr("Group by folder"), this))
    , m_filtersButton(new QToolButton(this))
    , m_download(new QPushButton(tr("Download"), this))
    , m_bitrateRule(new QComboBox(this))
    , m_bitrate(new QComboBox(this))
    , m_cbrOnly(new QCheckBox(tr("Constant bitrate only"), this))
    , m_bitDepth(new QComboBox(this))
    , m_minLength(new QTimeEdit(this))
    , m_maxLength(new QTimeEdit(this))
    , m_freeSlot(new QCheckBox(tr("Free slot only"), this))
    , m_maxQueue(new QComboBox(this))
    , m_strict(new QCheckBox(tr("Strict"), this))
    , m_owned(new QComboBox(this))
    , m_excluded(new QLineEdit(this))
    , m_filtersPanel(createFiltersPanel())
    , m_releaseRow(new QWidget(this))
    , m_release(new QComboBox(this))
    , m_releaseNote(new QLabel(this))
    , m_summary(new QLabel(this))
    , m_status(new QLabel(this))
    , m_table(new QTreeView(this))
    , m_refreshTimer(new QTimer(this))
    , m_clockTimer(new QTimer(this))
{
    setObjectName(u"view.search"_s);

    auto *empty = new QLabel(tr("Type in the search field at the top of the sidebar (⌘F). Selecta first finds the "
                                "release, then ranks the files on Soulseek by how well their length and name match "
                                "it. A search run again within a day opens from the cache without using your search "
                                "budget."),
                             m_pages);
    empty->setObjectName(u"search.empty"_s);
    empty->setWordWrap(true);
    empty->setAlignment(Qt::AlignCenter);
    empty->setForegroundRole(QPalette::PlaceholderText);
    empty->setContentsMargins(40, 0, 40, 0);
    m_pages->addWidget(empty);

    auto *page = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(createFilterBar());
    layout->addWidget(m_filtersPanel);

    // The release the results are ranked against (SRCH-17).
    m_releaseRow->setObjectName(u"search.releaseRow"_s);
    auto *releaseLayout = new QHBoxLayout(m_releaseRow);
    releaseLayout->setContentsMargins(16, 4, 16, 4);
    releaseLayout->addWidget(fieldLabel(tr("Release"), m_releaseRow));
    m_release->setObjectName(u"search.release"_s);
    m_release->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_release->setToolTip(tr("Files are ranked and checked against this release: length first, then names"));
    releaseLayout->addWidget(m_release);
    m_releaseNote->setObjectName(u"search.releaseNote"_s);
    m_releaseNote->setForegroundRole(QPalette::PlaceholderText);
    releaseLayout->addWidget(m_releaseNote, 1);
    layout->addWidget(m_releaseRow);
    connect(m_release, &QComboBox::activated, this, [this](int row) {
        const std::optional<core::ManualSearch> search = m_engine.search(m_searchId);
        if (!search) {
            return;
        }
        const int candidate = row < search->candidates.size() ? row : -1; // the last item is "Any (name only)"
        if (candidate != search->chosen) {
            m_engine.chooseSearchRelease(m_searchId, candidate);
        }
    });

    // "N of M match", why the others are hidden, and the search's state (SRCH-9, SRCH-12, SRCH-13).
    auto *summaryRow = new QHBoxLayout;
    summaryRow->setContentsMargins(16, 6, 16, 6);
    m_summary->setObjectName(u"search.summary"_s);
    m_summary->setTextFormat(Qt::RichText);
    m_status->setObjectName(u"search.status"_s);
    m_status->setTextFormat(Qt::RichText);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    connect(m_status, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link == u"refresh"_s) {
            m_engine.refreshSearch(m_searchId);
        } else if (link == u"widen"_s) {
            m_engine.widenSearch(m_searchId);
        }
    });
    summaryRow->addWidget(m_summary, 1);
    summaryRow->addWidget(m_status);
    layout->addLayout(summaryRow);

    m_table->setObjectName(u"search.results"_s);
    m_table->setModel(m_model);
    m_table->setAlternatingRowColors(true);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setRootIsDecorated(false);
    m_table->setItemsExpandable(true);
    QHeaderView *header = m_table->header();
    header->setStretchLastSection(false);
    header->setSortIndicatorClearable(true); // no indicator: best match (SRCH-18)
    header->setSortIndicator(-1, Qt::AscendingOrder);
    header->setSectionResizeMode(SearchResultsModel::TitleColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(SearchResultsModel::StatusColumn, QHeaderView::Interactive);
    m_table->setColumnWidth(SearchResultsModel::StatusColumn, 130);
    m_table->setColumnWidth(SearchResultsModel::QualityColumn, 110);
    m_table->setColumnWidth(SearchResultsModel::LengthColumn, 80);
    m_table->setColumnWidth(SearchResultsModel::PeerColumn, 130);
    m_table->setColumnWidth(SearchResultsModel::AvailabilityColumn, 125);
    m_table->setSortingEnabled(true);
    m_table->installEventFilter(this); // Return downloads
    layout->addWidget(m_table, 1);
    m_pages->addWidget(page);

    connect(m_table, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (m_model->resultAt(index)) {
            Q_EMIT previewRequested();
        }
    });
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SearchView::refreshInspector);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &SearchView::refreshInspector);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &SearchView::selectionMoved);
    connect(m_table, &QTreeView::customContextMenuRequested, this, [this](QPoint position) {
        const QList<int> selected = selectedResults();
        if (selected.isEmpty()) {
            return;
        }
        QMenu menu(this);
        menu.addAction(tr("Preview"), this, [this] { Q_EMIT previewRequested(); });
        menu.addAction(selected.size() == 1 ? tr("Download") : tr("Download %1 Files").arg(selected.size()), this,
                       &SearchView::downloadSelected);
        menu.addSeparator();
        menu.addAction(tr("Copy Path"), this, [this, selected] {
            QStringList paths;
            for (const int index : selected) {
                if (const core::SearchResult *result = m_model->result(index)) {
                    paths.append(result->path);
                }
            }
            QApplication::clipboard()->setText(paths.join(u'\n'));
        });
        menu.exec(m_table->viewport()->mapToGlobal(position));
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        if (m_model->isGrouped()) {
            m_table->expandAll();
        }
    });

    connect(m_inspector, &SearchInspector::previewRequested, this, &SearchView::previewRequested);
    connect(m_inspector, &SearchInspector::downloadRequested, this, &SearchView::downloadSelected);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_pages);

    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(kRefreshPace);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        keepSelection([this] { m_model->refresh(); });
        refreshSummary();
    });
    m_clockTimer->setInterval(30s);
    connect(m_clockTimer, &QTimer::timeout, this, &SearchView::refreshSummary);
    m_clockTimer->start();

    connect(&m_engine, &core::EngineHost::manualSearchChanged, this, [this](const core::ManualSearch &search) {
        if (search.id == m_searchId) {
            refreshHeader();
            refreshSummary();
        }
    });
    connect(&m_engine, &core::EngineHost::manualSearchClosed, this, [this](qint64 searchId) {
        if (searchId == m_searchId) {
            showSearch(0);
        }
    });
    connect(&m_engine, &core::EngineHost::searchResultsAdded, this,
            [this](qint64 searchId, const QList<core::SearchResult> &results) {
                if (searchId != m_searchId) {
                    return;
                }
                m_model->append(results);
                if (!m_refreshTimer->isActive()) {
                    m_refreshTimer->start();
                }
            });
    connect(&m_engine, &core::EngineHost::searchResultsReset, this,
            [this](qint64 searchId, const QList<core::SearchResult> &results) {
                if (searchId == m_searchId) {
                    keepSelection([this, &results] { m_model->reset(results); });
                    refreshSummary();
                }
            });
    connect(&m_engine, &core::EngineHost::searchResultsChanged, this,
            [this](qint64 searchId, const QList<core::SearchResult> &results) {
                if (searchId == m_searchId) {
                    keepSelection([this, &results] { m_model->update(results); });
                    refreshSummary();
                }
            });

    loadFilterControls();
    m_model->setFilter(m_state.filter);
    m_model->setGrouped(m_state.grouped);
    showSearch(0);
}

QWidget *SearchView::createFilterBar()
{
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 10, 16, 6);
    layout->addWidget(fieldLabel(tr("Want"), bar));
    m_preset->setObjectName(u"search.preset"_s);
    for (const core::QualityWanted wanted :
         {core::QualityWanted::Mp3_320OrLossless, core::QualityWanted::LosslessOnly, core::QualityWanted::Any}) {
        m_preset->addItem(core::qualityWantedName(wanted), static_cast<int>(wanted));
    }
    m_preset->setToolTip(tr("The quality you want. Get tracks uses the same choices."));
    layout->addWidget(m_preset);
    connect(m_preset, &QComboBox::activated, this, [this](int row) {
        const auto wanted = static_cast<core::QualityWanted>(m_preset->itemData(row).toInt());
        core::SearchFilter filter = core::presetFilter(wanted);
        filter.onlyThisRelease = m_state.filter.onlyThisRelease; // not part of the quality
        filter.owned = m_state.filter.owned;
        m_state.preset = wanted;
        applyFilter(filter, false);
        loadFilterControls();
    });
    m_onlyRelease->setObjectName(u"search.onlyRelease"_s);
    m_onlyRelease->setToolTip(tr("Hide files whose length or name doesn't fit the release"));
    connect(m_onlyRelease, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_loadingControls) {
            core::SearchFilter filter = m_state.filter;
            filter.onlyThisRelease = checked;
            applyFilter(filter, m_state.customised);
        }
    });
    layout->addSpacing(12);
    layout->addWidget(m_onlyRelease);
    m_grouped->setObjectName(u"search.grouped"_s);
    m_grouped->setToolTip(tr("Show the results by the folder they're in (a release, usually)"));
    connect(m_grouped, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_loadingControls) {
            return;
        }
        m_state.grouped = checked;
        m_table->setRootIsDecorated(checked);
        keepSelection([this, checked] { m_model->setGrouped(checked); });
        m_settings.setSearchViewState(m_state);
    });
    layout->addWidget(m_grouped);
    layout->addStretch(1);
    m_filtersButton->setObjectName(u"search.filters"_s);
    m_filtersButton->setText(tr("Filters"));
    m_filtersButton->setCheckable(true);
    m_filtersButton->setToolTip(tr("Formats, bitrate and resolution rules, length, the peer's queue, and more"));
    connect(m_filtersButton, &QToolButton::toggled, this, [this](bool shown) {
        m_filtersPanel->setVisible(shown);
        m_state.filtersShown = shown;
        m_settings.setSearchViewState(m_state);
    });
    layout->addWidget(m_filtersButton);
    m_download->setObjectName(u"search.download"_s);
    m_download->setToolTip(tr("Download the selected files to the Inbox, converted and tagged (Return)"));
    connect(m_download, &QPushButton::clicked, this, &SearchView::downloadSelected);
    layout->addWidget(m_download);
    return bar;
}

QWidget *SearchView::createFiltersPanel()
{
    auto *panel = new QWidget(this);
    panel->setObjectName(u"search.filtersPanel"_s);
    auto *grid = new QGridLayout(panel);
    grid->setContentsMargins(16, 0, 16, 8);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    const auto row = [grid, panel](int at, const QString &label) {
        grid->addWidget(fieldLabel(label, panel), at, 0, Qt::AlignRight | Qt::AlignVCenter);
        auto *line = new QHBoxLayout;
        line->setSpacing(8);
        grid->addLayout(line, at, 1);
        return line;
    };

    // Formats (SRCH-2): ticked ones only; none ticked, every format.
    QHBoxLayout *formats = row(0, tr("Formats"));
    for (int format = 0; format < core::kSearchFormatCount - 1; ++format) { // "Other" has no box
        auto *box = new QCheckBox(core::searchFormatName(static_cast<core::SearchFormat>(format)), panel);
        box->setObjectName(u"search.format.%1"_s.arg(format));
        connect(box, &QCheckBox::toggled, this, &SearchView::filtersEdited);
        m_formatButtons.append(box);
        formats->addWidget(box);
    }
    formats->addStretch(1);

    // The lossy rule (SRCH-3).
    QHBoxLayout *lossy = row(1, tr("Lossy"));
    m_bitrateRule->setObjectName(u"search.bitrateRule"_s);
    m_bitrateRule->addItem(tr("Any bitrate"), static_cast<int>(core::SearchFilter::BitrateRule::Any));
    m_bitrateRule->addItem(tr("Exactly"), static_cast<int>(core::SearchFilter::BitrateRule::Equal));
    m_bitrateRule->addItem(tr("At least"), static_cast<int>(core::SearchFilter::BitrateRule::AtLeast));
    m_bitrateRule->addItem(tr("At most"), static_cast<int>(core::SearchFilter::BitrateRule::AtMost));
    m_bitrateRule->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_bitrate->setObjectName(u"search.bitrate"_s);
    for (const int kbps : kBitrates) {
        m_bitrate->addItem(tr("%1 kbps").arg(kbps), kbps);
    }
    m_bitrate->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_cbrOnly->setObjectName(u"search.cbrOnly"_s);
    m_cbrOnly->setToolTip(tr("Hide VBR files (V0, V2…)"));
    connect(m_bitrateRule, &QComboBox::activated, this, &SearchView::filtersEdited);
    connect(m_bitrate, &QComboBox::activated, this, &SearchView::filtersEdited);
    connect(m_cbrOnly, &QCheckBox::toggled, this, &SearchView::filtersEdited);
    lossy->addWidget(m_bitrateRule);
    lossy->addWidget(m_bitrate);
    lossy->addWidget(m_cbrOnly);
    lossy->addStretch(1);

    // The lossless rule (SRCH-4): a minimum bit depth and the sample rates allowed.
    QHBoxLayout *lossless = row(2, tr("Lossless"));
    m_bitDepth->setObjectName(u"search.bitDepth"_s);
    m_bitDepth->addItem(tr("Any bit depth"), 0);
    m_bitDepth->addItem(tr("16-bit or more"), 16);
    m_bitDepth->addItem(tr("24-bit"), 24);
    m_bitDepth->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_bitDepth, &QComboBox::activated, this, &SearchView::filtersEdited);
    lossless->addWidget(m_bitDepth);
    lossless->addSpacing(8);
    for (const int rate : kSampleRates) {
        auto *box = new QCheckBox(tr("%1 kHz").arg(rate / 1000.0, 0, 'g', 4), panel);
        box->setObjectName(u"search.rate.%1"_s.arg(rate));
        box->setToolTip(tr("Allow %1 kHz (none ticked: any rate)").arg(rate / 1000.0, 0, 'g', 4));
        connect(box, &QCheckBox::toggled, this, &SearchView::filtersEdited);
        m_rateButtons.append(box);
        lossless->addWidget(box);
    }
    lossless->addStretch(1);

    // Length and the peer (SRCH-5).
    QHBoxLayout *length = row(3, tr("Length"));
    for (QTimeEdit *edit : {m_minLength, m_maxLength}) {
        edit->setDisplayFormat(u"m:ss"_s);
        edit->setTimeRange(QTime(0, 0), QTime(0, 59, 59));
        edit->setSpecialValueText(tr("Any"));
        connect(edit, &QTimeEdit::editingFinished, this, &SearchView::filtersEdited);
    }
    m_minLength->setObjectName(u"search.minLength"_s);
    m_maxLength->setObjectName(u"search.maxLength"_s);
    length->addWidget(m_minLength);
    length->addWidget(fieldLabel(tr("to"), panel));
    length->addWidget(m_maxLength);
    length->addSpacing(16);
    m_freeSlot->setObjectName(u"search.freeSlot"_s);
    connect(m_freeSlot, &QCheckBox::toggled, this, &SearchView::filtersEdited);
    length->addWidget(m_freeSlot);
    length->addSpacing(8);
    length->addWidget(fieldLabel(tr("Queue up to"), panel));
    m_maxQueue->setObjectName(u"search.maxQueue"_s);
    for (const int queue : kQueues) {
        m_maxQueue->addItem(queue == 0 ? tr("Any") : QString::number(queue), queue);
    }
    connect(m_maxQueue, &QComboBox::activated, this, &SearchView::filtersEdited);
    length->addWidget(m_maxQueue);
    length->addStretch(1);

    // Strictness, what the user already has, and words (SRCH-5, SRCH-6).
    QHBoxLayout *other = row(4, tr("Show"));
    m_strict->setObjectName(u"search.strict"_s);
    m_strict->setText(tr("Only files whose quality checks out"));
    m_strict->setToolTip(tr("Hide files whose quality the peer didn't report, or whose size contradicts it"));
    connect(m_strict, &QCheckBox::toggled, this, &SearchView::filtersEdited);
    other->addWidget(m_strict);
    other->addSpacing(16);
    other->addWidget(fieldLabel(tr("Already have"), panel));
    m_owned->setObjectName(u"search.owned"_s);
    m_owned->addItem(tr("Show"), static_cast<int>(core::SearchFilter::OwnedResults::Show));
    m_owned->addItem(tr("Dim"), static_cast<int>(core::SearchFilter::OwnedResults::Dim));
    m_owned->addItem(tr("Hide"), static_cast<int>(core::SearchFilter::OwnedResults::Hide));
    connect(m_owned, &QComboBox::activated, this, &SearchView::filtersEdited);
    other->addWidget(m_owned);
    other->addSpacing(16);
    m_excluded->setObjectName(u"search.excluded"_s);
    m_excluded->setPlaceholderText(tr("Leave out words"));
    m_excluded->setClearButtonEnabled(true);
    m_excluded->setToolTip(tr("Hide files whose path contains any of these words"));
    m_excluded->setMaximumWidth(260);
    connect(m_excluded, &QLineEdit::editingFinished, this, &SearchView::filtersEdited);
    other->addWidget(m_excluded, 1);
    other->addStretch(1);
    grid->setColumnStretch(1, 1);
    return panel;
}

void SearchView::loadFilterControls()
{
    m_loadingControls = true;
    const core::SearchFilter &filter = m_state.filter;
    const int presetRow = m_preset->findData(static_cast<int>(m_state.preset));
    for (int row = 0; row < m_preset->count(); ++row) {
        const auto wanted = static_cast<core::QualityWanted>(m_preset->itemData(row).toInt());
        const QString name = core::qualityWantedName(wanted);
        m_preset->setItemText(row, row == presetRow && m_state.customised ? tr("%1 (edited)").arg(name) : name);
    }
    m_preset->setCurrentIndex(std::max(presetRow, 0));
    m_onlyRelease->setChecked(filter.onlyThisRelease);
    m_grouped->setChecked(m_state.grouped);
    m_table->setRootIsDecorated(m_state.grouped);
    m_filtersButton->setChecked(m_state.filtersShown);
    m_filtersPanel->setVisible(m_state.filtersShown);
    for (int format = 0; format < m_formatButtons.size(); ++format) {
        const auto which = static_cast<core::SearchFormat>(format);
        m_formatButtons.at(format)->setChecked(filter.formats != 0 && filter.allows(which));
    }
    m_bitrateRule->setCurrentIndex(std::max(m_bitrateRule->findData(static_cast<int>(filter.bitrateRule)), 0));
    const int bitrateRow = m_bitrate->findData(filter.bitrateKbps);
    m_bitrate->setCurrentIndex(bitrateRow >= 0 ? bitrateRow : m_bitrate->count() - 1);
    m_bitrate->setEnabled(filter.bitrateRule != core::SearchFilter::BitrateRule::Any);
    m_cbrOnly->setChecked(filter.cbrOnly);
    m_bitDepth->setCurrentIndex(std::max(m_bitDepth->findData(filter.minBitDepth), 0));
    for (qsizetype i = 0; i < m_rateButtons.size(); ++i) {
        m_rateButtons.at(i)->setChecked(filter.sampleRates.contains(kSampleRates.at(static_cast<std::size_t>(i))));
    }
    m_minLength->setTime(timeOfSeconds(filter.minDurationSec));
    m_maxLength->setTime(timeOfSeconds(filter.maxDurationSec));
    m_freeSlot->setChecked(filter.freeSlotOnly);
    m_maxQueue->setCurrentIndex(std::max(m_maxQueue->findData(filter.maxQueue), 0));
    m_strict->setChecked(filter.strict);
    m_owned->setCurrentIndex(std::max(m_owned->findData(static_cast<int>(filter.owned)), 0));
    m_excluded->setText(filter.excludedWords.join(u' '));
    m_loadingControls = false;
}

void SearchView::filtersEdited()
{
    if (m_loadingControls) {
        return;
    }
    core::SearchFilter filter = m_state.filter;
    filter.formats = 0;
    for (int format = 0; format < m_formatButtons.size(); ++format) {
        if (m_formatButtons.at(format)->isChecked()) {
            filter.setAllowed(static_cast<core::SearchFormat>(format), true);
        }
    }
    filter.bitrateRule = static_cast<core::SearchFilter::BitrateRule>(m_bitrateRule->currentData().toInt());
    filter.bitrateKbps = m_bitrate->currentData().toInt();
    filter.cbrOnly = m_cbrOnly->isChecked();
    filter.minBitDepth = m_bitDepth->currentData().toInt();
    filter.sampleRates.clear();
    for (qsizetype i = 0; i < m_rateButtons.size(); ++i) {
        if (m_rateButtons.at(i)->isChecked()) {
            filter.sampleRates.append(kSampleRates.at(static_cast<std::size_t>(i)));
        }
    }
    filter.minDurationSec = secondsOf(m_minLength->time());
    filter.maxDurationSec = secondsOf(m_maxLength->time());
    filter.freeSlotOnly = m_freeSlot->isChecked();
    filter.maxQueue = m_maxQueue->currentData().toInt();
    filter.strict = m_strict->isChecked();
    filter.owned = static_cast<core::SearchFilter::OwnedResults>(m_owned->currentData().toInt());
    filter.excludedWords = m_excluded->text().split(QRegularExpression(u"[\\s,]+"_s), Qt::SkipEmptyParts);
    // Still the preset when only the layout-free choices moved.
    core::SearchFilter preset = core::presetFilter(m_state.preset);
    preset.onlyThisRelease = filter.onlyThisRelease;
    preset.owned = filter.owned;
    applyFilter(filter, !(filter == preset));
    loadFilterControls();
}

void SearchView::applyFilter(const core::SearchFilter &filter, bool customised)
{
    m_state.filter = filter;
    m_state.customised = customised;
    keepSelection([this] { m_model->setFilter(m_state.filter); });
    m_settings.setSearchViewState(m_state);
    refreshSummary();
}

void SearchView::keepSelection(const std::function<void()> &change)
{
    // A model reset forgets the selection; results keep their index, so it comes back.
    QList<int> selected;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        if (const core::SearchResult *result = m_model->resultAt(row)) {
            selected.append(result->index);
        }
    }
    const core::SearchResult *currentResult = m_model->resultAt(m_table->currentIndex());
    const int current = currentResult ? currentResult->index : -1;
    const int scroll = m_table->verticalScrollBar()->value();
    change();
    QItemSelection selection;
    for (const int index : std::as_const(selected)) {
        const QModelIndex row = m_model->indexOfResult(index);
        if (row.isValid()) {
            selection.select(row, row.siblingAtColumn(SearchResultsModel::ColumnCount - 1));
        }
    }
    QItemSelectionModel *selectionModel = m_table->selectionModel();
    const QSignalBlocker blocker(selectionModel); // one refresh below instead of one per row
    if (current >= 0) {
        selectionModel->setCurrentIndex(m_model->indexOfResult(current), QItemSelectionModel::NoUpdate);
    }
    selectionModel->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_table->verticalScrollBar()->setValue(scroll);
    refreshInspector();
}

void SearchView::showSearch(qint64 searchId)
{
    const std::optional<core::ManualSearch> search = searchId > 0 ? m_engine.search(searchId) : std::nullopt;
    m_searchId = search ? searchId : 0;
    m_refreshTimer->stop();
    m_pages->setCurrentIndex(search ? 1 : 0);
    m_table->selectionModel()->clear();
    m_model->reset(search ? m_engine.searchResults(searchId) : QList<core::SearchResult>());
    refreshHeader();
    refreshSummary();
    refreshInspector();
}

QString SearchView::title() const
{
    const std::optional<core::ManualSearch> search = m_engine.search(m_searchId);
    return search ? search->text : tr("Search");
}

QString SearchView::summary() const
{
    const std::optional<core::ManualSearch> search = m_engine.search(m_searchId);
    if (!search) {
        return tr("Use the search field at the top of the sidebar");
    }
    switch (search->state) {
    case core::ManualSearch::State::LookingUp:
        return tr("Finding the release…");
    case core::ManualSearch::State::Waiting:
        return tr("Waiting for the search budget…");
    case core::ManualSearch::State::Searching:
        return tr("Searching…");
    case core::ManualSearch::State::Failed:
        return search->message;
    case core::ManualSearch::State::Done:
        break;
    }
    const core::FilterCounts counts = m_model->counts();
    return tr("%1 of %2 results shown").arg(counts.shown).arg(counts.total);
}

void SearchView::refreshHeader()
{
    const std::optional<core::ManualSearch> search = m_engine.search(m_searchId);
    if (!search) {
        m_releaseRow->hide();
        m_model->setReleaseChosen(false);
        return;
    }
    const meta::Release *release = search->chosenRelease();
    m_model->setReleaseChosen(release != nullptr, release ? release->durationMs.value_or(0) : 0);
    m_onlyRelease->setEnabled(release != nullptr);
    m_releaseRow->setVisible(!search->candidates.isEmpty());
    {
        const QSignalBlocker blocker(m_release);
        m_release->clear();
        for (const meta::Candidate &candidate : search->candidates) {
            m_release->addItem(candidateText(candidate));
        }
        m_release->addItem(tr("Any (name only)"));
        m_release->setCurrentIndex(release ? search->chosen : static_cast<int>(search->candidates.size()));
    }
    QString note;
    if (release && search->respelled) {
        note = tr("Searched for “%1”, spelled as on %2").arg(search->query, meta::sourceName(release->source));
    } else if (release && search->chosenAutomatically) {
        note = tr("Picked from %1").arg(meta::sourceName(release->source));
    } else if (!release) {
        note = tr("Pick the release to rank the files against it");
    }
    m_releaseNote->setText(note);
}

void SearchView::refreshSummary()
{
    const std::optional<core::ManualSearch> search = m_engine.search(m_searchId);
    if (!search) {
        m_summary->clear();
        m_status->clear();
        Q_EMIT summaryChanged();
        return;
    }
    const core::FilterCounts counts = m_model->counts();
    QString summary = tr("<b>%1</b> of %2 match").arg(counts.shown).arg(counts.total);
    QList<std::pair<int, core::HiddenReason>> reasons;
    for (int reason = 1; reason < core::kHiddenReasonCount; ++reason) {
        const int hidden = counts.hidden.at(static_cast<std::size_t>(reason));
        if (hidden > 0) {
            reasons.append({hidden, static_cast<core::HiddenReason>(reason)});
        }
    }
    std::ranges::sort(reasons, [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList why;
    for (qsizetype i = 0; i < reasons.size() && i < kReasonsShown; ++i) {
        why.append(
            u"%1 %2"_s.arg(core::hiddenReasonLabel(reasons.at(i).second).toHtmlEscaped()).arg(reasons.at(i).first));
    }
    if (!why.isEmpty()) {
        summary += u" &nbsp;·&nbsp; "_s + tr("Hidden: %1").arg(why.join(u", "_s));
    }
    m_summary->setText(summary);

    QString status;
    const QString again = u"<a href=\"refresh\">%1</a>"_s.arg(tr("Search again"));
    switch (search->state) {
    case core::ManualSearch::State::LookingUp:
        status = tr("Finding the release…");
        break;
    case core::ManualSearch::State::Waiting:
        status = m_engine.isPaused() ? tr("Network activity is paused") : tr("Waiting for the search budget…");
        break;
    case core::ManualSearch::State::Searching:
        status = search->peers == 1 ? tr("Receiving results · 1 peer so far")
                                    : tr("Receiving results · %1 peers so far").arg(search->peers);
        break;
    case core::ManualSearch::State::Failed:
        status = search->message.toHtmlEscaped() + u" · "_s + again;
        break;
    case core::ManualSearch::State::Done:
        if (search->fromCache) {
            status = tr("Cached %1 · %2 (uses 1 search)")
                         .arg(format::ago(search->searchedAtMs, QDateTime::currentMSecsSinceEpoch()), again);
        } else {
            status = (search->peers == 1 ? tr("1 peer answered") : tr("%1 peers answered").arg(search->peers)) +
                     u" · "_s + again;
        }
        if (search->canWiden) {
            status += u" · <a href=\"widen\">%1</a>"_s.arg(tr("Search wider"));
        }
        if (!search->message.isEmpty()) {
            status += u"<br>"_s + search->message.toHtmlEscaped();
        }
        break;
    }
    m_status->setText(status);
    m_download->setEnabled(!selectedResults().isEmpty());
    Q_EMIT summaryChanged();
}

void SearchView::refreshInspector()
{
    const QList<int> selected = selectedResults();
    m_download->setEnabled(!selected.isEmpty());
    m_download->setText(selected.size() > 1 ? tr("Download %1").arg(selected.size()) : tr("Download"));
    const core::SearchResult *result = m_model->resultAt(m_table->currentIndex());
    if (!result && !selected.isEmpty()) {
        result = m_model->result(selected.constFirst());
    }
    const std::optional<core::ManualSearch> search = m_engine.search(m_searchId);
    const meta::Release *release = search ? search->chosenRelease() : nullptr;
    m_inspector->showResult(result ? std::optional<core::SearchResult>(*result) : std::nullopt,
                            release ? release->durationMs.value_or(0) : 0);
}

std::optional<SearchView::PreviewTarget> SearchView::previewTarget() const
{
    const core::SearchResult *result = m_model->resultAt(m_table->currentIndex());
    if (!result) {
        const QList<int> selected = selectedResults();
        result = selected.isEmpty() ? nullptr : m_model->result(selected.constFirst());
    }
    if (!result || !result->isAudio() || m_searchId == 0) {
        return std::nullopt;
    }
    return PreviewTarget{.searchId = m_searchId, .index = result->index, .peer = result->hit.peer};
}

QList<int> SearchView::selectedResults() const
{
    QList<int> results;
    QSet<int> seen;
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    for (const QModelIndex &row : rows) {
        const QList<int> indexes =
            m_model->resultAt(row) ? QList<int>{m_model->resultAt(row)->index} : m_model->resultsUnder(row);
        for (const int index : indexes) {
            const core::SearchResult *result = m_model->result(index);
            if (result && result->isAudio() && !seen.contains(index)) {
                seen.insert(index);
                results.append(index);
            }
        }
    }
    return results;
}

void SearchView::downloadSelected()
{
    QList<int> selected = selectedResults();
    if (selected.isEmpty() || m_searchId == 0) {
        return;
    }
    QList<int> fresh;
    for (const int index : std::as_const(selected)) {
        const core::SearchResult *result = m_model->result(index);
        if (result && !core::isOwned(result->ownership)) {
            fresh.append(index);
        }
    }
    if (fresh.size() == selected.size()) {
        m_engine.downloadResults(m_searchId, selected);
        return;
    }
    // The duplicate guard (DL-11): say what's already there before downloading it again.
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Question);
    const int owned = static_cast<int>(selected.size() - fresh.size());
    box->setText(selected.size() == 1 ? tr("You already have this track.")
                                      : tr("You already have %1 of these tracks.").arg(owned));
    const core::SearchResult *first = nullptr;
    for (const int index : std::as_const(selected)) {
        const core::SearchResult *result = m_model->result(index);
        if (result && core::isOwned(result->ownership)) {
            first = result;
            break;
        }
    }
    if (first) {
        box->setInformativeText(first->ownershipDetail.isEmpty()
                                    ? core::ownershipLabel(first->ownership)
                                    : core::ownershipLabel(first->ownership) + u": "_s + first->ownershipDetail);
    }
    QPushButton *anyway = box->addButton(tr("Download Anyway"), QMessageBox::AcceptRole);
    QPushButton *onlyNew = fresh.isEmpty() ? nullptr : box->addButton(tr("Only the New Ones"), QMessageBox::ActionRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(onlyNew ? onlyNew : anyway);
    box->setWindowModality(Qt::WindowModal); // a sheet on macOS
    connect(box, &QMessageBox::finished, this, [this, box, anyway, onlyNew, selected, fresh, searchId = m_searchId] {
        if (box->clickedButton() == anyway) {
            m_engine.downloadResults(searchId, selected);
        } else if (onlyNew && box->clickedButton() == onlyNew) {
            m_engine.downloadResults(searchId, fresh);
        }
    });
    box->open();
}

bool SearchView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_table && event->type() == QEvent::KeyPress) {
        const auto *key = dynamic_cast<const QKeyEvent *>(event);
        if (key && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::NoModifier) {
            downloadSelected(); // PREV-4: Return downloads
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace selecta::app
