// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/GetTracksInspector.h"

#include <QCoreApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using selecta::app::GetTracksInspector;
using selecta::core::FileChoice;
using selecta::core::GetLine;

namespace {

FileChoice file(const QString &peer, int bitrate, const QString &shortfall = {})
{
    FileChoice choice;
    choice.peer = peer;
    choice.remotePath = (u"@@x\\"_s + peer + u"\\Okuma Drift - Neon Rain.mp3"_s).toUtf8();
    choice.size = qint64{bitrate} * 1000 / 8 * 372;
    choice.extension = u"mp3"_s;
    choice.bitrate = bitrate;
    choice.durationSec = 372;
    choice.fitsRelease = true;
    choice.freeSlot = true;
    choice.shortfall = shortfall;
    return choice;
}

GetLine track(qint64 id)
{
    GetLine line;
    line.id = id;
    line.raw = u"Okuma Drift - Neon Rain"_s;
    line.artist = u"Okuma Drift"_s;
    line.title = u"Neon Rain"_s;
    line.foundThisSession = true;
    return line;
}

GetLine notFound(qint64 id)
{
    GetLine line = track(id);
    line.findState = GetLine::FindState::NotFound;
    line.nearMisses = {file(u"low_peer"_s, 192, u"under 320 kbps"_s), file(u"lower_peer"_s, 128, u"under 320 kbps"_s)};
    line.message = u"2 files of this track found, below the quality wanted."_s;
    return line;
}

GetLine queued(qint64 id)
{
    GetLine line = track(id);
    line.findState = GetLine::FindState::Ready;
    line.best = file(u"forever_queue"_s, 320);
    line.alternatives = {file(u"free_slot"_s, 320), file(u"short_queue"_s, 320)};
    line.downloadState = GetLine::DownloadState::Queued;
    line.message = u"Queued at forever_queue, place 12"_s;
    return line;
}

template <typename T>
T *child(const GetTracksInspector &inspector, const QString &name)
{
    T *found = inspector.findChild<T *>(name);
    EXPECT_NE(found, nullptr) << name.toStdString();
    return found;
}

bool shown(const GetTracksInspector &inspector, const QString &name)
{
    const auto *widget = inspector.findChild<QWidget *>(name);
    return widget != nullptr && widget->isVisibleTo(&inspector);
}

// Every label shown with text that looks like markup shows it as plain text, with no buddy to make a shortcut of an
// "&" in it. `atLeast`: how many such labels the state shows.
void expectPlain(const GetTracksInspector &inspector, int atLeast)
{
    int seen = 0;
    const QList<QLabel *> labels = inspector.findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        if (!label->isVisibleTo(&inspector) || !label->text().contains(u'<')) {
            continue;
        }
        ++seen;
        EXPECT_EQ(label->textFormat(), Qt::PlainText) << label->text().toStdString();
        EXPECT_EQ(label->buddy(), nullptr) << label->text().toStdString();
    }
    EXPECT_GE(seen, atLeast);
}

} // namespace

TEST(GetTracksInspector, NotFoundListsTheNearMissesToGetAnyway)
{
    GetTracksInspector inspector;
    inspector.showLine(notFound(7));
    EXPECT_TRUE(shown(inspector, u"inspector.get.nearMisses"_s));
    EXPECT_FALSE(shown(inspector, u"inspector.get.otherFiles"_s));
    EXPECT_FALSE(shown(inspector, u"inspector.get.wayOut"_s));
    const auto *first = child<QRadioButton>(inspector, u"inspector.get.nearMiss.0"_s);
    auto *second = child<QRadioButton>(inspector, u"inspector.get.nearMiss.1"_s);
    ASSERT_TRUE(first && second);
    EXPECT_TRUE(first->isChecked()); // the best is picked to begin with
    EXPECT_TRUE(second->accessibleName().contains(u"lower_peer"_s));
    EXPECT_TRUE(second->accessibleName().contains(u"under 320 kbps"_s));

    const QSignalSpy getAnyway(&inspector, &GetTracksInspector::getAnywayRequested);
    second->click();
    // An update of the same line keeps the pick.
    inspector.showLine(notFound(7));
    EXPECT_TRUE(child<QRadioButton>(inspector, u"inspector.get.nearMiss.1"_s)->isChecked());
    child<QPushButton>(inspector, u"inspector.get.nearMisses.button"_s)->click();
    ASSERT_EQ(getAnyway.size(), 1);
    EXPECT_EQ(getAnyway.at(0).at(0).toLongLong(), 7);
    EXPECT_EQ(getAnyway.at(0).at(1).toString(), u"lower_peer"_s);
    EXPECT_EQ(getAnyway.at(0).at(2).toByteArray(), file(u"lower_peer"_s, 128).remotePath);

    // Another line starts from its best again.
    inspector.showLine(notFound(8));
    EXPECT_TRUE(child<QRadioButton>(inspector, u"inspector.get.nearMiss.0"_s)->isChecked());
}

