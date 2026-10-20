// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/SettingsView.h"

#include "app/Format.h"
#include "library/Naming.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
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

QChar separatorOf(const QComboBox *box)
{
    const QString value = box->currentData().toString();
    return value.isEmpty() ? QChar(u'_') : value.at(0);
}

} // namespace

SettingsView::SettingsView(const BuildInfo &info, Appearance appearance, core::LibraryConfig library,
                           core::NetworkConfig networkConfig, QWidget *parent)
    : QWidget(parent)
    , m_appearance(new QComboBox(this))
    , m_library(std::move(library))
    , m_network(std::move(networkConfig))
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
    for (const auto &[value, label] : choices) {
        m_appearance->addItem(label, static_cast<int>(value));
    }
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
    columnLayout->addWidget(createLibraryBox(column));
    columnLayout->addWidget(createFilingBox(column));
    columnLayout->addWidget(createNetworkBox(column));
    columnLayout->addWidget(createSpotifyBox(column));
    columnLayout->addWidget(createPreviewBox(column));
    columnLayout->addWidget(createCacheBox(column));
    columnLayout->addWidget(dataBox);
    columnLayout->addWidget(aboutBox);
    columnLayout->addStretch(1);

    auto *centered = new QWidget;
    centered->setObjectName(u"settings.page"_s); // the whole page, for full-length screenshots
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
    showLibrary();
}

QWidget *SettingsView::createLibraryBox(QWidget *parent)
{
    auto *box = new QGroupBox(tr("Library"), parent);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *inboxRow = new QWidget(box);
    auto *inboxLayout = new QHBoxLayout(inboxRow);
    inboxLayout->setContentsMargins(0, 0, 0, 0);
    m_inbox = new QLineEdit(inboxRow);
    m_inbox->setObjectName(u"settings.inbox"_s);
    m_inbox->setReadOnly(true);
    auto *chooseInbox = new QPushButton(tr("Choose…"), inboxRow);
    chooseInbox->setObjectName(u"settings.inbox.choose"_s);
    connect(chooseInbox, &QPushButton::clicked, this, &SettingsView::chooseInbox);
    inboxLayout->addWidget(m_inbox, 1);
    inboxLayout->addWidget(chooseInbox);
    form->addRow(tr("Inbox:"), inboxRow);

    auto *collectionRow = new QWidget(box);
    auto *collectionLayout = new QVBoxLayout(collectionRow);
    collectionLayout->setContentsMargins(0, 0, 0, 0);
    m_collections = new QListWidget(collectionRow);
    m_collections->setObjectName(u"settings.collections"_s);
    m_collections->setMaximumHeight(72);
    m_collections->setTextElideMode(Qt::ElideMiddle);
    m_collections->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_collections->setUniformItemSizes(true);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add…"), collectionRow);
    add->setObjectName(u"settings.collections.add"_s);
    auto *remove = new QPushButton(tr("Remove"), collectionRow);
    remove->setObjectName(u"settings.collections.remove"_s);
    connect(add, &QPushButton::clicked, this, &SettingsView::addCollection);
    connect(remove, &QPushButton::clicked, this, &SettingsView::removeCollection);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    auto *note = new QLabel(tr("Filing adds new files to the first folder. Selecta never changes, moves or deletes "
                               "files already in your collection."),
                            collectionRow);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    collectionLayout->addWidget(m_collections);
    collectionLayout->addLayout(buttons);
    collectionLayout->addWidget(note);
    form->addRow(tr("Collection:"), collectionRow);

    m_singlesFolder = new QLineEdit(box);
    m_singlesFolder->setObjectName(u"settings.singlesFolder"_s);
    m_albumsFolder = new QLineEdit(box);
    m_albumsFolder->setObjectName(u"settings.albumsFolder"_s);
    connect(m_singlesFolder, &QLineEdit::editingFinished, this, &SettingsView::editsFinished);
    connect(m_albumsFolder, &QLineEdit::editingFinished, this, &SettingsView::editsFinished);
    form->addRow(tr("Singles folder:"), m_singlesFolder);
    form->addRow(tr("Albums folder:"), m_albumsFolder);

    m_libraryError = new QLabel(box);
    m_libraryError->setObjectName(u"settings.libraryError"_s);
    m_libraryError->setWordWrap(true);
    m_libraryError->hide();
    form->addRow(m_libraryError);
    return box;
}

