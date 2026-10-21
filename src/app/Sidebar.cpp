// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Sidebar.h"

#include "app/Accessibility.h"
#include "app/Appearance.h"
#include "app/Icons.h"
#include "app/NavDelegate.h"
#include "app/NavModel.h"
#include "app/SourceListView.h"
#include "app/Style.h"
#include "app/WindowChrome.h"

#include <QAction>
#include <QCompleter>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShortcut>
#include <QStringListModel>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kDotSize = 8;
const QColor kOnlineGreen(0x34, 0xC7, 0x59);
constexpr int kSearchHeight = 30;
// The panel's top, down to the search field: the traffic lights sit in it, centred in the title bar.
constexpr int kHeaderHeight = chrome::kTitleBarHeight - (2 * style::kSpace2);

// The search field's capsule (macOS 26): a soft fill, and the accent ring while the field has the keyboard.
class SearchCapsule : public QWidget
{
public:
    explicit SearchCapsule(QWidget *parent)
        : QWidget(parent)
    {
        setFixedHeight(kSearchHeight);
    }

    void setField(QLineEdit *field)
    {
        m_field = field;
        field->installEventFilter(this); // the ring follows the field's focus
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_field && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
            update();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF body = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        const QPainterPath shape = style::squircle(body, body.height() / 2.0);
        const bool focused = m_field && m_field->hasFocus();
        QColor fill = palette().color(QPalette::WindowText);
        fill.setAlphaF(isDarkPalette(palette()) ? 0.1F : 0.065F);
        painter.fillPath(shape, focused ? palette().color(QPalette::Base) : fill);
        if (focused) {
            QColor ring = symbolAccentColor(palette());
            painter.setPen(QPen(ring, 1.0));
            painter.drawPath(shape);
            ring.setAlphaF(0.3F);
            painter.setPen(QPen(ring, 3.0));
            painter.drawPath(style::squircle(body.adjusted(-1, -1, 1, 1), (body.height() / 2.0) + 1));
        }
    }

private:
    QLineEdit *m_field = nullptr;
};

} // namespace

StatusDot::StatusDot(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kDotSize, kDotSize);
    setAccessibleName(tr("Offline"));
}

void StatusDot::setOnline(bool online)
{
    if (m_online == online) {
        return;
    }
    m_online = online;
    setAccessibleName(online ? tr("Online") : tr("Offline"));
    update();
}

void StatusDot::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_online ? kOnlineGreen : palette().color(QPalette::PlaceholderText));
    painter.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
}

Sidebar::Sidebar(NavModel *model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_search(new QLineEdit(this))
    , m_recent(new QStringListModel(this))
    , m_list(new SourceListView(this))
    , m_statusDot(new StatusDot(this))
    , m_statusText(new QLabel(this))
{
    setObjectName(u"sidebar"_s);
    setAutoFillBackground(false); // the window's glass shows around the panel
    setMinimumWidth(180);

    // Searches typed by hand start here (SRCH-1); each stays open in the list below.
    m_search->setObjectName(u"sidebar.search"_s);
    m_search->setPlaceholderText(tr("Search Soulseek"));
    m_search->setAccessibleName(tr("Search Soulseek"));
    a11y::addClearButton(m_search);
    // Frameless on its capsule, which draws the focus ring.
    m_search->setFrame(false);
    m_search->setAttribute(Qt::WA_MacShowFocusRect, false);
    QPalette fieldPalette = m_search->palette();
    fieldPalette.setColor(QPalette::Base, Qt::transparent);
    m_search->setPalette(fieldPalette);
    auto *capsule = new SearchCapsule(this);
    capsule->setObjectName(u"sidebar.searchBox"_s);
    capsule->setField(m_search);
    m_searchBox = capsule;
    auto *fieldLayout = new QHBoxLayout(capsule);
    fieldLayout->setContentsMargins(style::kSpace1, 0, style::kSpace2, 0);
    fieldLayout->addWidget(m_search);
    m_search->addAction(icons::symbol(u"magnifyingglass"_s, u"edit-find"_s), QLineEdit::LeadingPosition)
        ->setText(tr("Search"));
    m_search->setToolTip(tr("Search the Soulseek network (⌘F). Use -word to leave a word out."));
    auto *completer = new QCompleter(m_recent, m_search);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    m_search->setCompleter(completer);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QString text = m_search->text().simplified();
        if (!text.isEmpty()) {
            Q_EMIT searchRequested(text);
            m_search->clear();
        }
    });

    m_list->setObjectName(u"sidebar.list"_s);
    m_list->setModel(m_model);
    m_list->setAutoFillBackground(false);
    m_list->viewport()->setAutoFillBackground(false); // the panel shows through
    auto *delegate = new NavDelegate(m_list);
    m_list->setItemDelegate(delegate);
    m_list->setAccessibleName(tr("Sidebar"));
    m_list->setMouseTracking(true); // hover shows the close buttons
    connect(delegate, &NavDelegate::closeRequested, this, &Sidebar::closeRequested);
    // ⌫ closes the selected search or browse, as the × does under the mouse.
    for (const QKeySequence &keys : {QKeySequence(Qt::Key_Backspace), QKeySequence(Qt::Key_Delete)}) {
        auto *close = new QShortcut(keys, m_list);
        close->setContext(Qt::WidgetShortcut);
        connect(close, &QShortcut::activated, this, [this] {
            const QModelIndex current = m_list->currentIndex();
            if (current.data(NavModel::ClosableRole).toBool()) {
                Q_EMIT closeRequested(current.data(NavModel::IdRole).toString());
            }
        });
    }
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](QPoint position) {
        const QModelIndex index = m_list->indexAt(position);
        if (!index.data(NavModel::ClosableRole).toBool()) {
            return;
        }
        const QString id = index.data(NavModel::IdRole).toString();
        QMenu menu(this);
        menu.addAction(NavModel::searchOfItem(id) > 0 ? tr("Close Search") : tr("Close"), this,
                       [this, id] { Q_EMIT closeRequested(id); });
        menu.exec(m_list->viewport()->mapToGlobal(position));
    });
    a11y::addMenuKey(m_list);

    m_statusDot->setObjectName(u"sidebar.statusDot"_s);
    m_statusText->setObjectName(u"sidebar.status"_s);
    m_statusText->setTextFormat(Qt::PlainText); // a username, the server's words
    m_statusText->setForegroundRole(QPalette::PlaceholderText);

    auto *status = new QHBoxLayout;
    status->setContentsMargins(style::kSpace4, style::kSpace2, style::kSpace4, style::kSpace3);
    status->setSpacing(6);
    status->addWidget(m_statusDot);
    status->addWidget(m_statusText, 1);

    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(style::kSpace2, 0, style::kSpace2, style::kSpace2);
    searchRow->addWidget(m_searchBox);

    // The panel stands kSpace2 in from the window's edges; its content starts under the traffic lights on macOS.
    auto *layout = new QVBoxLayout(this);
    const int top = chrome::contentUnderTitleBar() ? kHeaderHeight : style::kSpace2;
    layout->setContentsMargins(style::kSpace2, style::kSpace2 + top, style::kSpace2, style::kSpace2);
    layout->setSpacing(0);
    layout->addLayout(searchRow);
    layout->addWidget(m_list, 1);
    layout->addLayout(status);

    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &current) {
        if (m_selecting || !current.isValid()) {
            return;
        }
        const QString id = m_model->idAt(current.row());
        if (!id.isEmpty()) {
            Q_EMIT navigationRequested(id);
        }
    });

    setConnectionStatus(tr("Not connected"), false);
}

