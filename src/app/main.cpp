// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "base/DebugProtocol.h"
#include "base/Logging.h"
#include "base/Paths.h"
#include "base/Version.h"
#include "core/EngineHost.h"
#include "governor/ServerGuard.h"
#include "meta/KeychainTokenStore.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"

#ifdef SELECTA_DEVTOOLS
#include "app/devtools/DebugServer.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDesktopServices>
#include <QImageReader>
#include <QLockFile>
#include <QLoggingCategory>
#include <QMessageBox>

#include <cstdio>
#include <memory>

using namespace Qt::StringLiterals;
namespace paths = selecta::base::paths;

namespace {
Q_LOGGING_CATEGORY(lcApp, "selecta.app")
} // namespace

namespace {

struct Options
{
    bool isolated = false;        // --data-dir: nothing outside that folder, not even the Keychain
    bool metadataNetwork = false; // --metadata-network: let an isolated run reach Spotify, Deezer and the rest
    bool headless = false;
    bool debugServer = false;
    QString debugSocket;
};

Options parseOptions(const QApplication &app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(u"Selecta, a quality-first Soulseek client."_s);
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dataDir(u"data-dir"_s,
                                     u"Keep the database, settings, logs and caches under <dir> instead of the "
                                     u"standard locations (for isolated test runs)."_s,
                                     u"dir"_s);
    const QCommandLineOption headless(u"headless"_s,
                                      u"Lay out and render the window without showing it on screen (with "
                                      u"--debug-server, for automated inspection)."_s);
    const QCommandLineOption debugServer(u"debug-server"_s,
                                         u"Accept inspection commands from `selecta-cli app` on a local socket."_s);
    const QCommandLineOption debugSocket(u"debug-socket"_s, u"Name of the debug socket."_s, u"name"_s,
                                         QString(selecta::base::debugprotocol::kSocketName));
    const QCommandLineOption metadataNetwork(
        u"metadata-network"_s,
        u"With --data-dir, still look releases up on Spotify, Deezer and MusicBrainz (isolated runs don't)."_s);
    parser.addOptions({dataDir, headless, debugServer, debugSocket, metadataNetwork});
    parser.process(app);

    if (parser.isSet(dataDir)) {
        paths::setRootOverride(parser.value(dataDir));
    }
    return {parser.isSet(dataDir), parser.isSet(metadataNetwork), parser.isSet(headless), parser.isSet(debugServer),
            parser.value(debugSocket)};
}

// Tells the user why Selecta can't start: a dialog normally, stderr when headless.
void reportFatal(const QString &title, const QString &detail, bool headless)
{
    qCCritical(lcApp).noquote() << title << '-' << detail;
    if (headless) {
        if (std::fputs(qPrintable(title + u": "_s + detail + u'\n'), stderr) == EOF) {
            // stderr is closed: the log file above already has the message
        }
    } else {
        QMessageBox::critical(nullptr, title, detail);
    }
}

QString migrationMessage(const selecta::storage::MigrationResult &result)
{
    using selecta::storage::MigrationStatus;
    switch (result.status) {
    case MigrationStatus::DatabaseTooNew:
        return QApplication::translate("main", "This library was last used by a newer version of Selecta (%1). "
                                               "Install that version or newer to open it.")
            .arg(result.error);
    case MigrationStatus::Ok:
    case MigrationStatus::Failed:
    case MigrationStatus::InvalidList:
        break;
    }
    return QApplication::translate("main", "Selecta couldn't update its library: %1").arg(result.error);
}

QString buildType()
{
#ifdef QT_NO_DEBUG
    return u"Release"_s;
#else
    return u"Debug"_s;
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    const QApplication app(argc, argv);
    // Cover art comes from untrusted files and services: cap what any single image decode may allocate (process-wide;
    // the artwork code also checks dimensions before decoding).
    QImageReader::setAllocationLimit(256);
    QApplication::setApplicationName(u"Selecta"_s);
    QApplication::setApplicationDisplayName(u"Selecta"_s);
    QApplication::setOrganizationDomain(u"usr-ein.github.io"_s);
    QApplication::setApplicationVersion(QString(selecta::base::kVersion));

    const Options options = parseOptions(app);

    const QString dataDir = paths::appSupportDir();
    if (!paths::ensureDir(dataDir)) {
        reportFatal(QApplication::translate("main", "Selecta can't start"),
                    QApplication::translate("main", "It can't create its data folder: %1").arg(dataDir),
                    options.headless);
        return 1;
    }

    // One Selecta per data folder (and per account on a machine, etiquette note §9.1). A lock left by a crashed
    // process is detected through its PID and taken over.
    QLockFile lock(dataDir + u"/selecta.lock"_s);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        reportFatal(QApplication::translate("main", "Selecta is already running"),
                    QApplication::translate("main", "Another Selecta is using %1.").arg(dataDir), options.headless);
        return 1;
    }