QWidget *SettingsView::createFilingBox(QWidget *parent)
{
    auto *box = new QGroupBox(tr("File names"), parent);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const QString tokens = tr("Tokens: {artist} {albumartist} {title} {album} {year} {track} {disc} {label} {key} "
                              "{bpm}. Text in [brackets] disappears when a token inside it is empty.");
    const auto templateField = [&](const QString &objectName) {
        auto *field = new QLineEdit(box);
        field->setObjectName(objectName);
        field->setToolTip(tokens);
        connect(field, &QLineEdit::editingFinished, this, &SettingsView::editsFinished);
        connect(field, &QLineEdit::textEdited, this, &SettingsView::refreshPreview);
        return field;
    };
    m_singleTemplate = templateField(u"settings.template.single"_s);
    m_albumFolderTemplate = templateField(u"settings.template.albumFolder"_s);
    m_albumTrackTemplate = templateField(u"settings.template.albumTrack"_s);
    form->addRow(tr("Singles:"), m_singleTemplate);
    form->addRow(tr("Album folders:"), m_albumFolderTemplate);
    form->addRow(tr("Album tracks:"), m_albumTrackTemplate);

    m_clean = new QCheckBox(tr("Make names path-safe"), box);
    m_clean->setObjectName(u"settings.clean"_s);
    m_clean->setToolTip(tr("Keeps letters in every language; replaces spaces, quotes, brackets and symbols such as "
                           "% $ / : with the separator; \"&\" becomes \"and\"."));
    form->addRow(QString(), m_clean);
    m_separator = new QComboBox(box);
    m_separator->setObjectName(u"settings.separator"_s);
    m_separator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_separator->addItem(tr("_ (underscore)"), u"_"_s);
    m_separator->addItem(tr("- (dash)"), u"-"_s);
    m_separator->addItem(tr(". (dot)"), u"."_s);
    m_separator->addItem(tr("space"), u" "_s);
    connect(m_clean, &QCheckBox::toggled, this, &SettingsView::editsFinished);
    connect(m_separator, &QComboBox::activated, this, &SettingsView::editsFinished);
    form->addRow(tr("Separator:"), m_separator);

    m_preview = new QLabel(box);
    m_preview->setObjectName(u"settings.preview"_s);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    form->addRow(tr("Example:"), m_preview);
    return box;
}

