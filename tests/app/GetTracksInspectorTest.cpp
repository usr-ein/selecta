// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/views/GetTracksInspector.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QTest>

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

// --- the cover (P-021) ---

namespace {

using selecta::app::CoverArt;
using selecta::meta::Release;

Release release(const QString &cover)
{
    Release result;
    result.title = u"Neon Rain"_s;
    result.artists = {u"Okuma Drift"_s};
    result.album = u"Neon Rain"_s;
    if (!cover.isEmpty()) {
        result.covers = {{QUrl(cover), 640, 640}};
    }
    return result;
}

GetLine withRelease(qint64 id, const QString &cover)
{
    GetLine line = track(id);
    line.releaseState = GetLine::ReleaseState::Resolved;
    line.release = release(cover);
    return line;
}

QString coverUrl(int n)
{
    return u"https://i.scdn.co/image/test-cover-%1"_s.arg(n);
}

QByteArray pngBytes(int width, int height, const QColor &color = Qt::red)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

CoverArt *cover(const GetTracksInspector &inspector)
{
    return child<CoverArt>(inspector, u"inspector.get.cover"_s);
}

// Shows `line` and waits for the request for its cover; false when none came.
bool shownAndAsked(GetTracksInspector &inspector, QSignalSpy &wanted, const GetLine &line)
{
    const qsizetype before = wanted.size();
    inspector.showLine(line);
    return QTest::qWaitFor([&] { return wanted.size() > before; }, 2000);
}

} // namespace

TEST(GetTracksInspector, TheCoverIsAskedForOnceTheLineHasBeenShownAMoment)
{
    GetTracksInspector inspector;
    QSignalSpy wanted(&inspector, &GetTracksInspector::coverWanted);
    const GetLine line = withRelease(1, coverUrl(1));
    inspector.showLine(line);
    EXPECT_TRUE(wanted.isEmpty()); // not at once: arrowing through the list asks for nothing on the way
    ASSERT_TRUE(QTest::qWaitFor([&] { return !wanted.isEmpty(); }, 2000));
    EXPECT_EQ(wanted.at(0).at(0).toString(), coverUrl(1));
    EXPECT_EQ(wanted.at(0).at(0).toString(), GetTracksInspector::coverKey(*line.release));
    EXPECT_EQ(wanted.at(0).at(1).value<Release>(), *line.release);

    // The same line again (its progress changed), or another line with the same cover: not asked again.
    GetLine moved = line;
    moved.message = u"Searching"_s;
    inspector.showLine(moved);
    inspector.showLine(withRelease(2, coverUrl(1)));
    QTest::qWait(GetTracksInspector::kCoverDelayMs * 2);
    EXPECT_EQ(wanted.size(), 1);

    // Lines passed on the way ask for nothing; the one the selection stops on does.
    wanted.clear();
    inspector.showLine(withRelease(3, coverUrl(3)));
    inspector.showLine(withRelease(4, coverUrl(4)));
    inspector.showLine(withRelease(5, coverUrl(5)));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !wanted.isEmpty(); }, 2000));
    QTest::qWait(GetTracksInspector::kCoverDelayMs * 2);
    ASSERT_EQ(wanted.size(), 1);
    EXPECT_EQ(wanted.at(0).at(0).toString(), coverUrl(5));
}

TEST(GetTracksInspector, TheCoverSitsAboveTheTitleSquareAndAsWideAsThePanel)
{
    GetTracksInspector inspector;
    inspector.resize(300, 900);
    inspector.show();
    QSignalSpy wanted(&inspector, &GetTracksInspector::coverWanted);
    const GetLine line = withRelease(1, coverUrl(1));
    ASSERT_TRUE(shownAndAsked(inspector, wanted, line));
    CoverArt *art = cover(inspector);
    ASSERT_NE(art, nullptr);
    EXPECT_FALSE(art->hasImage());
    EXPECT_EQ(art->accessibleName(), u"No cover"_s);
    QCoreApplication::processEvents();
    const QRect placeholder = art->geometry();

    inspector.showCover(coverUrl(1), pngBytes(640, 640));
    ASSERT_TRUE(QTest::qWaitFor([&] { return art->hasImage(); }, 2000));
    EXPECT_EQ(art->accessibleName(), u"Cover"_s);
    QCoreApplication::processEvents();

    const auto *title = child<QLabel>(inspector, u"inspector.get.title"_s);
    ASSERT_NE(title, nullptr);
    EXPECT_LT(art->geometry().bottom(), title->geometry().top());
    EXPECT_EQ(art->width(), title->width()); // the panel's width, between its margins
    EXPECT_GT(art->width(), 200);
    EXPECT_EQ(art->height(), art->width());
    EXPECT_EQ(art->geometry(), placeholder); // the layout doesn't jump when the cover arrives
}

