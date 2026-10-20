// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Toasts.h"

#include "app/Icons.h"

#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kWidth = 360;
constexpr int kMargin = 16;
constexpr qreal kRadius = 12.0;

// One notification: a rounded panel in the window's colours.
class Toast : public QFrame
{
    Q_OBJECT

public:
    explicit Toast(QWidget *parent)
        : QFrame(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(kWidth);
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(24);
        shadow->setOffset(0, 4);
        shadow->setColor(QColor(0, 0, 0, 70));
        setGraphicsEffect(shadow);
    }

    QTimer *timer = nullptr;
    QString text;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
        painter.fillPath(path, palette().color(QPalette::Window));
        painter.setPen(QPen(palette().color(QPalette::Mid), 0.5));
        painter.drawPath(path);
    }
};

QColor kindColor(Toasts::Kind kind, const QPalette &palette)
{
    switch (kind) {
    case Toasts::Kind::Good:
        return {52, 168, 83};
    case Toasts::Kind::Warning:
        return {230, 140, 20};
    case Toasts::Kind::Info:
        break;
    }
    return palette.color(QPalette::Highlight);
}

QIcon kindIcon(Toasts::Kind kind)
{
    switch (kind) {
    case Toasts::Kind::Good:
        return icons::symbol(u"checkmark.circle"_s, u"dialog-ok"_s);
    case Toasts::Kind::Warning:
        return icons::symbol(u"exclamationmark.triangle"_s, u"dialog-warning"_s);
    case Toasts::Kind::Info:
        break;
    }
    return icons::symbol(u"info.circle"_s, u"dialog-information"_s);
}

} // namespace

Toasts::Toasts(QWidget *host)
    : QWidget(host)
    , m_host(host)
    , m_layout(new QVBoxLayout(this))
{
    setObjectName(u"toasts"_s);
    m_layout->setContentsMargins(8, 8, 8, 8); // room for the shadows
    m_layout->setSpacing(8);
    m_layout->setSizeConstraint(QLayout::SetFixedSize);
    host->installEventFilter(this);
    hide();
}

QString Toasts::show(const QString &title, const QString &body, Kind kind, const QList<Action> &actions,
                     std::chrono::milliseconds timeout)
{
    auto *toast = new Toast(this);
    const QString name = u"toast.%1"_s.arg(m_next++);
    toast->setObjectName(name);
    toast->text = body.isEmpty() ? title : title + u'\n' + body;
    toast->setAccessibleName(title);
    toast->setAccessibleDescription(body);

    auto *row = new QHBoxLayout(toast);
    row->setContentsMargins(12, 10, 8, 10);
    row->setSpacing(10);
    auto *icon = new QLabel(toast);
    const QIcon symbol = kindIcon(kind);
    if (!symbol.isNull()) {
        icon->setPixmap(icons::tinted(symbol, QSize(18, 18), devicePixelRatioF(), kindColor(kind, palette())));
    }
    icon->setFixedSize(18, 18);
    row->addWidget(icon, 0, Qt::AlignTop);

    auto *text = new QVBoxLayout;
    text->setSpacing(2);
    auto *heading = new QLabel(title, toast);
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    heading->setWordWrap(true);
    heading->setTextFormat(Qt::PlainText);
    text->addWidget(heading);
    if (!body.isEmpty()) {
        auto *line = new QLabel(body, toast);
        line->setWordWrap(true);
        line->setTextFormat(Qt::PlainText);
        line->setForegroundRole(QPalette::PlaceholderText);
        text->addWidget(line);
    }
    if (!actions.isEmpty()) {
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(6);
        for (qsizetype i = 0; i < actions.size(); ++i) {
            auto *button = new QPushButton(actions.at(i).label, toast);
            button->setObjectName(u"%1.action.%2"_s.arg(name).arg(i));
            connect(button, &QPushButton::clicked, this, [this, toast, run = actions.at(i).run] {
                dismiss(toast);
                if (run) {
                    run();
                }
            });
            buttons->addWidget(button);
        }
        buttons->addStretch(1);
        text->addLayout(buttons);
    }
    row->addLayout(text, 1);

    auto *close = new QToolButton(toast);
    close->setObjectName(name + u".close"_s);
    close->setAutoRaise(true);
    close->setText(u"×"_s);
    close->setAccessibleName(tr("Dismiss"));
    close->setToolTip(tr("Dismiss"));
    connect(close, &QToolButton::clicked, this, [this, toast] { dismiss(toast); });
    row->addWidget(close, 0, Qt::AlignTop);

    toast->timer = new QTimer(toast);
    toast->timer->setSingleShot(true);
    toast->timer->setInterval(timeout);
    connect(toast->timer, &QTimer::timeout, this, [this, toast] { dismiss(toast); });
    toast->timer->start();
    toast->installEventFilter(this); // the pointer on it holds it

    m_layout->insertWidget(0, toast);
    while (m_layout->count() > kMaxShown) {
        QWidget *oldest = m_layout->itemAt(m_layout->count() - 1)->widget();
        m_layout->removeWidget(oldest);
        oldest->deleteLater();
    }
    QWidget::show();
    place();
    return name;
}

int Toasts::count() const
{
    return m_layout->count();
}

QStringList Toasts::texts() const
{
    QStringList texts;
    for (int i = 0; i < m_layout->count(); ++i) {
        if (const auto *toast = qobject_cast<const Toast *>(m_layout->itemAt(i)->widget())) {
            texts.append(toast->text);
        }
    }
    return texts;
}

void Toasts::dismiss(QWidget *toast)
{
    if (m_layout->indexOf(toast) < 0) {
        return;
    }
    m_layout->removeWidget(toast);
    toast->hide();
    toast->deleteLater();
    if (m_layout->count() == 0) {
        hide();
    } else {
        place();
    }
}

void Toasts::place()
{
    adjustSize();
    move(m_host->width() - width() - kMargin + 8, kMargin - 8);
    raise();
}

bool Toasts::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_host && event->type() == QEvent::Resize && isVisible()) {
        place();
    } else if (const auto *toast = qobject_cast<const Toast *>(watched); toast && toast->timer) {
        if (event->type() == QEvent::Enter) {
            toast->timer->stop();
        } else if (event->type() == QEvent::Leave) {
            toast->timer->start();
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace selecta::app

#include "Toasts.moc"