QWidget *SettingsView::createSpotifyBox(QWidget *parent)
{
    auto *box = new QGroupBox(tr("Spotify"), parent);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto *intro = new QLabel(tr("Tags and covers for your downloads come from Spotify. Create an app at "
                                "developer.spotify.com with the redirect URI http://127.0.0.1/callback and paste its "
                                "Client ID here. With its Client secret too, no browser sign-in is needed for tags. "
                                "Without either, Selecta uses Deezer and MusicBrainz."),
                             box);
    intro->setWordWrap(true);
    intro->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(intro);

    m_spotifyState = new QLabel(box);
    m_spotifyState->setObjectName(u"settings.spotify.state"_s);
    m_spotifyState->setWordWrap(true);
    form->addRow(tr("Status:"), m_spotifyState);

    m_spotifyClientId = new QLineEdit(box);
    m_spotifyClientId->setObjectName(u"settings.spotify.clientId"_s);
    m_spotifyClientId->setPlaceholderText(tr("32 letters and digits"));
    form->addRow(tr("Client ID:"), m_spotifyClientId);

    m_spotifySecret = new QLineEdit(box);
    m_spotifySecret->setObjectName(u"settings.spotify.secret"_s);
    m_spotifySecret->setEchoMode(QLineEdit::Password);
    m_spotifySecret->setPlaceholderText(tr("Optional"));
    form->addRow(tr("Client secret:"), m_spotifySecret);

    auto *buttons = new QHBoxLayout;
    m_spotifySave = new QPushButton(tr("Save"), box);
    m_spotifySave->setObjectName(u"settings.spotify.save"_s);
    m_spotifySignIn = new QPushButton(tr("Sign In in the Browser…"), box);
    m_spotifySignIn->setObjectName(u"settings.spotify.signIn"_s);
    m_spotifySignIn->setToolTip(tr("Needed only to import your playlists and Liked Songs"));
    m_spotifySignOut = new QPushButton(tr("Sign Out"), box);
    m_spotifySignOut->setObjectName(u"settings.spotify.signOut"_s);
    connect(m_spotifySave, &QPushButton::clicked, this, &SettingsView::submitSpotify);
    connect(m_spotifyClientId, &QLineEdit::returnPressed, this, &SettingsView::submitSpotify);
    connect(m_spotifySecret, &QLineEdit::returnPressed, this, &SettingsView::submitSpotify);
    connect(m_spotifySignIn, &QPushButton::clicked, this, &SettingsView::signInToSpotify);
    connect(m_spotifySignOut, &QPushButton::clicked, this, &SettingsView::spotifySignOutRequested);
    buttons->addWidget(m_spotifySave);
    buttons->addWidget(m_spotifySignIn);
    buttons->addWidget(m_spotifySignOut);
    buttons->addStretch(1);
    form->addRow(QString(), buttons);
    setSpotifyStatus(m_spotify);
    return box;
}

QWidget *SettingsView::createCacheBox(QWidget *parent)
{
    auto *box = new QGroupBox(tr("Caches"), parent);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *note = new QLabel(tr("What Selecta keeps to save the network and your time. Emptying one only means it is "
                               "fetched or worked out again when needed."),
                            box);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);
    const auto row = [this, box, form](const QString &label, const QString &name, QLabel *&size,
                                       void (SettingsView::*clear)()) {
        auto *line = new QWidget(box);
        auto *layout = new QHBoxLayout(line);
        layout->setContentsMargins(0, 0, 0, 0);
        size = new QLabel(tr("…"), line);
        size->setObjectName(u"settings.cache."_s + name);
        auto *button = new QPushButton(tr("Empty"), line);
        button->setObjectName(u"settings.cache."_s + name + u".clear"_s);
        connect(button, &QPushButton::clicked, this, clear);
        layout->addWidget(size, 1);
        layout->addWidget(button);
        form->addRow(label, line);
    };
    row(tr("Search results:"), u"searches"_s, m_searchCache, &SettingsView::clearSearchCacheRequested);
    row(tr("Previews:"), u"previews"_s, m_previewCache, &SettingsView::clearPreviewCacheRequested);
    row(tr("Waveforms:"), u"waveforms"_s, m_waveformCache, &SettingsView::clearWaveformCacheRequested);
    return box;
}

void SettingsView::setCacheSizes(const core::CacheStatus &engine, qint64 waveformBytes)
{
    m_searchCache->setText(
        engine.searches == 1
            ? tr("1 search · %1, kept a day").arg(format::size(engine.searchBytes))
            : tr("%1 searches · %2, kept a day").arg(engine.searches).arg(format::size(engine.searchBytes)));
    m_previewCache->setText(tr("%1 of audio, at most 2 GB, each kept 30 days after it was last heard")
                                .arg(format::size(engine.previewBytes)));
    m_waveformCache->setText(tr("%1, at most 320 MB").arg(format::size(waveformBytes)));
}

