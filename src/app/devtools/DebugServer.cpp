// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/devtools/DebugServer.h"

#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/Preview.h"
#include "base/DebugProtocol.h"
#include "base/Version.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QStyleHints>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
Q_LOGGING_CATEGORY(lcDevtools, "selecta.app.devtools")
} // namespace

namespace selecta::app::devtools {
namespace protocol = base::debugprotocol;
namespace {

constexpr int kProbeTimeoutMs = 200;

QJsonArray rectJson(QRect rect)
{
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

QString widgetText(const QWidget *widget)
{
    if (const auto *label = qobject_cast<const QLabel *>(widget)) {
        return label->text();
    }
    if (const auto *button = qobject_cast<const QAbstractButton *>(widget)) {
        return button->text();
    }
    if (const auto *edit = qobject_cast<const QLineEdit *>(widget)) {
        return edit->text();
    }
    return {};
}

QJsonObject describe(const QWidget *widget, int depth)
{
    QJsonObject node;
    node["class"_L1] = QString::fromLatin1(widget->metaObject()->className());
    if (!widget->objectName().isEmpty()) {
        node["name"_L1] = widget->objectName();
    }
    node["visible"_L1] = widget->isVisible();
    node["geometry"_L1] = rectJson(widget->geometry());
    if (!widget->isEnabled()) {
        node["enabled"_L1] = false;
    }
    if (const QString text = widgetText(widget); !text.isEmpty()) {
        node["text"_L1] = text;
    }
    if (const auto *button = qobject_cast<const QAbstractButton *>(widget); button && button->isCheckable()) {
        node["checked"_L1] = button->isChecked();
    }
    if (const auto *view = qobject_cast<const QAbstractItemView *>(widget); view && view->model()) {
        node["rows"_L1] = view->model()->rowCount(view->rootIndex());
        if (const QModelIndex current = view->currentIndex(); current.isValid()) {
            node["current"_L1] = current.data().toString();
        }
    }
    if (depth > 0) {
        QJsonArray children;
        for (QObject *child : widget->children()) {
            if (const auto *childWidget = qobject_cast<const QWidget *>(child);
                childWidget && !childWidget->isWindow()) {
                children.append(describe(childWidget, depth - 1));
            }
        }
        if (!children.isEmpty()) {
            node["children"_L1] = children;
        }
    }
    return node;
}

QString colorSchemeName(Qt::ColorScheme scheme)
{
    switch (scheme) {
    case Qt::ColorScheme::Light:
        return u"light"_s;
    case Qt::ColorScheme::Dark:
        return u"dark"_s;
    case Qt::ColorScheme::Unknown:
        break;
    }
    return u"unknown"_s;
}

} // namespace

DebugServer::DebugServer(MainWindow &window, QString logDir, QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_logDir(std::move(logDir))
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &DebugServer::acceptConnections);
}

DebugServer::~DebugServer() = default;

bool DebugServer::listen(const QString &name)
{
    // Never take over a socket another live Selecta is serving; only clear one left behind by a crash.
    QLocalSocket probe;
    probe.connectToServer(name);
    if (probe.waitForConnected(kProbeTimeoutMs)) {
        qCWarning(lcDevtools) << "another process already serves the debug socket" << name;
        return false;
    }
    QLocalServer::removeServer(name);
    if (!m_server.listen(name)) {
        qCWarning(lcDevtools) << "debug server can't listen on" << name << '-' << m_server.errorString();
        return false;
    }
    qCInfo(lcDevtools) << "debug server listening on" << m_server.fullServerName();
    return true;
}

void DebugServer::acceptConnections()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        m_buffers.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readFrom(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void DebugServer::readFrom(QLocalSocket *socket)
{
    if (!m_buffers.contains(socket)) {
        return;
    }
    // Work on a copy: handling a command can run code that adds or removes connections and rehashes m_buffers.
    QByteArray buffer = m_buffers.value(socket) + socket->readAll();
    QByteArray replies;
    qsizetype newline = buffer.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            QJsonObject response;
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                response = QJsonObject{{"ok"_L1, false},
                                       {"error"_L1, QString(u"invalid request: "_s + parseError.errorString())}};
            } else {
                response = handle(document.object());
            }
            replies += QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
        }
        newline = buffer.indexOf('\n');
    }

    if (!m_buffers.contains(socket)) {
        return; // disconnected while handling
    }
    if (!replies.isEmpty()) {
        socket->write(replies);
        socket->flush();
    }
    if (buffer.size() > protocol::kMaxLineBytes) {
        const QJsonObject tooLong{{"ok"_L1, false},
                                  {"error"_L1, u"request longer than %1 bytes"_s.arg(protocol::kMaxLineBytes)}};
        socket->write(QJsonDocument(tooLong).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
        socket->disconnectFromServer();
        return;
    }
    m_buffers.insert(socket, buffer);
}

