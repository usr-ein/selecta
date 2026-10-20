// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchFilter.h"

#include <QList>
#include <QObject>
#include <QString>

#include <optional>

namespace selecta::app {

class AppSettings;

// The filter presets (PRD SRCH-8) the Search view, Get tracks and the Wishlist pick from: Selecta's own, which can't
// be changed, then the user's, saved from the Search view's filters; one of them is the default. Kept in the settings;
// views follow changed(). Main thread only.
class Presets : public QObject
{
    Q_OBJECT

public:
    static constexpr int kMaxNameLength = 80;

    explicit Presets(AppSettings &settings, QObject *parent = nullptr);

    QList<core::FilterPreset> all() const;
    std::optional<core::FilterPreset> byId(const QString &id) const;
    // The preset by id; the default one when the id is unknown (a preset deleted meanwhile).
    core::FilterPreset preset(const QString &id) const;
    QString defaultId() const;
    void setDefaultId(const QString &id);

    // Saves a filter under a name. A preset of the user's with that name (any case) is replaced; Selecta's names can't
    // be taken. Returns the preset's id, or nullopt for an empty or taken name.
    std::optional<QString> save(const QString &name, const core::SearchFilter &filter);
    // The user's presets only: Selecta's stay as they are. False when the name is empty or taken.
    bool rename(const QString &id, const QString &name);
    void remove(const QString &id);

    // The preset Get tracks finds files with (BATCH-2).
    QString getTracksId() const;
    void setGetTracksId(const QString &id);

Q_SIGNALS:
    void changed();

private:
    bool nameTaken(const QString &name, const QString &exceptId) const;

    AppSettings &m_settings;
};

} // namespace selecta::app
