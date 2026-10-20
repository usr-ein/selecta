// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/QualityCheck.h"
#include "convert/MetadataModel.h"
#include "library/InboxStore.h"

#include <QImage>
#include <QWidget>

#include <memory>
#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace selecta::audio {
class AnalysisCache;
}
namespace selecta::core {
class EngineHost;
}

namespace selecta::app {

class SpectrogramView;

// The inspector beside the Inbox (PRD DL-4, META-4, PREV-9): the selected item's cover and the tags in its file, where
// they came from (the release, the user, the file's own), where the audio came from and what its spectrum says, and the
// actions that change them: Edit Tags and Choose Release. The file and the cached spectrum are read on workers; a
// selection change drops reads in flight. Main thread.
class InboxInspector : public QWidget
{
    Q_OBJECT

public:
    explicit InboxInspector(core::EngineHost &engine, QWidget *parent = nullptr);

    // Where the arrival kept the spectrum checks' reports and spectrograms (null: the findings alone are shown). Shared
    // with the workers that read it.
    void setAnalysisCache(std::shared_ptr<audio::AnalysisCache> cache);
    // The item to show (nullopt: the empty page), and the Inbox folder it lives in.
    void showItem(const std::optional<library::InboxItem> &item, const QString &inboxRoot);
    std::optional<qint64> itemId() const;
    // What to say about the shown item beyond its tags: the arrival's warning (DL-10) and the albums a single is also
    // on (DL-8). Call after showItem().
    void setAlsoOn(const QStringList &albums);

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
    // The Spectrum section from what the item keeps, then from its cached report (nullopt: not in the cache).
    void showSpectrum();
    void showReport(const std::optional<audio::QualityReport> &report);

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
    QLabel *m_alsoOn;
    QLabel *m_source;
    QPushButton *m_openRelease;
    std::shared_ptr<audio::AnalysisCache> m_cache;
    QWidget *m_spectrum;
    QLabel *m_spectrumText;
    SpectrogramView *m_spectrogram;
    QLabel *m_spectrumNote;
};

} // namespace selecta::app
