// SPDX-License-Identifier: GPL-3.0-or-later
// PRD META-4, D30: the one-time notice on Spotify's terms, before Spotify is first used, and nothing from Spotify
// without a yes.
#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "app/Preview.h"
#include "core/EngineHost.h"
#include "meta/TokenStore.h"
#include "storage/Database.h"
#include "storage/Migrations.h"
#include "storage/SessionLog.h"

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <gtest/gtest.h>
#include <memory>

using namespace Qt::StringLiterals;
using selecta::app::AppSettings;
using selecta::app::MainWindow;
using State = selecta::meta::SpotifyAuth::State;

namespace {

const QString kClientId = u"0123456789abcdef0123456789abcdef"_s; // shaped like a Spotify Client ID; not a real one

class SpotifyTermsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(db.open(u":memory:"_s));
        ASSERT_TRUE(selecta::storage::migrate(db, selecta::storage::selectaMigrations()).ok());
        session = sessions.start(1'000);
        settings = std::make_unique<AppSettings>(dir.filePath(u"settings.ini"_s));
        preview = std::make_unique<selecta::app::PreviewController>(selecta::playback::AudioOutput::createSilent(),
                                                                    nullptr); // never the real audio device
    }

    void TearDown() override
    {
        if (engine) {
            engine->stop();
        }
    }

    // Starts the engine as the app does: Spotify allowed once the notice was accepted. `savedClientId`: a Client ID
    // already saved, as an earlier version of Selecta could have left it.
    void startEngine(const QString &savedClientId = {})
    {
        selecta::core::EngineOptions options;
        options.databasePath = dir.filePath(u"engine.sqlite3"_s);
        options.metaEndpoints = selecta::core::MetaEndpoints::unreachable(); // tests never reach real services
        options.spotifyAllowed = settings->spotifyTermsAccepted();
        options.openUrl = [this](const QUrl &) { // on the engine thread
            browserOpened = true;
            return true;
        };
        options.tokenStore = [savedClientId] {
            auto store = std::make_unique<selecta::meta::InMemoryTokenStore>();
            if (!savedClientId.isEmpty()) {
                std::ignore =
                    store->write(QString(selecta::core::MetaServices::kClientIdAccount), savedClientId.toUtf8());
            }
            return store;
        };
        engine = std::make_unique<selecta::core::EngineHost>(options);
        QSignalSpy started(engine.get(), &selecta::core::EngineHost::started);
        engine->start();
        ASSERT_TRUE(started.wait(5000));
        ASSERT_TRUE(started.constFirst().at(0).toBool());
    }

    std::unique_ptr<MainWindow> makeWindow()
    {
        selecta::app::BuildInfo build;
        build.version = u"0.0.0-test"_s;
        build.session = session;
        auto window = std::make_unique<MainWindow>(
            selecta::app::AppServices{db, sessions, *settings, *engine, *preview, build, 1'000});
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
        return window;
    }

    // The notice on screen, if any.
    static QMessageBox *notice(const MainWindow &window)
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // notices answered are gone
        const QList<QMessageBox *> boxes = window.findChildren<QMessageBox *>(u"settings.spotify.terms"_s);
        for (QMessageBox *box : boxes) {
            if (box->isVisible()) {
                return box;
            }
        }
        return nullptr;
    }

    static void answer(QMessageBox &box, bool useSpotify)
    {
        auto *button = box.findChild<QPushButton *>(useSpotify ? u"settings.spotify.terms.accept"_s
                                                               : u"settings.spotify.terms.decline"_s);
        ASSERT_NE(button, nullptr);
        button->click();
    }

    QTemporaryDir dir;
    selecta::storage::Database db;
    selecta::storage::SessionLog sessions{db};
    int session = 0;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<selecta::core::EngineHost> engine;
    std::unique_ptr<selecta::app::PreviewController> preview;
    std::atomic<bool> browserOpened = false;
};

} // namespace

