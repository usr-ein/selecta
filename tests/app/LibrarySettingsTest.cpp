// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Library (PRD LIB-1, P-001): a tracks folder and an albums folder, each with its role; Selecta's own Inbox,
// which nobody picks; and the move from the single collection folder of earlier builds.
#include "app/AppSettings.h"
#include "app/views/SettingsView.h"
#include "base/Paths.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::AppSettings;
using selecta::app::SettingsView;
using selecta::core::LibraryConfig;

namespace {

class LibrarySettingsTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(dir.isValid()); }

    QString iniPath() const { return dir.filePath(u"settings.ini"_s); }
    QString inbox() const { return dir.filePath(u"data/Inbox"_s); }

    std::unique_ptr<SettingsView> makeView(const LibraryConfig &library)
    {
        return std::make_unique<SettingsView>(selecta::app::BuildInfo{}, selecta::app::Appearance::System, library,
                                              selecta::core::NetworkConfig{});
    }

    LibraryConfig chosen(const QString &tracks, const QString &albums) const
    {
        LibraryConfig config;
        config.inboxRoot = inbox();
        config.tracksRoot = tracks;
        config.albumsRoot = albums;
        return config;
    }

    QTemporaryDir dir;
};

QString text(const SettingsView &view, const QString &objectName)
{
    if (const auto *label = view.findChild<QLabel *>(objectName)) {
        return label->text();
    }
    if (const auto *field = view.findChild<QLineEdit *>(objectName)) {
        return field->text();
    }
    return u"(missing)"_s;
}

bool shown(const SettingsView &view, const QString &objectName)
{
    const auto *widget = view.findChild<QWidget *>(objectName);
    return widget != nullptr && widget->isVisibleTo(&view);
}

// --- AppSettings ---------------------------------------------------------------------------------------------------

TEST_F(LibrarySettingsTest, TheSingleFolderOfEarlierBuildsIsNotGuessed)
{
    {
        QSettings old(iniPath(), QSettings::IniFormat);
        old.setValue(u"library/inbox"_s, u"/Users/me/Music/Selecta/Inbox"_s);
        old.setValue(u"library/collections"_s, QStringList{u"/Users/me/Music"_s});
        old.setValue(u"library/singlesFolder"_s, u"tracks"_s);
        old.setValue(u"library/albumsFolder"_s, u"albums"_s);
        old.setValue(u"filing/single"_s, u"{title} - {artist}"_s);
        old.setValue(u"filing/separator"_s, u"-"_s);
        old.sync();
    }
    {
        AppSettings settings(iniPath(), inbox());
        const LibraryConfig config = settings.libraryConfig();
        EXPECT_EQ(config.inboxRoot, inbox()); // Selecta's own, not the old folder
        EXPECT_TRUE(config.tracksRoot.isEmpty());
        EXPECT_TRUE(config.albumsRoot.isEmpty());
        EXPECT_EQ(config.templates.single, u"{title} - {artist}"_s); // the naming stays
        EXPECT_EQ(config.cleaner.separator, u'-');
        ASSERT_TRUE(settings.sync());
    }
    const QSettings after(iniPath(), QSettings::IniFormat);
    for (const QString &key : {u"library/inbox"_s, u"library/collections"_s, u"library/singlesFolder"_s,
                               u"library/albumsFolder"_s}) {
        EXPECT_FALSE(after.contains(key)) << key.toStdString();
    }
    EXPECT_EQ(after.value(u"filing/single"_s).toString(), u"{title} - {artist}"_s);
}

