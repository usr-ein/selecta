// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"
#include "library/InboxStore.h"

#include <QImage>
#include <QWidget>

#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

// The inspector beside the Inbox (PRD DL-4, META-4): the selected item's cover and the tags in its file, where they
// came from (the release, the user, the file's own), where the audio came from, and the actions that change them:
// Edit Tags and Choose Release. The file is read on a worker; a selection change drops a read in flight. Main thread.
class InboxInspector : public QWidget
{
    Q_OBJECT

public:
    explicit InboxInspector(core::EngineHost &engine, QWidget *parent = nullptr);

    // The item to show (nullopt: the empty page), and the Inbox folder it lives in.
    void showItem(const std::optional<library::InboxItem> &item, const QString &inboxRoot);
    std::optional<qint64> itemId() const;

    // What a read of the file found: its tags and a cover thumbnail.
    struct FileTags
    {
        bool ok = false;
        QString error;
        convert::MetadataModel tags;
        QImage cover;
    };
    // Reads an audio file's tags and cover (bounded), for the inspector. Blocking; call from a worker.
    static FileTags readFileTags(const QString &path);

Q_SIGNALS:
    void previewRequested();

private:
    void showTags(const FileTags &read);
    void refreshProvenance();

    core::EngineHost &m_engine;
    std::optional<library::InboxItem> m_item;
    QString m_path;
    convert::MetadataModel m_tags;
    quint64 m_request = 0; // the read whose result is wanted
    QStackedWidget *m_pages;
    QLabel *m_cover;
    QLabel *m_title;
    QLabel *m_subtitle;
    QPushButton *m_preview;
    QPushButton *m_edit;
    QPushButton *m_release;
    QLabel *m_tagList;
    QLabel *m_provenance;
    QLabel *m_source;
    QPushButton *m_openRelease;
};

} // namespace selecta::app
