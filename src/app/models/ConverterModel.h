// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ConverterService.h"

#include <QAbstractTableModel>
#include <QList>

#include <optional>

namespace selecta::app {

// The files in the converter (PRD CONV-3, CONV-6): each with its quality, what converting it gives (or the warning
// that asks first), its progress and where the new file went. "Convert anyway" is a tick box on the rows that ask.
// Main thread only.
class ConverterModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { // NOLINT(cppcoreguidelines-use-enum-class): Qt columns are plain ints
        FileColumn,
        QualityColumn,
        ResultColumn,
        AnywayColumn,
        ColumnCount
    };
    static constexpr int ItemIdRole = Qt::UserRole + 1; // qint64

    explicit ConverterModel(QObject *parent = nullptr);

    void setItems(const QList<core::ConverterItem> &items);
    const QList<core::ConverterItem> &items() const { return m_items; }
    std::optional<core::ConverterItem> itemAt(int row) const;
    int rowOf(qint64 id) const;
    // "Converting · 42 %", "Saved as 01 - Neon Rain.mp3", "No quality gain…", "Failed: …".
    static QString resultText(const core::ConverterItem &item);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

Q_SIGNALS:
    // The user ticked or unticked "Convert anyway".
    void confirmToggled(qint64 id, bool yes);

private:
    QList<core::ConverterItem> m_items;
};

} // namespace selecta::app