    if (!selecta::base::installFileLogger(paths::logDir())) {
        qCWarning(lcApp) << "can't write the log file in" << paths::logDir();
    }
    qCInfo(lcApp).noquote() << u"Selecta %1 starting; Qt %2 on %3; data in %4"_s.arg(
        QString(selecta::base::kVersion), QString::fromLatin1(qVersion()), QGuiApplication::platformName(), dataDir);

    selecta::storage::Database database;
    if (!database.open(paths::databaseFile())) {
        reportFatal(QApplication::translate("main", "Selecta can't open its library"), database.lastError(),
                    options.headless);
        return 1;
    }
    const selecta::storage::MigrationResult migration =
        selecta::storage::migrate(database, selecta::storage::selectaMigrations());
    if (!migration.ok()) {
        reportFatal(QApplication::translate("main", "Selecta can't open its library"), migrationMessage(migration),
                    options.headless);
        return 1;
    }

    selecta::storage::SessionLog sessions(database);
    const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
    const int session = sessions.start(startedAt);
    if (session < 0) {
        qCWarning(lcApp) << "couldn't record the session:" << database.lastError();
    }

    selecta::app::AppSettings settings(paths::settingsFile());
    selecta::app::applyAppearance(settings.appearance());

    const selecta::app::BuildInfo build{
        .version = QString(selecta::base::kVersion),
        .qtVersion = QString::fromLatin1(qVersion()),
        .buildType = buildType(),
        .liveServerCompiledIn = selecta::governor::liveServerCompiledIn(),
        .session = session,
        .dataDir = dataDir,
        .logDir = paths::logDir(),
    };
    // The engine thread: governor, network, metadata and the arrival pipeline. It opens its own database connection.
    selecta::core::EngineOptions engineOptions;
    engineOptions.databasePath = paths::databaseFile();
    engineOptions.startPaused = settings.networkPaused();
    engineOptions.library = settings.libraryConfig();
    engineOptions.network = settings.networkConfig();
    if (!options.isolated) {
        engineOptions.tokenStore = [] {
            return std::make_unique<selecta::meta::KeychainTokenStore>(u"io.github.usr-ein.Selecta"_s);
        };
    } else if (!options.metadataNetwork) {
        engineOptions.metaEndpoints = selecta::core::MetaEndpoints::unreachable(); // nothing leaves the machine
    } // isolated runs keep secrets in memory only
    engineOptions.openUrl = [](const QUrl &url) {
        // Called on the engine thread; the browser is opened from the UI thread.
        QMetaObject::invokeMethod(qApp, [url] { QDesktopServices::openUrl(url); }, Qt::QueuedConnection);
        return true;
    };
    if (!paths::ensureDir(engineOptions.library.inboxRoot)) {
        qCWarning(lcApp) << "can't create the Inbox folder" << engineOptions.library.inboxRoot;
    }
    selecta::core::EngineHost engine(engineOptions);
    QObject::connect(&engine, &selecta::core::EngineHost::started, &engine,
                     [headless = options.headless](bool ok, const QString &error) {
                         if (!ok) {
                             reportFatal(QApplication::translate("main", "Selecta's network engine didn't start"),
                                         error, headless);
                         }
                     });
    engine.start();

    selecta::app::MainWindow window({database, sessions, settings, engine, build, startedAt});
    if (options.headless) {
        window.setAttribute(Qt::WA_DontShowOnScreen);
    }
    window.show();

#ifdef SELECTA_DEVTOOLS
    std::unique_ptr<selecta::app::devtools::DebugServer> debugServer;
    if (options.debugServer) {
        debugServer = std::make_unique<selecta::app::devtools::DebugServer>(window, paths::logDir());
        if (!debugServer->listen(options.debugSocket)) {
            qCWarning(lcApp) << "the debug server isn't available";
        }
    }
#else
    if (options.debugServer)
        qCWarning(lcApp) << "--debug-server is ignored: this build has no developer tools";
#endif

    const int exitCode = QApplication::exec();
    engine.stop();

    if (session > 0 && !sessions.end(session, QDateTime::currentMSecsSinceEpoch())) {
        qCWarning(lcApp) << "couldn't close session" << session << ':' << database.lastError();
    }
    qCInfo(lcApp) << "Selecta quit with code" << exitCode;
    selecta::base::uninstallFileLogger();
    return exitCode;
}