QJsonObject DebugServer::handle(const QJsonObject &request)
{
    const QString command = request.value("cmd"_L1).toString();
    const QJsonObject args = request.value("args"_L1).toObject();
    QString error;
    QJsonValue result;
    if (command == u"ping") {
        {
            result = ping();
        }
    } else if (command == u"state") {
        {
            result = state();
        }
    } else if (command == u"views") {
        {
            result = QJsonArray::fromStringList(m_window.viewIds());
        }
    } else if (command == u"tree") {
        {
            result = tree(args, &error);
        }
    } else if (command == u"screenshot") {
        {
            result = screenshot(args, &error);
        }
    } else if (command == u"navigate") {
        {
            result = navigate(args, &error);
        }
    } else if (command == u"click") {
        {
            result = click(args, &error);
        }
    } else if (command == u"type") {
        {
            result = type(args, &error);
        }
    } else if (command == u"appearance") {
        {
            result = appearance(args, &error);
        }
    } else if (command == u"log") {
        {
            result = log(args, &error);
        }
    } else if (command == u"select") {
        result = select(args, &error);
    } else if (command == u"drop") {
        result = drop(args, &error);
    } else if (command == u"play-pause") {
        m_window.playOrPause();
        result = true;
    } else if (command == u"player") {
        result = player(args, &error);
    } else if (command == u"quit") {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        result = true;
    } else {
        error = command.isEmpty() ? u"missing \"cmd\""_s : u"unknown command: "_s + command;
    }

    QJsonObject response{{"id"_L1, request.value("id"_L1)}, {"ok"_L1, error.isEmpty()}};
    if (error.isEmpty()) {
        response["result"_L1] = result;
    } else {
        response["error"_L1] = error;
    }
    return response;
}

QJsonValue DebugServer::ping()
{
    return QJsonObject{{"version"_L1, QString(base::kVersion)},
                       {"pid"_L1, QCoreApplication::applicationPid()},
                       {"qt"_L1, QString::fromLatin1(qVersion())},
                       {"platform"_L1, QGuiApplication::platformName()},
                       {"style"_L1, QApplication::style() ? QApplication::style()->name() : QString()}};
}

QJsonValue DebugServer::state()
{
    return QJsonObject{
        {"view"_L1, m_window.currentViewId()},
        {"title"_L1, m_window.currentTitle()},
        {"subtitle"_L1, m_window.currentSubtitle()},
        {"sidebar"_L1, m_window.isSidebarShown()},
        {"inspector"_L1, m_window.isInspectorShown()},
        {"appearance"_L1, appearanceKey(m_window.appearance())},
        {"colorScheme"_L1, colorSchemeName(QGuiApplication::styleHints()->colorScheme())},
        {"window"_L1, QJsonObject{{"geometry"_L1, rectJson(m_window.geometry())},
                                  {"visible"_L1, m_window.isVisible()},
                                  {"active"_L1, m_window.isActiveWindow()},
                                  {"minimized"_L1, m_window.isMinimized()},
                                  {"onScreen"_L1, !m_window.testAttribute(Qt::WA_DontShowOnScreen)}}},
    };
}

QJsonValue DebugServer::tree(const QJsonObject &args, QString *error)
{
    const QString name = args.value("name"_L1).toString();
    const QWidget *root = name.isEmpty() ? &m_window : m_window.findChild<QWidget *>(name);
    if (!root) {
        *error = u"no widget named "_s + name;
        return {};
    }
    const int depth = std::clamp(args.value("depth"_L1).toInt(protocol::kTreeDefaultDepth), 0, protocol::kTreeMaxDepth);
    return describe(root, depth);
}

