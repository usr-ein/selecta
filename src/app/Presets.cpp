// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Presets.h"

#include "app/AppSettings.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::app {

Presets::Presets(AppSettings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{}

QList<core::FilterPreset> Presets::all() const
{
    return m_settings.presets();
}

std::optional<core::FilterPreset> Presets::byId(const QString &id) const
{
    return m_settings.presetById(id);
}

core::FilterPreset Presets::preset(const QString &id) const
{
    if (std::optional<core::FilterPreset> found = byId(id)) {
        return *found;
    }
    return byId(defaultId()).value_or(core::builtInPresets().constFirst());
}

QString Presets::defaultId() const
{
    return m_settings.defaultPresetId();
}

void Presets::setDefaultId(const QString &id)
{
    if (byId(id) && id != defaultId()) {
        m_settings.setDefaultPresetId(id);
        Q_EMIT changed();
    }
}

bool Presets::nameTaken(const QString &name, const QString &exceptId) const
{
    const QList<core::FilterPreset> presets = all();
    return std::ranges::any_of(presets, [&](const core::FilterPreset &preset) {
        return preset.id != exceptId && preset.name.compare(name, Qt::CaseInsensitive) == 0;
    });
}

std::optional<QString> Presets::save(const QString &name, const core::SearchFilter &filter)
{
    const QString trimmed = name.simplified().left(kMaxNameLength);
    if (trimmed.isEmpty()) {
        return std::nullopt;
    }
    QList<core::FilterPreset> mine = m_settings.userPresets();
    const auto same = std::ranges::find_if(
        mine, [&](const core::FilterPreset &preset) { return preset.name.compare(trimmed, Qt::CaseInsensitive) == 0; });
    QString id;
    if (same != mine.end()) {
        same->filter = filter; // saving under a name of one's own replaces it
        id = same->id;
    } else {
        if (nameTaken(trimmed, {})) {
            return std::nullopt; // one of Selecta's
        }
        int next = 1;
        for (const core::FilterPreset &preset : std::as_const(mine)) {
            next = std::max(next, preset.id.mid(5).toInt() + 1); // "user-<n>"
        }
        id = u"user-%1"_s.arg(next);
        mine.append(core::FilterPreset{.id = id, .name = trimmed, .filter = filter, .builtIn = false});
    }
    m_settings.setUserPresets(mine);
    Q_EMIT changed();
    return id;
}

bool Presets::rename(const QString &id, const QString &name)
{
    const QString trimmed = name.simplified().left(kMaxNameLength);
    if (trimmed.isEmpty() || nameTaken(trimmed, id)) {
        return false;
    }
    QList<core::FilterPreset> mine = m_settings.userPresets();
    const auto it = std::ranges::find(mine, id, &core::FilterPreset::id);
    if (it == mine.end()) {
        return false;
    }
    it->name = trimmed;
    m_settings.setUserPresets(mine);
    Q_EMIT changed();
    return true;
}

void Presets::remove(const QString &id)
{
    QList<core::FilterPreset> mine = m_settings.userPresets();
    if (mine.removeIf([&id](const core::FilterPreset &preset) { return preset.id == id; }) == 0) {
        return;
    }
    m_settings.setUserPresets(mine);
    Q_EMIT changed(); // the default and Get tracks' choice fall back by themselves (AppSettings)
}

QString Presets::getTracksId() const
{
    return m_settings.getTracksPresetId();
}

void Presets::setGetTracksId(const QString &id)
{
    if (byId(id) && id != getTracksId()) {
        m_settings.setGetTracksPresetId(id);
        Q_EMIT changed();
    }
}

} // namespace selecta::app