QWidget *SettingsView::createPreviewBox(QWidget *parent)
{
    auto *box = new QGroupBox(tr("Previews"), parent);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *note = new QLabel(tr("Space previews a track from YouTube through your own yt-dlp, which costs Soulseek "
                               "peers nothing; without yt-dlp, from Deezer's 30-second clip. ⇧Space always asks "
                               "the peer for the very file a Get would download, K keeps it and D discards it."),
                            box);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);

    m_previewSource = new QComboBox(box);
    m_previewSource->setObjectName(u"settings.preview.source"_s);
    m_previewSource->addItem(tr("YouTube, else the 30-second clip"), static_cast<int>(core::PreviewSource::Automatic));
    m_previewSource->addItem(tr("YouTube only"), static_cast<int>(core::PreviewSource::YouTube));
    m_previewSource->addItem(tr("The 30-second clip only"), static_cast<int>(core::PreviewSource::Clip));
    m_previewSource->addItem(tr("The peer's file"), static_cast<int>(core::PreviewSource::Peer));
    connect(m_previewSource, &QComboBox::activated, this, [this](int row) {
        Q_EMIT previewSourceChanged(static_cast<core::PreviewSource>(m_previewSource->itemData(row).toInt()));
    });
    form->addRow(tr("Space previews:"), m_previewSource);

    auto *row = new QWidget(box);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    m_ytDlpPath = new QLineEdit(row);
    m_ytDlpPath->setObjectName(u"settings.preview.ytDlp"_s);
    m_ytDlpPath->setPlaceholderText(tr("Found by itself in Homebrew or your PATH"));
    m_ytDlpPath->setClearButtonEnabled(true);
    auto *choose = new QPushButton(tr("Choose…"), row);
    choose->setObjectName(u"settings.preview.ytDlp.choose"_s);
    auto *check = new QPushButton(tr("Check"), row);
    check->setObjectName(u"settings.preview.ytDlp.check"_s);
    rowLayout->addWidget(m_ytDlpPath, 1);
    rowLayout->addWidget(choose);
    rowLayout->addWidget(check);
    form->addRow(tr("yt-dlp:"), row);
    m_ytDlpState = new QLabel(tr("Not checked yet."), box);
    m_ytDlpState->setObjectName(u"settings.preview.ytDlp.state"_s);
    m_ytDlpState->setWordWrap(true);
    m_ytDlpState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QString(), m_ytDlpState);

    const auto submit = [this] {
        Q_EMIT ytDlpPathChanged(m_ytDlpPath->text().trimmed());
        m_ytDlpState->setText(tr("Checking…"));
        Q_EMIT ytDlpCheckRequested();
    };
    connect(m_ytDlpPath, &QLineEdit::editingFinished, this, [this, submit] {
        if (m_ytDlpPath->isModified()) {
            m_ytDlpPath->setModified(false);
            submit();
        }
    });
    connect(choose, &QPushButton::clicked, this, [this, submit] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Choose yt-dlp"), u"/opt/homebrew/bin"_s);
        if (!path.isEmpty()) {
            m_ytDlpPath->setText(path);
            submit();
        }
    });
    connect(check, &QPushButton::clicked, this, [this] {
        m_ytDlpState->setText(tr("Checking…"));
        Q_EMIT ytDlpCheckRequested();
    });
    return box;
}

void SettingsView::setPreviewSettings(core::PreviewSource source, const QString &ytDlpPath)
{
    const QSignalBlocker blocker(m_previewSource);
    m_previewSource->setCurrentIndex(std::max(m_previewSource->findData(static_cast<int>(source)), 0));
    m_ytDlpPath->setText(ytDlpPath);
}

void SettingsView::setYtDlpStatus(bool ok, const QString &description)
{
    m_ytDlpState->setText(ok ? description : tr("Not usable: %1").arg(description));
}

