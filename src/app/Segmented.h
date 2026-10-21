// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QTabWidget;

namespace selecta::app {

// A segmented control as macOS 26 draws one (mock: .seg): a capsule track, the chosen segment raised on it. Each segment
// is a button the keyboard reaches; one is chosen at a time. Main thread only.
class SegmentedControl : public QWidget
{
    Q_OBJECT

public:
    explicit SegmentedControl(QWidget *parent = nullptr);

    // Adds a segment at the end; returns its index. The first one added is chosen.
    int addSegment(const QString &text, const QString &objectName = {});
    int count() const;
    int currentIndex() const;
    // Chooses a segment without emitting currentChanged; out of range changes nothing.
    void setCurrentIndex(int index);

    // Stands in for a tab widget's tabs (macOS 26 chooses pages with a segmented control): one segment per tab, named
    // `<namePrefix>.<index>`, following the tab widget both ways; the tab bar is hidden. Returns the control, a child of
    // `parent`, for the caller to place. Tabs added later aren't covered.
    static SegmentedControl *forTabs(QTabWidget *tabs, const QString &namePrefix, QWidget *parent);

Q_SIGNALS:
    // The user chose another segment.
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QHBoxLayout *m_layout;
    QButtonGroup *m_group;
};

} // namespace selecta::app