TEST_F(LibrarySettingsTest, TheFoldersAreKeptAndTheInboxIsNot)
{
    LibraryConfig config = chosen(u"/Music/Tracks/"_s, u"/Music/Albums"_s);
    config.inboxRoot = u"/Somewhere/Else"_s; // not a setting
    {
        AppSettings settings(iniPath(), inbox());
        settings.setLibraryConfig(config);
        ASSERT_TRUE(settings.sync());
    }
    const LibraryConfig read = AppSettings(iniPath(), inbox()).libraryConfig();
    EXPECT_EQ(read.tracksRoot, u"/Music/Tracks"_s);
    EXPECT_EQ(read.albumsRoot, u"/Music/Albums"_s);
    EXPECT_EQ(read.inboxRoot, inbox());
    EXPECT_FALSE(QSettings(iniPath(), QSettings::IniFormat).allKeys().contains(u"library/inbox"_s));

    // A folder set back to nothing is forgotten.
    {
        AppSettings settings(iniPath(), inbox());
        LibraryConfig cleared = read;
        cleared.albumsRoot.clear();
        settings.setLibraryConfig(cleared);
        ASSERT_TRUE(settings.sync());
    }
    EXPECT_TRUE(AppSettings(iniPath(), inbox()).libraryConfig().albumsRoot.isEmpty());
    EXPECT_FALSE(QSettings(iniPath(), QSettings::IniFormat).contains(u"library/albums"_s));
}

TEST_F(LibrarySettingsTest, AFileEditedIntoWhatSettingsRefusesReadsAsNotChosen)
{
    const auto read = [this](const QString &tracks, const QString &albums) {
        {
            QSettings file(iniPath(), QSettings::IniFormat);
            file.setValue(u"library/tracks"_s, tracks);
            file.setValue(u"library/albums"_s, albums);
        }
        return AppSettings(iniPath(), inbox()).libraryConfig();
    };
    const LibraryConfig same = read(u"/Music"_s, u"/Music"_s);
    EXPECT_TRUE(same.tracksRoot.isEmpty() && same.albumsRoot.isEmpty());
    const LibraryConfig nested = read(u"/Music"_s, u"/Music/Albums"_s);
    EXPECT_TRUE(nested.tracksRoot.isEmpty() && nested.albumsRoot.isEmpty());
    const LibraryConfig relative = read(u"Music/Tracks"_s, u"/Music/Albums"_s);
    EXPECT_TRUE(relative.tracksRoot.isEmpty());
    EXPECT_EQ(relative.albumsRoot, u"/Music/Albums"_s);
}

TEST_F(LibrarySettingsTest, TheInboxIsTheDataFoldersByDefault)
{
    EXPECT_EQ(AppSettings(iniPath()).libraryConfig().inboxRoot, selecta::base::paths::inboxDir());
}

// --- SettingsView --------------------------------------------------------------------------------------------------

TEST_F(LibrarySettingsTest, SettingsAsksForBothFoldersUntilTheyAreChosen)
{
    auto view = makeView(chosen({}, {}));
    // No Inbox to pick, no list of collection folders, no folder names to type.
    EXPECT_EQ(view->findChild<QWidget *>(u"settings.inbox"_s), nullptr);
    EXPECT_EQ(view->findChild<QWidget *>(u"settings.collections"_s), nullptr);
    EXPECT_EQ(view->findChild<QWidget *>(u"settings.singlesFolder"_s), nullptr);
    ASSERT_NE(view->findChild<QPushButton *>(u"settings.tracksFolder.choose"_s), nullptr);
    ASSERT_NE(view->findChild<QPushButton *>(u"settings.albumsFolder.choose"_s), nullptr);
    const auto *tracksField = view->findChild<QLineEdit *>(u"settings.tracksFolder"_s);
    ASSERT_NE(tracksField, nullptr);
    EXPECT_TRUE(tracksField->isReadOnly()); // chosen in the folder picker only
    EXPECT_TRUE(tracksField->text().isEmpty());

    EXPECT_TRUE(shown(*view, u"settings.libraryPrompt"_s));
    EXPECT_EQ(text(*view, u"settings.libraryPrompt"_s), u"Choose your tracks folder and your albums folder."_s);

    QSignalSpy changed(view.get(), &SettingsView::libraryConfigChanged);
    EXPECT_EQ(view->applyLibraryConfig(chosen(u"/Music/Tracks"_s, {})), QString());
    EXPECT_EQ(text(*view, u"settings.tracksFolder"_s), u"/Music/Tracks"_s);
    EXPECT_EQ(text(*view, u"settings.libraryPrompt"_s), u"Choose your albums folder: filed albums go there."_s);
    EXPECT_EQ(view->applyLibraryConfig(chosen({}, u"/Music/Albums"_s)), QString());
    EXPECT_EQ(text(*view, u"settings.libraryPrompt"_s), u"Choose your tracks folder: filed singles go there."_s);
    EXPECT_EQ(view->applyLibraryConfig(chosen(u"/Music/Tracks"_s, u"/Music/Albums"_s)), QString());
    EXPECT_FALSE(shown(*view, u"settings.libraryPrompt"_s));
    EXPECT_EQ(text(*view, u"settings.albumsFolder"_s), u"/Music/Albums"_s);
    ASSERT_EQ(changed.size(), 3);
    const auto last = changed.constLast().at(0).value<LibraryConfig>();
    EXPECT_EQ(last.tracksRoot, u"/Music/Tracks"_s);
    EXPECT_EQ(last.albumsRoot, u"/Music/Albums"_s);
}