QWidget *SettingsView::createNetworkBox(QWidget *parent)
{
    auto *box = new QGroupBox(tr("Soulseek"), parent);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_networkState = new QLabel(box);
    m_networkState->setObjectName(u"settings.network.state"_s);
    m_networkState->setWordWrap(true);
    form->addRow(tr("Status:"), m_networkState);

    auto *serverRow = new QWidget(box);
    auto *serverLayout = new QHBoxLayout(serverRow);
    serverLayout->setContentsMargins(0, 0, 0, 0);
    m_networkHost = new QLineEdit(m_network.host, serverRow);
    m_networkHost->setObjectName(u"settings.network.host"_s);
    m_networkPort = new QSpinBox(serverRow);
    m_networkPort->setObjectName(u"settings.network.port"_s);
    m_networkPort->setRange(1, 65535);
    m_networkPort->setValue(m_network.port);
    serverLayout->addWidget(m_networkHost, 1);
    serverLayout->addWidget(new QLabel(tr("Port:"), serverRow));
    serverLayout->addWidget(m_networkPort);
    form->addRow(tr("Server:"), serverRow);

    m_networkUser = new QLineEdit(m_network.username, box);
    m_networkUser->setObjectName(u"settings.network.username"_s);
    form->addRow(tr("Username:"), m_networkUser);
    m_networkPassword = new QLineEdit(box);
    m_networkPassword->setObjectName(u"settings.network.password"_s);
    m_networkPassword->setEchoMode(QLineEdit::Password);
    m_networkPassword->setPlaceholderText(tr("Saved in the Keychain once entered"));
    form->addRow(tr("Password:"), m_networkPassword);

    m_listenPort = new QSpinBox(box);
    m_listenPort->setObjectName(u"settings.network.listenPort"_s);
    m_listenPort->setRange(1024, 65535);
    m_listenPort->setValue(std::max<int>(1024, m_network.listenPort));
    m_listenPort->setToolTip(tr("Other users connect to you on this port. Forward it in your router for faster "
                                "transfers; without it, connections go through the server."));
    form->addRow(tr("Listening port:"), m_listenPort);

    m_connectAtLaunch = new QCheckBox(tr("Connect when Selecta opens"), box);
    m_connectAtLaunch->setObjectName(u"settings.network.connectAtLaunch"_s);
    m_connectAtLaunch->setChecked(m_network.connectAtLaunch);
    form->addRow(QString(), m_connectAtLaunch);

    auto *buttons = new QHBoxLayout;
    m_networkSave = new QPushButton(tr("Save"), box);
    m_networkSave->setObjectName(u"settings.network.save"_s);
    m_networkConnect = new QPushButton(tr("Connect"), box);
    m_networkConnect->setObjectName(u"settings.network.connect"_s);
    connect(m_networkSave, &QPushButton::clicked, this, &SettingsView::submitNetwork);
    connect(m_networkPassword, &QLineEdit::returnPressed, this, &SettingsView::submitNetwork);
    connect(m_networkConnect, &QPushButton::clicked, this, [this] {
        const bool online = m_networkStatus.state == core::NetworkStatus::State::Online ||
                            m_networkStatus.state == core::NetworkStatus::State::Connecting ||
                            m_networkStatus.state == core::NetworkStatus::State::Reconnecting;
        if (online) {
            Q_EMIT disconnectRequested();
        } else {
            Q_EMIT connectRequested();
        }
    });
    buttons->addWidget(m_networkSave);
    buttons->addWidget(m_networkConnect);
    buttons->addStretch(1);
    form->addRow(QString(), buttons);

    auto *note = new QLabel(tr("Selecta paces every search and download to stay a good citizen of the network: see "
                               "Activity for what it sent and why."),
                            box);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);
    setNetworkStatus(m_networkStatus);
    return box;
}

void SettingsView::submitNetwork()
{
    core::NetworkConfig config = m_network;
    config.host = m_networkHost->text().trimmed();
    config.port = static_cast<quint16>(m_networkPort->value());
    config.username = m_networkUser->text().trimmed();
    config.listenPort = static_cast<quint16>(m_listenPort->value());
    config.connectAtLaunch = m_connectAtLaunch->isChecked();
    if (config.host.isEmpty()) {
        config.host = m_network.host;
    }
    const QString password = m_networkPassword->text();
    m_networkPassword->clear(); // the password doesn't stay in the window
    if (!(config == m_network)) {
        m_network = config;
        Q_EMIT networkConfigChanged(config);
    }
    if (!password.isEmpty()) {
        Q_EMIT networkPasswordSubmitted(password);
    }
}

