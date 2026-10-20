// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/AppSettings.h"
#include "core/ConverterService.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class ConverterInspector;
class ConverterModel;

// The Converter (PRD CONV-1…7): files dropped here or added (from anywhere, the Inbox's "Convert…" too) become new
// files in the format chosen, next to the originals or in a chosen folder, with their tags mapped and the folder's
// cover embedded. Before converting, each row says what it gives or warns why it gains nothing; those need "Convert
// anyway". Originals are never changed. The engine converts; this view shows it. Main thread only.
class ConverterView : public QWidget
{
    Q_OBJECT

public:
    ConverterView(core::EngineHost &engine, AppSettings &settings, QWidget *parent = nullptr);

    ConverterInspector *inspectorPanel() const { return m_inspector; }
    ConverterModel *model() const { return m_model; }
    // Adds files, and the audio files in folders.
    void addPaths(const QStringList &paths);
    QString summary() const;
    // The choices made in the bar, as the converter takes them.
    core::ConverterOptions options() const;
    // "mp3-320" → MP3 320 kbps CBR, … (AppSettings::ConverterState::target).
    static convert::EncodeSettings targetFor(const QString &id);

Q_SIGNALS:
    void summaryChanged();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QWidget *createBar();
    void optionsEdited();
    void chooseFolder();
    void addFiles();
    void refresh();
    void showSelection();
    void contextMenu(QPoint position);
    QList<qint64> selectedIds() const;

    core::EngineHost &m_engine;
    AppSettings &m_settings;
    ConverterModel *m_model;
    ConverterInspector *m_inspector;
    QComboBox *m_target;
    QComboBox *m_id3;
    QComboBox *m_artwork;
    QComboBox *m_destination;
    QPushButton *m_add;
    QPushButton *m_convert;
    QPushButton *m_stop;
    QLabel *m_status;
    QStackedWidget *m_pages;
    QTreeView *m_table;
    QString m_outputFolder;
};

} // namespace selecta::app