TEST(GetTracksInspector, AQueuedLineOffersTheOtherPeersFiles)
{
    GetTracksInspector inspector;
    inspector.showLine(queued(3));
    EXPECT_TRUE(shown(inspector, u"inspector.get.otherFiles"_s));
    EXPECT_TRUE(shown(inspector, u"inspector.get.otherFiles.button"_s));
    EXPECT_FALSE(shown(inspector, u"inspector.get.tryAnotherPeer"_s)); // not stuck: no alarm
    EXPECT_FALSE(shown(inspector, u"inspector.get.nearMisses"_s));
    const QSignalSpy switching(&inspector, &GetTracksInspector::switchRequested);
    child<QRadioButton>(inspector, u"inspector.get.other.1"_s)->click();
    child<QPushButton>(inspector, u"inspector.get.otherFiles.button"_s)->click();
    ASSERT_EQ(switching.size(), 1);
    EXPECT_EQ(switching.at(0).at(1).toString(), u"short_queue"_s);

    // Once the peer says the place, it stands in for the queue length its search result gave.
    GetLine placed = queued(3);
    placed.best->freeSlot = false;
    placed.best->queueLength = 40;
    placed.placeInQueue = 12;
    inspector.showLine(placed);
    const QString fileText = child<QLabel>(inspector, u"inspector.get.file"_s)->text();
    EXPECT_TRUE(fileText.contains(u"From forever_queue · your place 12"_s)) << fileText.toStdString();
    EXPECT_FALSE(fileText.contains(u"queue 40"_s)) << fileText.toStdString();

    // Arriving: it keeps its bytes, no switching offered.
    GetLine arriving = queued(3);
    arriving.downloadState = GetLine::DownloadState::Transferring;
    inspector.showLine(arriving);
    EXPECT_FALSE(shown(inspector, u"inspector.get.otherFiles"_s));
}

TEST(GetTracksInspector, AStuckQueueIsSaidPlainlyWithOneButton)
{
    GetTracksInspector inspector;
    GetLine stuck = queued(4);
    stuck.stuck = true;
    stuck.message = u"No progress in forever_queue's queue since 21:03"_s;
    inspector.showLine(stuck);
    EXPECT_TRUE(child<QLabel>(inspector, u"inspector.get.status"_s)->text().startsWith(u"Stuck at the peer"_s));
    EXPECT_TRUE(shown(inspector, u"inspector.get.tryAnotherPeer"_s));
    EXPECT_FALSE(shown(inspector, u"inspector.get.otherFiles.button"_s)); // one button
    EXPECT_TRUE(shown(inspector, u"inspector.get.otherFiles"_s));         // its choice: the file picked
    // In the order it happens: the other file is asked for first.
    EXPECT_EQ(child<QLabel>(inspector, u"inspector.get.wayOut.note"_s)->text(),
              u"Selecta asks for the file picked below, then gives this queue place back."_s);
    const QSignalSpy switching(&inspector, &GetTracksInspector::switchRequested);
    child<QPushButton>(inspector, u"inspector.get.tryAnotherPeer"_s)->click();
    ASSERT_EQ(switching.size(), 1);
    EXPECT_EQ(switching.at(0).at(1).toString(), u"free_slot"_s); // the best, unless another is picked

    // No other file known (Selecta restarted): the engine looks for one.
    stuck.alternatives.clear();
    inspector.showLine(stuck);
    EXPECT_FALSE(shown(inspector, u"inspector.get.otherFiles"_s));
    child<QPushButton>(inspector, u"inspector.get.tryAnotherPeer"_s)->click();
    ASSERT_EQ(switching.size(), 2);
    EXPECT_TRUE(switching.at(1).at(1).toString().isEmpty());

    // While it looks, the button waits; then it says what it found.
    stuck.lookingForPeers = true;
    inspector.showLine(stuck);
    EXPECT_FALSE(child<QPushButton>(inspector, u"inspector.get.tryAnotherPeer"_s)->isEnabled());
    EXPECT_TRUE(child<QLabel>(inspector, u"inspector.get.wayOut.note"_s)->text().startsWith(u"Looking"_s));
    stuck.lookingForPeers = false;
    stuck.peersNote = u"No other peer has it in the quality wanted."_s;
    inspector.showLine(stuck);
    EXPECT_TRUE(child<QPushButton>(inspector, u"inspector.get.tryAnotherPeer"_s)->isEnabled());
    EXPECT_EQ(child<QLabel>(inspector, u"inspector.get.wayOut.note"_s)->text(), stuck.peersNote);
}