void SettingsView::setNetworkStatus(const core::NetworkStatus &status)
{
    using State = core::NetworkStatus::State;
    m_networkStatus = status;
    QString text;
    switch (status.state) {
    case State::NotConfigured:
        text = status.detail.isEmpty() ? tr("Not set up.") : status.detail;
        break;
    case State::Offline:
        text = tr("Offline.");
        break;
    case State::Connecting:
        text = tr("Connecting…");
        break;
    case State::Online:
        text = tr("Online as %1 on %2.").arg(status.username, status.server);
        if (!status.listening) {
            text += u' ' + tr("Not listening for peers: connections go through the server.");
        }
        break;
    case State::Reconnecting:
        text = tr("%1 Trying again at %2.")
                   .arg(status.detail, QLocale().toString(QDateTime::fromMSecsSinceEpoch(status.retryAtMs).time(),
                                                          QLocale::ShortFormat));
        break;
    case State::NeedsUser:
        text = status.detail;
        break;
    }
    m_networkState->setText(text);
    const bool active =
        status.state == State::Online || status.state == State::Connecting || status.state == State::Reconnecting;
    m_networkConnect->setText(active ? tr("Disconnect") : tr("Connect"));
    m_networkConnect->setEnabled(active || status.state != State::NotConfigured);
}

void SettingsView::submitSpotify()
{
    const QString clientId = m_spotifyClientId->text().trimmed();
    const QByteArray secret = m_spotifySecret->text().toUtf8();
    m_spotifySecret->clear(); // the secret doesn't stay in the window
    const bool keepSavedSecret = secret.isEmpty() && clientId == m_spotify.clientId;
    if (clientId.isEmpty() || m_spotifyTermsAccepted) { // forgetting the credentials needs no notice
        Q_EMIT spotifyCredentialsSubmitted(clientId, secret, keepSavedSecret);
        return;
    }
    askAboutSpotifyTerms([this, clientId, secret, keepSavedSecret] {
        Q_EMIT spotifyCredentialsSubmitted(clientId, secret, keepSavedSecret);
    });
}

void SettingsView::signInToSpotify()
{
    if (m_spotifyTermsAccepted) {
        Q_EMIT spotifySignInRequested();
        return;
    }
    askAboutSpotifyTerms([this] { Q_EMIT spotifySignInRequested(); });
}

void SettingsView::askAboutSpotifyTerms(const std::function<void()> &accepted)
{
    // D30, and the wording of references/metadata-and-preview-services.md §5.3: what Selecta does with Spotify's data,
    // what Spotify's terms say about it, and what the user risks: their own developer app, not Selecta's.
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setObjectName(u"settings.spotify.terms"_s);
    box->setIcon(QMessageBox::Warning);
    box->setText(tr("Tag your downloads from Spotify?"));
    box->setInformativeText(
        tr("Selecta writes Spotify's track details and cover art into the files you download: title, artists, album, "
           "release date, track numbers, ISRC and the cover. Spotify's developer terms don't allow apps to keep its "
           "metadata or artwork in files. If Spotify objects, it can revoke the developer app (Client ID) you created "
           "for Selecta, which belongs to your Spotify account.\n\nWithout Spotify, tags come from Deezer and "
           "MusicBrainz. Selecta asks only once."));
    QPushButton *use = box->addButton(tr("Use Spotify Tags"), QMessageBox::AcceptRole);
    use->setObjectName(u"settings.spotify.terms.accept"_s);
    QPushButton *decline = box->addButton(tr("Use Deezer and MusicBrainz"), QMessageBox::RejectRole);
    decline->setObjectName(u"settings.spotify.terms.decline"_s);
    box->setDefaultButton(use);
    box->setEscapeButton(decline);
    box->setWindowModality(Qt::WindowModal); // a sheet on macOS
    connect(box, &QMessageBox::finished, this, [this, box, use, accepted] {
        if (box->clickedButton() != use) {
            m_spotifyClientId->setText(m_spotify.clientId); // nothing was saved
            return;
        }
        m_spotifyTermsAccepted = true;
        Q_EMIT spotifyTermsAccepted();
        accepted();
        setSpotifyStatus(m_spotify);
    });
    box->open();
}