QRect Sidebar::panelRect() const
{
    return rect().adjusted(style::kSpace2, style::kSpace2, -style::kSpace2, -style::kSpace2);
}

QColor Sidebar::panelFill(bool dark)
{
    return dark ? QColor(255, 255, 255, 14) : QColor(255, 255, 255, 97);
}

void Sidebar::paintEvent(QPaintEvent * /*event*/)
{
    // The floating panel (mock: .sidebar): a lighter glass over the window's, a lit rim, a hairline and a soft shadow.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkPalette(palette());
    const QRectF panel = QRectF(panelRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = style::kRadiusLarge;
    for (int i = 1; i <= 3; ++i) {
        painter.fillPath(style::squircle(panel.adjusted(-0.5 * i, 0, 0.5 * i, 0.8 * i), radius + (0.5 * i)),
                         QColor(0, 0, 0, dark ? 22 / i : 9 / i));
    }
    const QPainterPath shape = style::squircle(panel, radius);
    painter.fillPath(shape, panelFill(dark));
    painter.setPen(QPen(dark ? QColor(0, 0, 0, 115) : QColor(0, 0, 0, 20), 0.75));
    painter.drawPath(shape);
    painter.setPen(QPen(dark ? QColor(255, 255, 255, 33) : QColor(255, 255, 255, 205), 0.75));
    painter.drawPath(style::squircle(panel.adjusted(0.75, 0.75, -0.75, -0.75), radius - 0.75));
}

void Sidebar::mousePressEvent(QMouseEvent *event)
{
    // The panel's top and its empty space move the window, as a title bar does; the controls take their own clicks.
    if (event->button() == Qt::LeftButton) {
        chrome::titleBarPressed(window());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void Sidebar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->position().y() < style::kSpace2 + kHeaderHeight) {
        chrome::titleBarDoubleClicked(window());
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void Sidebar::select(const QString &id)
{
    const int row = m_model->rowOf(id);
    m_selecting = true;
    if (row < 0) {
        m_list->selectionModel()->clear();
    } else {
        m_list->selectionModel()->setCurrentIndex(m_model->index(row), QItemSelectionModel::ClearAndSelect);
    }
    m_selecting = false;
}

QString Sidebar::currentId() const
{
    const QModelIndex current = m_list->currentIndex();
    return current.isValid() ? m_model->idAt(current.row()) : QString();
}

void Sidebar::setRecentSearches(const QStringList &searches)
{
    m_recent->setStringList(searches);
}

void Sidebar::focusSearch()
{
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void Sidebar::setConnectionStatus(const QString &text, bool online)
{
    m_statusText->setText(text);
    m_statusDot->setOnline(online);
}

} // namespace selecta::app
