// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchResults.h"

#include <QWidget>

#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace selecta::app {

// The inspector beside the Search view: the selected result's quality check, its fit to the release, what the user
// already has, and how much was taken from its peer today (PRD SRCH-6, SRCH-7, SRCH-11, FAIR-1). Main thread only.
class SearchInspector : public QWidget
{
    Q_OBJECT

public:
    explicit SearchInspector(QWidget *parent = nullptr);

    // The result to show (nullopt: the empty page), and the chosen release's length in ms (0: none or unknown).
    void showResult(const std::optional<core::SearchResult> &result, qint64 releaseDurationMs = 0);
    std::optional<int> resultIndex() const;

Q_SIGNALS:
    void previewRequested();
    void downloadRequested();
    void downloadFolderRequested();

private:
    QStackedWidget *m_pages;
    QLabel *m_title;
    QLabel *m_subtitle;
    QPushButton *m_preview;
    QPushButton *m_download;
    QPushButton *m_downloadFolder;
    QLabel *m_ownership;
    QLabel *m_checks;
    QLabel *m_quality;
    QLabel *m_file;
    QLabel *m_peer;
    std::optional<int> m_index;
};

} // namespace selecta::app