void SettingsView::setSpotifyTermsAccepted(bool accepted)
{
    m_spotifyTermsAccepted = accepted;
    setSpotifyStatus(m_spotify);
}

void SettingsView::setSpotifyStatus(const core::SpotifyStatus &status)
{
    using State = meta::SpotifyAuth::State;
    using Mode = meta::SpotifyAuth::Mode;
    m_spotify = status;
    if (!m_spotifyClientId->hasFocus()) {
        m_spotifyClientId->setText(status.clientId);
    }
    m_spotifySecret->setPlaceholderText(status.hasSecret ? tr("Saved in the Keychain") : tr("Optional"));

    QString text;
    switch (status.state) {
    case State::NotConfigured:
        text = tr("Not set up: tags come from Deezer and MusicBrainz.");
        break;
    case State::SignedOut:
        text = tr("Not signed in.");
        break;
    case State::Restoring:
    case State::Exchanging:
        text = tr("Signing in…");
        break;
    case State::Authorizing:
        text = tr("Waiting for you in the browser…");
        break;
    case State::SignedIn:
        if (status.mode == Mode::ClientCredentials) {
            text = tr("Ready for tags and covers (Client ID and secret).");
        } else if (status.expiresAt) {
            text = tr("Signed in until %1.").arg(QLocale().toString(status.expiresAt->date(), QLocale::LongFormat));
        } else {
            text = tr("Signed in.");
        }
        break;
    case State::Expired:
        text = tr("The sign-in expired: sign in again.");
        break;
    }
    if (!m_spotifyTermsAccepted && status.state != State::NotConfigured) {
        // Credentials saved before the notice existed: unused until the user has read it.
        text = tr("Saved, but not in use until you accept the notice on Spotify's terms: press Save to read it.");
    } else if (!status.error.isEmpty()) {
        text += u' ' + tr("Last problem: %1").arg(status.error);
    }
    m_spotifyState->setText(text);
    const bool configured = status.state != State::NotConfigured;
    const bool userSignedIn = status.state == State::SignedIn && status.mode == Mode::User;
    m_spotifySignIn->setEnabled(configured && !userSignedIn && status.state != State::Authorizing);
    m_spotifySignOut->setVisible(userSignedIn);
}

void SettingsView::showLibrary()
{
    m_inbox->setText(QDir::toNativeSeparators(m_library.inboxRoot));
    m_inbox->setToolTip(m_library.inboxRoot);
    m_collections->clear();
    for (const QString &root : std::as_const(m_library.collectionRoots)) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(root));
        item->setToolTip(root);
        m_collections->addItem(item);
    }
    if (m_library.collectionRoots.isEmpty()) {
        auto *placeholder = new QListWidgetItem(tr("No collection folder yet: add one to file downloads."));
        placeholder->setFlags(Qt::NoItemFlags);
        m_collections->addItem(placeholder);
    }
    m_singlesFolder->setText(m_library.singlesFolder);
    m_albumsFolder->setText(m_library.albumsFolder);
    m_singleTemplate->setText(m_library.templates.single);
    m_albumFolderTemplate->setText(m_library.templates.albumFolder);
    m_albumTrackTemplate->setText(m_library.templates.albumTrack);
    {
        const QSignalBlocker blocker(m_clean);
        m_clean->setChecked(m_library.cleaner.enabled);
    }
    const int separator = m_separator->findData(QString(m_library.cleaner.separator));
    m_separator->setCurrentIndex(std::max(0, separator));
    m_separator->setEnabled(m_library.cleaner.enabled);
    refreshPreview();
}