TEST_F(LibrarySettingsTest, TheFoldersMustStayApart)
{
    auto view = makeView(chosen(u"/Music/Tracks"_s, u"/Music/Albums"_s));
    QSignalSpy changed(view.get(), &SettingsView::libraryConfigChanged);
    const QString same = view->applyLibraryConfig(chosen(u"/Music/Tracks"_s, u"/Music/Tracks"_s));
    EXPECT_EQ(same, u"The tracks folder and the albums folder must be two different folders."_s);
    EXPECT_TRUE(shown(*view, u"settings.libraryError"_s));
    EXPECT_EQ(text(*view, u"settings.libraryError"_s), same);
    EXPECT_EQ(text(*view, u"settings.albumsFolder"_s), u"/Music/Albums"_s); // back to the last good values
    EXPECT_FALSE(view->applyLibraryConfig(chosen(u"/Music"_s, u"/Music/Albums"_s)).isEmpty());
    EXPECT_FALSE(view->applyLibraryConfig(chosen(u"/Music/Tracks"_s, inbox() + u"/Albums"_s)).isEmpty());
    EXPECT_TRUE(changed.isEmpty());
    EXPECT_EQ(view->libraryConfig().tracksRoot, u"/Music/Tracks"_s);

    // A good choice clears the error.
    EXPECT_EQ(view->applyLibraryConfig(chosen(u"/Music/Loose"_s, u"/Music/Albums"_s)), QString());
    EXPECT_FALSE(shown(*view, u"settings.libraryError"_s));
    EXPECT_EQ(changed.size(), 1);
}

TEST_F(LibrarySettingsTest, TheInboxIsNeverChosenInSettings)
{
    auto view = makeView(chosen({}, {}));
    LibraryConfig config = chosen(u"/Music/Tracks"_s, u"/Music/Albums"_s);
    config.inboxRoot = u"/Music/My Inbox"_s;
    QSignalSpy changed(view.get(), &SettingsView::libraryConfigChanged);
    EXPECT_EQ(view->applyLibraryConfig(config), QString());
    ASSERT_EQ(changed.size(), 1);
    EXPECT_EQ(changed.constFirst().at(0).value<LibraryConfig>().inboxRoot, inbox());
}

TEST_F(LibrarySettingsTest, TheExampleShowsWhereFiledFilesGo)
{
    auto view = makeView(chosen({}, {}));
    const QString unchosen = text(*view, u"settings.preview"_s);
    EXPECT_TRUE(unchosen.startsWith(u"Tracks folder/"_s)) << unchosen.toStdString();
    EXPECT_TRUE(unchosen.contains(u"\nAlbums folder/"_s)) << unchosen.toStdString();
    ASSERT_EQ(view->applyLibraryConfig(chosen(u"/Music/tracks"_s, u"/Music/albums"_s)), QString());
    const QString named = text(*view, u"settings.preview"_s);
    EXPECT_TRUE(named.startsWith(u"tracks/"_s)) << named.toStdString();
    EXPECT_TRUE(named.contains(u"\nalbums/"_s)) << named.toStdString();
}

} // namespace