TEST(GetTracksInspector, ALineSearchedBeforeARestartOffersToFindItAgain)
{
    GetTracksInspector inspector;
    GetLine before = notFound(5);
    before.nearMisses.clear();
    before.foundThisSession = false;
    inspector.showLine(before);
    EXPECT_FALSE(shown(inspector, u"inspector.get.nearMisses"_s));
    ASSERT_TRUE(shown(inspector, u"inspector.get.findAgain"_s));
    const QSignalSpy findAgain(&inspector, &GetTracksInspector::findAgainRequested);
    child<QPushButton>(inspector, u"inspector.get.findAgain"_s)->click();
    ASSERT_EQ(findAgain.size(), 1);
    EXPECT_EQ(findAgain.at(0).at(0).toLongLong(), 5);

    // Found this session with nothing below the quality wanted either: nothing to offer.
    before.foundThisSession = true;
    inspector.showLine(before);
    EXPECT_FALSE(shown(inspector, u"inspector.get.findAgain"_s));
    EXPECT_FALSE(shown(inspector, u"inspector.get.wayOut"_s));
}

TEST(GetTracksInspector, AClickOnAFilesWordsPicksIt)
{
    GetTracksInspector inspector;
    inspector.showLine(notFound(7));
    auto *second = child<QRadioButton>(inspector, u"inspector.get.nearMiss.1"_s);
    ASSERT_NE(second, nullptr);
    auto *words = second->parentWidget()->findChild<QLabel *>(QString(), Qt::FindDirectChildrenOnly);
    ASSERT_NE(words, nullptr);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(2, 2), QPointF(2, 2), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(words, &release);
    EXPECT_TRUE(second->isChecked());
}

TEST(GetTracksInspector, NamesThatLookLikeMarkupShowAsTheyAre)
{
    // A peer, a file or a release can be named anything: a name like "<b>x</b>" shows literally, "&" included.
    const QString peer = u"<b>x</b>&y"_s;
    GetTracksInspector inspector;

    // Stuck: the title, the status, the note under the button, the file, the release and the other peers' files.
    GetLine stuck = queued(9);
    stuck.raw = u"<h1>Okuma Drift</h1> - Neon Rain"_s;
    stuck.best->peer = peer;
    stuck.best->remotePath = QByteArray("@@x\\<i>Music</i>\\Okuma Drift - Neon Rain.flac");
    stuck.alternatives = {file(u"<i>free</i>"_s, 320)};
    stuck.stuck = true;
    stuck.placeInQueue = 3;
    stuck.message = u"No progress in %1's queue since 21:03 (place 3)"_s.arg(peer);
    stuck.peersNote = u"Selecta couldn't ask <i>free</i> for the file."_s;
    selecta::meta::Release release;
    release.title = u"<b>Neon Rain</b>"_s;
    release.artists = {u"<i>Okuma Drift</i>"_s};
    stuck.release = release;
    stuck.releaseState = GetLine::ReleaseState::Resolved;
    inspector.showLine(stuck);
    expectPlain(inspector, 7);

    // Not found: the near misses.
    GetLine missed = notFound(10);
    missed.nearMisses = {file(peer, 192, u"under 320 kbps"_s)};
    inspector.showLine(missed);
    expectPlain(inspector, 1);

    // Queued: the other peers' files, and what trying one came to.
    GetLine waiting = queued(11);
    waiting.alternatives = {file(peer, 320)};
    waiting.peersNote = u"Selecta couldn't ask %1 for the file."_s.arg(peer);
    inspector.showLine(waiting);
    expectPlain(inspector, 2);
}
