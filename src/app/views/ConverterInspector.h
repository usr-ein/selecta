// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/ConverterService.h"

#include <QWidget>

#include <optional>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace selecta::app {

// The inspector beside the Converter (PRD CONV-4, CONV-5, CONV-6): for the selected file, how each tag is written in
// the new format (Vorbis comment, ID3v2.3 or 2.4 frame, MP4 atom), which fields its folder and name add, where the
// cover comes from, and every quality notice in full. Main thread only.
class ConverterInspector : public QWidget
{
    Q_OBJECT

public:
    explicit ConverterInspector(QWidget *parent = nullptr);

    void showItem(const std::optional<core::ConverterItem> &item, const core::ConverterOptions &options);
    // The tag a field is written as: "TIT2", "TYER" (ID3v2.3) or "TDRC" (2.4), "TXXX:CATALOGNUMBER", the Vorbis
    // comment name for FLAC, or the MP4 atom for ALAC ("©nam", "----:com.apple.iTunes:LABEL").
    static QString writtenAs(const QString &key, const core::ConverterOptions &options);

private:
    QStackedWidget *m_pages;
    QLabel *m_title;
    QLabel *m_subtitle;
    QLabel *m_cover;
    QLabel *m_notices;
    QLabel *m_tags;
    QLabel *m_explain;
    QPushButton *m_reveal;
    QString m_output;
};

} // namespace selecta::app