TEST_F(SpotifyTermsTest, TheNoticeComesBeforeSpotifyIsUsedAndOnlyOnce)
{
    ASSERT_NO_FATAL_FAILURE(startEngine());
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"settings"_s));
    auto *clientId = window->findChild<QLineEdit *>(u"settings.spotify.clientId"_s);
    auto *save = window->findChild<QPushButton *>(u"settings.spotify.save"_s);
    ASSERT_NE(clientId, nullptr);
    ASSERT_NE(save, nullptr);

    // Saving a Client ID asks first and says what is at stake.
    clientId->setText(kClientId);
    save->click();
    QMessageBox *box = notice(*window);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(box->informativeText().contains(u"developer terms"_s)) << box->informativeText().toStdString();
    EXPECT_TRUE(box->informativeText().contains(u"revoke the developer app (Client ID)"_s));
    EXPECT_TRUE(box->informativeText().contains(u"Deezer and MusicBrainz"_s));

    // "Use Deezer and MusicBrainz": nothing is saved, and Spotify stays unused.
    ASSERT_NO_FATAL_FAILURE(answer(*box, false));
    EXPECT_EQ(notice(*window), nullptr);
    EXPECT_FALSE(settings->spotifyTermsAccepted());
    EXPECT_TRUE(clientId->text().isEmpty());
    QTest::qWait(200);
    EXPECT_EQ(engine->spotifyStatus().state, State::NotConfigured);
    EXPECT_TRUE(engine->spotifyStatus().clientId.isEmpty());

    // Asked again next time; "Use Spotify Tags" saves the Client ID, and the answer is kept.
    clientId->setText(kClientId);
    save->click();
    box = notice(*window);
    ASSERT_NE(box, nullptr);
    ASSERT_NO_FATAL_FAILURE(answer(*box, true));
    EXPECT_TRUE(settings->spotifyTermsAccepted());
    EXPECT_TRUE(AppSettings(dir.filePath(u"settings.ini"_s)).spotifyTermsAccepted()); // on disk
    ASSERT_TRUE(QTest::qWaitFor([this] { return engine->spotifyStatus().clientId == kClientId; }, 5000));
    EXPECT_EQ(engine->spotifyStatus().state, State::SignedOut);

    // Never again: not on the next Save, nor in a new window.
    save->click();
    EXPECT_EQ(notice(*window), nullptr);
    window.reset();
    window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"settings"_s));
    auto *saveAgain = window->findChild<QPushButton *>(u"settings.spotify.save"_s);
    ASSERT_NE(saveAgain, nullptr);
    saveAgain->click();
    EXPECT_EQ(notice(*window), nullptr);
}

TEST_F(SpotifyTermsTest, CredentialsSavedBeforeTheNoticeWaitForAnAnswer)
{
    // A Client ID saved before the notice existed: read and shown, but never signed in with.
    ASSERT_NO_FATAL_FAILURE(startEngine(kClientId));
    ASSERT_TRUE(QTest::qWaitFor([this] { return engine->spotifyStatus().clientId == kClientId; }, 5000));
    EXPECT_EQ(engine->spotifyStatus().state, State::SignedOut);
    auto window = makeWindow();
    ASSERT_TRUE(window->navigateTo(u"settings"_s));
    auto *state = window->findChild<QLabel *>(u"settings.spotify.state"_s);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->text().contains(u"not in use"_s)) << state->text().toStdString();

    // Sign In asks first: declined, nothing happens.
    auto *signIn = window->findChild<QPushButton *>(u"settings.spotify.signIn"_s);
    ASSERT_NE(signIn, nullptr);
    ASSERT_TRUE(signIn->isEnabled());
    signIn->click();
    QMessageBox *box = notice(*window);
    ASSERT_NE(box, nullptr);
    ASSERT_NO_FATAL_FAILURE(answer(*box, false));
    QTest::qWait(200);
    EXPECT_FALSE(browserOpened);
    EXPECT_EQ(engine->spotifyStatus().state, State::SignedOut);

    // Accepted: the browser opens for the sign-in (the fake accounts service here: nothing listens).
    signIn->click();
    box = notice(*window);
    ASSERT_NE(box, nullptr);
    ASSERT_NO_FATAL_FAILURE(answer(*box, true));
    ASSERT_TRUE(QTest::qWaitFor([this] { return browserOpened.load(); }, 5000));
    ASSERT_TRUE(QTest::qWaitFor([this] { return engine->spotifyStatus().state == State::Authorizing; }, 5000));
    EXPECT_FALSE(state->text().contains(u"not in use"_s)) << state->text().toStdString();
}