void SettingsView::refreshPreview()
{
    library::naming::Fields single;
    single.artist = u"Halden & Rue"_s;
    single.title = u"Tidewater (Original Mix)"_s;
    library::naming::Fields album;
    album.artist = u"Okuma Drift"_s;
    album.albumArtist = u"Okuma Drift"_s;
    album.album = u"Nightfall"_s;
    album.year = u"2024"_s;
    album.title = u"Neon Rain"_s;
    album.track = u"3"_s;
    library::naming::Cleaner cleaner;
    cleaner.enabled = m_clean->isChecked();
    cleaner.separator = separatorOf(m_separator);
    const QString singleName = library::naming::componentName(m_singleTemplate->text(), single, cleaner, u"mp3"_s);
    const QString folder = library::naming::componentName(m_albumFolderTemplate->text(), album, cleaner);
    const QString track = library::naming::componentName(m_albumTrackTemplate->text(), album, cleaner, u"mp3"_s);
    m_preview->setText(m_singlesFolder->text() + u'/' + singleName + u'\n' + m_albumsFolder->text() + u'/' + folder +
                       u'/' + track);
}

QString SettingsView::applyLibraryConfig(const core::LibraryConfig &config)
{
    QString problem = core::checkLibraryConfig(config);
    if (!problem.isEmpty()) {
        m_libraryError->setText(problem);
        m_libraryError->show();
        showLibrary(); // back to the last good values
        return problem;
    }
    m_libraryError->hide();
    m_library = config;
    showLibrary();
    Q_EMIT libraryConfigChanged(m_library);
    return {};
}

void SettingsView::chooseInbox()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose the Inbox folder"), m_library.inboxRoot);
    if (folder.isEmpty()) {
        return;
    }
    core::LibraryConfig config = m_library;
    config.inboxRoot = QDir::cleanPath(folder);
    applyLibraryConfig(config);
}

void SettingsView::addCollection()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Add a collection folder"), QDir::homePath());
    if (folder.isEmpty()) {
        return;
    }
    core::LibraryConfig config = m_library;
    if (!config.collectionRoots.contains(QDir::cleanPath(folder))) {
        config.collectionRoots.append(QDir::cleanPath(folder));
    }
    applyLibraryConfig(config);
}

void SettingsView::removeCollection()
{
    const int row = m_collections->currentRow();
    if (row < 0 || row >= m_library.collectionRoots.size()) {
        return;
    }
    core::LibraryConfig config = m_library;
    config.collectionRoots.removeAt(row);
    applyLibraryConfig(config);
}

void SettingsView::editsFinished()
{
    core::LibraryConfig config = m_library;
    config.singlesFolder = m_singlesFolder->text().trimmed();
    config.albumsFolder = m_albumsFolder->text().trimmed();
    const auto templ = [](const QLineEdit *field, const QString &fallback) {
        const QString text = field->text().trimmed();
        return text.isEmpty() ? fallback : text;
    };
    config.templates.single = templ(m_singleTemplate, m_library.templates.single);
    config.templates.albumFolder = templ(m_albumFolderTemplate, m_library.templates.albumFolder);
    config.templates.albumTrack = templ(m_albumTrackTemplate, m_library.templates.albumTrack);
    config.cleaner.enabled = m_clean->isChecked();
    config.cleaner.separator = separatorOf(m_separator);
    const bool changed =
        config.singlesFolder != m_library.singlesFolder || config.albumsFolder != m_library.albumsFolder ||
        config.templates.single != m_library.templates.single ||
        config.templates.albumFolder != m_library.templates.albumFolder ||
        config.templates.albumTrack != m_library.templates.albumTrack ||
        config.cleaner.enabled != m_library.cleaner.enabled || config.cleaner.separator != m_library.cleaner.separator;
    if (changed) {
        applyLibraryConfig(config);
    }
}

void SettingsView::setAppearance(Appearance appearance)
{
    const QSignalBlocker blocker(m_appearance);
    const int index = m_appearance->findData(static_cast<int>(appearance));
    if (index >= 0) {
        m_appearance->setCurrentIndex(index);
    }
}

} // namespace selecta::app
