// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/SettingsView.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

#include <array>

using namespace Qt::StringLiterals;

namespace selecta::app {
namespace {

constexpr int kColumnWidth = 640;

QLabel *valueLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// A folder path: read-only and selectable in full (no clipping or wrapping), with a button to reveal it in Finder.
QWidget *folderRow(const QString &path, const QString &objectName, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *field = new QLineEdit(path, row);
    field->setObjectName(objectName);
    field->setReadOnly(true);
    field->setToolTip(path);
    field->setCursorPosition(0);
    auto *show = new QPushButton(SettingsView::tr("Show in Finder"), row);
    show->setObjectName(objectName + u".show"_s);
    QObject::connect(show, &QPushButton::clicked, row,
                     [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    layout->addWidget(field, 1);
    layout->addWidget(show);
    return row;
}

} // namespace

SettingsView::SettingsView(const BuildInfo &info, Appearance appearance, QWidget *parent)
    : QWidget(parent)
    , m_appearance(new QComboBox(this))
{
    setObjectName(u"view.settings"_s);

    auto *column = new QWidget;
    column->setMaximumWidth(kColumnWidth);

    // Appearance
    auto *appearanceBox = new QGroupBox(tr("Appearance"), column);
    auto *appearanceForm = new QFormLayout(appearanceBox);
    m_appearance->setObjectName(u"settings.appearance"_s);
    const std::array choices{std::pair{Appearance::System, tr("System")}, std::pair{Appearance::Light, tr("Light")},
                             std::pair{Appearance::Dark, tr("Dark")}};
    for (const auto &[value, label] : choices)
        m_appearance->addItem(label, static_cast<int>(value));
    setAppearance(appearance);
    appearanceForm->addRow(tr("Theme:"), m_appearance);
    connect(m_appearance, &QComboBox::activated, this, [this](int index) {
        Q_EMIT appearanceRequested(static_cast<Appearance>(m_appearance->itemData(index).toInt()));
    });

    // Data
    auto *dataBox = new QGroupBox(tr("Data"), column);
    auto *dataForm = new QFormLayout(dataBox);
    dataForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    dataForm->addRow(tr("Library and settings:"), folderRow(info.dataDir, u"settings.dataDir"_s, dataBox));
    dataForm->addRow(tr("Logs:"), folderRow(info.logDir, u"settings.logDir"_s, dataBox));

    // About this build
    auto *aboutBox = new QGroupBox(tr("About this build"), column);
    auto *aboutForm = new QFormLayout(aboutBox);
    aboutForm->addRow(tr("Version:"), valueLabel(info.version, aboutBox));
    aboutForm->addRow(tr("Build:"), valueLabel(info.buildType, aboutBox));
    aboutForm->addRow(tr("Qt:"), valueLabel(info.qtVersion, aboutBox));
    aboutForm->addRow(tr("Session:"), valueLabel(QString::number(info.session), aboutBox));
    auto *network =
        valueLabel(info.liveServerCompiledIn ? tr("Live network allowed") : tr("Local test servers only"), aboutBox);
    network->setObjectName(u"settings.networkScope"_s);
    network->setToolTip(info.liveServerCompiledIn
                            ? tr("This build can reach the live Soulseek network when you allow it.")
                            : tr("This build can't reach the live Soulseek network, only test servers on this Mac or "
                                 "the local network."));
    aboutForm->addRow(tr("Network:"), network);

    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(24, 20, 24, 24);
    columnLayout->setSpacing(16);
    columnLayout->addWidget(appearanceBox);
    columnLayout->addWidget(dataBox);
    columnLayout->addWidget(aboutBox);
    columnLayout->addStretch(1);

    auto *centered = new QWidget;
    auto *centeredLayout = new QHBoxLayout(centered);
    centeredLayout->setContentsMargins(0, 0, 0, 0);
    centeredLayout->addStretch(1);
    centeredLayout->addWidget(column, 100);
    centeredLayout->addStretch(1);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(u"settings.scroll"_s);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(centered);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void SettingsView::setAppearance(Appearance appearance)
{
    const QSignalBlocker blocker(m_appearance);
    const int index = m_appearance->findData(static_cast<int>(appearance));
    if (index >= 0)
        m_appearance->setCurrentIndex(index);
}

} // namespace selecta::app