QJsonValue DebugServer::screenshot(const QJsonObject &args, QString *error)
{
    const QString path = args.value("path"_L1).toString();
    const QFileInfo target(path);
    if (path.isEmpty() || !target.isAbsolute() || target.suffix().compare(u"png"_s, Qt::CaseInsensitive) != 0) {
        *error = u"\"path\" must be an absolute path ending in .png"_s;
        return {};
    }
    if (!target.absoluteDir().exists()) {
        *error = u"folder doesn't exist: "_s + target.absolutePath();
        return {};
    }
    const QString name = args.value("name"_L1).toString();
    QWidget *widget = name.isEmpty() ? &m_window : m_window.findChild<QWidget *>(name);
    if (!widget) {
        *error = u"no widget named "_s + name;
        return {};
    }
    QCoreApplication::sendPostedEvents(); // apply pending layout and polish before rendering
    const qreal dpr = widget->devicePixelRatioF();
    const QSize pixelSize = widget->size() * dpr;
    if (pixelSize.isEmpty()) {
        *error = u"the widget has no size to render"_s;
        return {};
    }
    QPixmap rendered(pixelSize);
    rendered.setDevicePixelRatio(dpr);
    rendered.fill(Qt::transparent);
    widget->render(&rendered, QPoint(), QRegion(), QWidget::DrawWindowBackground | QWidget::DrawChildren);

    // macOS draws some areas natively (the window background under a unified toolbar is transparent to Qt), so a plain
    // render leaves holes. Composite over an opaque window colour, as the screen does.
    QColor background = widget->palette().color(QPalette::Window);
    if (background.alpha() < 255) {
        const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        background = dark ? QColor(0x1E, 0x1E, 0x1E) : QColor(0xEC, 0xEC, 0xEC);
    }
    QPixmap pixmap(pixelSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(background);
    {
        QPainter painter(&pixmap);
        painter.drawPixmap(0, 0, rendered);
    }
    if (!pixmap.save(path, "PNG")) {
        *error = u"couldn't render or save the screenshot"_s;
        return {};
    }
    return QJsonObject{{"path"_L1, path},
                       {"width"_L1, pixmap.width()},
                       {"height"_L1, pixmap.height()},
                       {"devicePixelRatio"_L1, pixmap.devicePixelRatio()}};
}

QJsonValue DebugServer::navigate(const QJsonObject &args, QString *error)
{
    const QString view = args.value("view"_L1).toString();
    if (!m_window.navigateTo(view)) {
        *error = u"unknown view \"%1\"; known: %2"_s.arg(view, m_window.viewIds().join(u", "_s));
        return {};
    }
    return view;
}

QJsonValue DebugServer::click(const QJsonObject &args, QString *error)
{
    const QString name = args.value("name"_L1).toString();
    if (name.isEmpty()) {
        *error = u"missing \"name\""_s;
        return {};
    }
    if (auto *action = m_window.findChild<QAction *>(name)) {
        if (!action->isEnabled()) {
            *error = u"action is disabled: "_s + name;
            return {};
        }
        action->trigger();
        return u"action"_s;
    }
    if (auto *button = m_window.findChild<QAbstractButton *>(name)) {
        if (!button->isEnabled() || !button->isVisible()) {
            *error = u"button is disabled or hidden: "_s + name;
            return {};
        }
        button->click();
        return u"button"_s;
    }
    *error = u"no action or button named "_s + name;
    return {};
}

QJsonValue DebugServer::type(const QJsonObject &args, QString *error)
{
    const QString name = args.value("name"_L1).toString();
    const QString text = args.value("text"_L1).toString();
    if (auto *line = m_window.findChild<QLineEdit *>(name)) {
        line->setText(text);
        if (args.value("submit"_L1).toBool()) {
            // As if Return was pressed in it (the sidebar's search field searches on Return).
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(line, &press);
            QCoreApplication::sendEvent(line, &release);
        }
        return true;
    }
    if (auto *plain = m_window.findChild<QPlainTextEdit *>(name)) {
        plain->setPlainText(text);
        return true;
    }
    *error = u"no text field named "_s + name;
    return {};
}

// select name=<item view> row=<n> [parent=<n>]: makes a row current and selected (a child row under `parent` in a
// tree); name=<tab widget> row=<n> shows that page.
QJsonValue DebugServer::select(const QJsonObject &args, QString *error)
{
    const QString name = args.value("name"_L1).toString();
    if (auto *tabs = m_window.findChild<QTabWidget *>(name)) { // a page of a tab widget, by its number
        const int page = args.value("row"_L1).toInt(-1);
        if (page < 0 || page >= tabs->count()) {
            *error = u"no page %1 there (%2 pages)"_s.arg(page).arg(tabs->count());
            return {};
        }
        tabs->setCurrentIndex(page);
        return tabs->tabText(page);
    }
    auto *view = m_window.findChild<QAbstractItemView *>(name);
    if (!view || !view->model()) {
        *error = u"no item view named "_s + name;
        return {};
    }
    QModelIndex parent;
    if (args.contains("parent"_L1)) {
        parent = view->model()->index(args.value("parent"_L1).toInt(), 0);
        if (!parent.isValid()) {
            *error = u"no row %1 at the top level"_s.arg(args.value("parent"_L1).toInt());
            return {};
        }
    }
    const QModelIndex index = view->model()->index(args.value("row"_L1).toInt(-1), 0, parent);
    if (!index.isValid()) {
        *error =
            u"no row %1 there (%2 rows)"_s.arg(args.value("row"_L1).toInt(-1)).arg(view->model()->rowCount(parent));
        return {};
    }
    view->setCurrentIndex(index);
    view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    return index.data().toString();
}

// drop name=<widget> paths=<file>[;<file>…]: drops these local files on the widget, as from the Finder.
QJsonValue DebugServer::drop(const QJsonObject &args, QString *error)
{
    const QString name = args.value("name"_L1).toString();
    auto *widget = m_window.findChild<QWidget *>(name);
    if (!widget) {
        *error = u"no widget named "_s + name;
        return {};
    }
    QList<QUrl> urls;
    const QStringList paths = args.value("paths"_L1).toString().split(u';', Qt::SkipEmptyParts);
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    if (urls.isEmpty()) {
        *error = u"missing \"paths\""_s;
        return {};
    }
    QMimeData data;
    data.setUrls(urls);
    const QPoint centre = widget->rect().center();
    QDragEnterEvent enter(centre, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &enter);
    if (!enter.isAccepted()) {
        *error = u"the widget doesn't take dropped files: "_s + name;
        return {};
    }
    QDropEvent dropped(QPointF(centre), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &dropped);
    return static_cast<int>(urls.size());
}

// player [action=toggle|stop|seek|skip|play] [seconds=<s>] [path=<file>]: drives the preview, then reports its state.
QJsonValue DebugServer::player(const QJsonObject &args, QString *error)
{
    PreviewController &preview = m_window.preview();
    playback::Player &player = preview.player();
    const QString action = args.value("action"_L1).toString();
    const double seconds = args.value("seconds"_L1).toDouble();
    if (action == u"toggle") {
        preview.toggle();
    } else if (action == u"stop") {
        preview.stop();
    } else if (action == u"seek") {
        player.seek(seconds);
    } else if (action == u"skip") {
        player.skip(seconds);
    } else if (action == u"play") {
        const QString path = args.value("path"_L1).toString();
        const LocalPreview local = previewForPath(path, {QFileInfo(path).completeBaseName(), {}, u"Debug"_s});
        if (!local.request) {
            *error = local.error;
            return {};
        }
        preview.preview(*local.request);
    } else if (!action.isEmpty()) {
        *error = u"unknown action \"%1\": toggle, stop, seek, skip or play"_s.arg(action);
        return {};
    }
    const audio::Waveform &waveform = preview.waveform();
    return QJsonObject{
        {"state"_L1, QString::fromLatin1(
                         QMetaEnum::fromType<playback::Player::State>().valueToKey(static_cast<int>(player.state())))},
        {"title"_L1, player.track().title},
        {"artist"_L1, player.track().artist},
        {"source"_L1, player.track().source},
        {"position"_L1, player.position()},
        {"duration"_L1, player.duration()},
        {"buffered"_L1, player.buffered()},
        {"volume"_L1, player.volume()},
        {"detail"_L1, player.detail()},
        {"error"_L1, player.error()},
        {"identity"_L1, preview.identity()},
        {"waveform"_L1, QJsonObject{{"columns"_L1, waveform.detail().size()},
                                    {"complete"_L1, waveform.isComplete()},
                                    {"seconds"_L1, waveform.isNull() ? 0.0 : waveform.durationSeconds()}}},
    };
}

QJsonValue DebugServer::appearance(const QJsonObject &args, QString *error)
{
    const QString value = args.value("value"_L1).toString();
    const Appearance fallback = m_window.appearance();
    const Appearance parsed = appearanceFromKey(value, fallback);
    if (appearanceKey(parsed) != value) {
        *error = u"\"value\" must be system, light or dark"_s;
        return {};
    }
    m_window.setAppearance(parsed);
    return value;
}

QJsonValue DebugServer::log(const QJsonObject &args, QString *error)
{
    const int lines = std::clamp(args.value("lines"_L1).toInt(50), 1, protocol::kLogMaxLines);
    QFile file(m_logDir + u"/selecta.log"_s);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = u"can't read the log: "_s + file.errorString();
        return {};
    }
    const QList<QByteArray> all = file.readAll().split('\n');
    QJsonArray tail;
    const qsizetype end = (!all.isEmpty() && all.last().isEmpty()) ? all.size() - 1 : all.size();
    for (qsizetype i = std::max<qsizetype>(0, end - lines); i < end; ++i) {
        tail.append(QString::fromUtf8(all.at(i)));
    }
    return tail;
}

} // namespace selecta::app::devtools
