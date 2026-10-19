// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/AppSettings.h"

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

const QString kAppearance = u"appearance"_s;
const QString kGeometry = u"window/geometry"_s;
const QString kSplitterPrefix = u"window/splitter/"_s;
const QString kSidebar = u"window/sidebarShown"_s;
const QString kInspector = u"window/inspectorShown"_s;
const QString kLastView = u"window/lastView"_s;

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

bool AppSettings::sync()
{
    m_settings.sync();
    return m_settings.status() == QSettings::NoError;
}

} // namespace selecta::app