TEST(GetTracksInspector, WithoutACoverThePlaceholderHoldsItsPlace)
{
    GetTracksInspector inspector;
    inspector.setCoverDelay(0);
    QSignalSpy wanted(&inspector, &GetTracksInspector::coverWanted);

    // No release yet, or none: nothing to ask for.
    GetLine pending = track(1);
    inspector.showLine(pending);
    GetLine none = track(2);
    none.releaseState = GetLine::ReleaseState::None;
    inspector.showLine(none);
    // A release that links no cover and has no MusicBrainz ids: none can be found.
    inspector.showLine(withRelease(3, QString()));
    QTest::qWait(100);
    EXPECT_TRUE(wanted.isEmpty());
    EXPECT_TRUE(shown(inspector, u"inspector.get.cover"_s));
    EXPECT_FALSE(cover(inspector)->hasImage());

    // A release known by MusicBrainz only: the Cover Art Archive has its cover.
    GetLine archived = withRelease(4, QString());
    archived.release->musicBrainzReleaseId = u"c6a53877-7733-4bdb-9721-0dff076ce2ef"_s;
    ASSERT_TRUE(shownAndAsked(inspector, wanted, archived));
    EXPECT_EQ(wanted.constLast().at(0).toString(), u"musicbrainz:release:c6a53877-7733-4bdb-9721-0dff076ce2ef"_s);
    GetLine grouped = withRelease(5, QString());
    grouped.release->musicBrainzReleaseGroupId = u"0d464138-a623-4255-bfc1-ea36f17fd374"_s;
    EXPECT_EQ(GetTracksInspector::coverKey(*grouped.release),
              u"musicbrainz:release-group:0d464138-a623-4255-bfc1-ea36f17fd374"_s);

    // No cover found, or bytes that aren't a cover we take: the placeholder stays.
    const QString key = wanted.constLast().at(0).toString();
    inspector.showCover(key, {});
    EXPECT_FALSE(cover(inspector)->hasImage());
    const QList<QByteArray> refused = {
        QByteArrayLiteral("<html>not found</html>"),
        QByteArray("GIF89a\x01\x00\x01\x00", 10),
        pngBytes(640, 640).left(60), // a valid header, then cut short
    };
    for (const QByteArray &bytes : refused) {
        ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(6, coverUrl(6)))); // answered: asked again
        inspector.showCover(coverUrl(6), bytes);
        QTest::qWait(100);
        EXPECT_FALSE(cover(inspector)->hasImage());
        inspector.showLine(std::nullopt);
    }

    // Nothing selected: the placeholder, ready for the next line.
    inspector.showLine(std::nullopt);
    EXPECT_FALSE(cover(inspector)->hasImage());
}

TEST(GetTracksInspector, ACoverIsKeptForItsLineAndOnlyAsAskedFor)
{
    GetTracksInspector inspector;
    inspector.setCoverDelay(0);
    QSignalSpy wanted(&inspector, &GetTracksInspector::coverWanted);

    // An answer nobody asked for is ignored.
    inspector.showCover(coverUrl(9), pngBytes(64, 64));
    ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(9, coverUrl(9))));
    QTest::qWait(100);
    EXPECT_FALSE(cover(inspector)->hasImage());

    // The answer for a line left meanwhile doesn't show on the next one, but is there when it comes back.
    ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(1, coverUrl(1))));
    ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(2, coverUrl(2))));
    inspector.showCover(coverUrl(1), pngBytes(64, 64));
    QTest::qWait(200);
    EXPECT_FALSE(cover(inspector)->hasImage());
    const qsizetype asked = wanted.size();
    inspector.showLine(withRelease(1, coverUrl(1)));
    EXPECT_TRUE(cover(inspector)->hasImage()); // at once, from memory
    QTest::qWait(100);
    EXPECT_EQ(wanted.size(), asked);

    // A cover not found is asked for again when its line is next shown.
    inspector.showCover(coverUrl(2), {});
    ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(2, coverUrl(2))));
    EXPECT_EQ(wanted.constLast().at(0).toString(), coverUrl(2));
}

TEST(GetTracksInspector, OnlyTheLastCoversAreKept)
{
    GetTracksInspector inspector;
    inspector.setCoverDelay(0);
    QSignalSpy wanted(&inspector, &GetTracksInspector::coverWanted);
    for (int n = 0; n <= GetTracksInspector::kCoverCacheSize; ++n) {
        ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(n, coverUrl(n))));
        inspector.showCover(coverUrl(n), pngBytes(32, 32));
        ASSERT_TRUE(QTest::qWaitFor([&] { return cover(inspector)->hasImage(); }, 2000)) << n;
    }
    // The oldest went; the others stay.
    inspector.showLine(withRelease(1, coverUrl(1)));
    EXPECT_TRUE(cover(inspector)->hasImage());
    const qsizetype asked = wanted.size();
    ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(0, coverUrl(0))));
    EXPECT_EQ(wanted.size(), asked + 1);
    EXPECT_FALSE(cover(inspector)->hasImage());
}

TEST(GetTracksInspector, AHugeCoverIsShownScaledDown)
{
    GetTracksInspector inspector;
    inspector.setCoverDelay(0);
    QSignalSpy wanted(&inspector, &GetTracksInspector::coverWanted);
    ASSERT_TRUE(shownAndAsked(inspector, wanted, withRelease(1, coverUrl(1))));
    inspector.showCover(coverUrl(1), pngBytes(2400, 1200)); // not square either
    ASSERT_TRUE(QTest::qWaitFor([&] { return cover(inspector)->hasImage(); }, 5000));
    EXPECT_EQ(cover(inspector)->imageSize(), QSize(GetTracksInspector::kCoverPixels, GetTracksInspector::kCoverPixels / 2));
}
