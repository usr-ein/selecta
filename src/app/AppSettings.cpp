// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/AppSettings.h"

#include "base/Paths.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

const QString kAppearance = u"appearance"_s;
const QString kGeometry = u"window/geometry"_s;
const QString kSplitterPrefix = u"window/splitter/"_s;
const QString kSidebar = u"window/sidebarShown"_s;
const QString kInspector = u"window/inspectorShown"_s;
const QString kLastView = u"window/lastView"_s;
const QString kNetworkPaused = u"network/paused"_s;
const QString kPreviewVolume = u"player/volume"_s;
const QString kInboxDir = u"library/inbox"_s;
const QString kCollectionRoots = u"library/collections"_s;
const QString kSinglesFolder = u"library/singlesFolder"_s;
const QString kAlbumsFolder = u"library/albumsFolder"_s;
const QString kSingleTemplate = u"filing/single"_s;
const QString kAlbumFolderTemplate = u"filing/albumFolder"_s;
const QString kAlbumTrackTemplate = u"filing/albumTrack"_s;
const QString kCleanNames = u"filing/clean"_s;
const QString kSeparator = u"filing/separator"_s;
const QString kServerHost = u"soulseek/host"_s;
const QString kServerPort = u"soulseek/port"_s;
const QString kUsername = u"soulseek/username"_s;
const QString kListenPort = u"soulseek/listenPort"_s;
const QString kConnectAtLaunch = u"soulseek/connectAtLaunch"_s;
const QString kLiveAllowed = u"soulseek/liveAllowed"_s;

quint16 portFrom(const QVariant &value, quint16 fallback)
{
    bool ok = false;
    const int port = value.toInt(&ok);
    return ok && port >= 0 && port <= 65535 ? static_cast<quint16>(port) : fallback;
}

// A folder name typed in Settings: one plain component, or the default.
QString folderName(const QString &value, const QString &fallback)
{
    QString name = value.trimmed();
    if (name.isEmpty() || name == u"." || name == u".." || name.contains(u'/') || name.contains(u'\\')) {
        return fallback;
    }
    return name;
}

} // namespace

AppSettings::AppSettings(const QString &iniPath)
    : m_settings(iniPath, QSettings::IniFormat)
{}

Appearance AppSettings::appearance() const
{
    return appearanceFromKey(m_settings.value(kAppearance).toString());
}

void AppSettings::setAppearance(Appearance appearance)
{
    m_settings.setValue(kAppearance, appearanceKey(appearance));
}

QByteArray AppSettings::windowGeometry() const
{
    return m_settings.value(kGeometry).toByteArray();
}

void AppSettings::setWindowGeometry(const QByteArray &geometry)
{
    m_settings.setValue(kGeometry, geometry);
}

QByteArray AppSettings::splitterState(const QString &name) const
{
    return m_settings.value(kSplitterPrefix + name).toByteArray();
}

void AppSettings::setSplitterState(const QString &name, const QByteArray &state)
{
    m_settings.setValue(kSplitterPrefix + name, state);
}

bool AppSettings::sidebarShown() const
{
    return m_settings.value(kSidebar, true).toBool();
}

void AppSettings::setSidebarShown(bool shown)
{
    m_settings.setValue(kSidebar, shown);
}

bool AppSettings::inspectorShown() const
{
    return m_settings.value(kInspector, true).toBool();
}

void AppSettings::setInspectorShown(bool shown)
{
    m_settings.setValue(kInspector, shown);
}

QString AppSettings::lastView() const
{
    return m_settings.value(kLastView).toString();
}

void AppSettings::setLastView(const QString &id)
{
    m_settings.setValue(kLastView, id);
}

bool AppSettings::networkPaused() const
{
    return m_settings.value(kNetworkPaused, false).toBool();
}

void AppSettings::setNetworkPaused(bool paused)
{
    m_settings.setValue(kNetworkPaused, paused);
}

double AppSettings::previewVolume() const
{
    bool ok = false;
    const double volume = m_settings.value(kPreviewVolume, 0.8).toDouble(&ok);
    return ok && std::isfinite(volume) ? std::clamp(volume, 0.0, 1.0) : 0.8;
}

void AppSettings::setPreviewVolume(double volume)
{
    m_settings.setValue(kPreviewVolume, std::clamp(volume, 0.0, 1.0));
}

core::LibraryConfig AppSettings::libraryConfig() const
{
    core::LibraryConfig config;
    const QString inbox = m_settings.value(kInboxDir).toString();
    config.inboxRoot = inbox.isEmpty() ? base::paths::defaultInboxDir() : inbox;
    config.collectionRoots = m_settings.value(kCollectionRoots).toStringList();
    config.collectionRoots.removeAll(QString());
    config.singlesFolder = folderName(m_settings.value(kSinglesFolder).toString(), config.singlesFolder);
    config.albumsFolder = folderName(m_settings.value(kAlbumsFolder).toString(), config.albumsFolder);
    const auto templ = [this](const QString &key, const QString &fallback) {
        const QString value = m_settings.value(key).toString().trimmed();
        return value.isEmpty() ? fallback : value;
    };
    config.templates.single = templ(kSingleTemplate, config.templates.single);
    config.templates.albumFolder = templ(kAlbumFolderTemplate, config.templates.albumFolder);
    config.templates.albumTrack = templ(kAlbumTrackTemplate, config.templates.albumTrack);
    config.cleaner.enabled = m_settings.value(kCleanNames, true).toBool();
    const QString separator = m_settings.value(kSeparator).toString();
    if (separator.size() == 1 && !separator.at(0).isLetterOrNumber() && separator != u"/" && separator != u"\\") {
        config.cleaner.separator = separator.at(0);
    }
    return config;
}

void AppSettings::setLibraryConfig(const core::LibraryConfig &config)
{
    m_settings.setValue(kInboxDir, config.inboxRoot);
    m_settings.setValue(kCollectionRoots, config.collectionRoots);
    m_settings.setValue(kSinglesFolder, config.singlesFolder);
    m_settings.setValue(kAlbumsFolder, config.albumsFolder);
    m_settings.setValue(kSingleTemplate, config.templates.single);
    m_settings.setValue(kAlbumFolderTemplate, config.templates.albumFolder);
    m_settings.setValue(kAlbumTrackTemplate, config.templates.albumTrack);
    m_settings.setValue(kCleanNames, config.cleaner.enabled);
    m_settings.setValue(kSeparator, QString(config.cleaner.separator));
}

core::NetworkConfig AppSettings::networkConfig() const
{
    core::NetworkConfig config;
    const QString host = m_settings.value(kServerHost).toString().trimmed();
    if (!host.isEmpty()) {
        config.host = host;
    }
    config.port = portFrom(m_settings.value(kServerPort), config.port);
    config.username = m_settings.value(kUsername).toString().trimmed();
    config.listenPort = portFrom(m_settings.value(kListenPort), config.listenPort);
    config.connectAtLaunch = m_settings.value(kConnectAtLaunch, config.connectAtLaunch).toBool();
    config.liveAllowedByUser = m_settings.value(kLiveAllowed, false).toBool();
    return config;
}

void AppSettings::setNetworkConfig(const core::NetworkConfig &config)
{
    m_settings.setValue(kServerHost, config.host);
    m_settings.setValue(kServerPort, config.port);
    m_settings.setValue(kUsername, config.username);
    m_settings.setValue(kListenPort, config.listenPort);
    m_settings.setValue(kConnectAtLaunch, config.connectAtLaunch);
    m_settings.setValue(kLiveAllowed, config.liveAllowedByUser);
}

bool AppSettings::sync()
{
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

} // namespace selecta::app
