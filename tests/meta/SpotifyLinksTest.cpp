// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/SpotifyLinks.h"

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::meta::spotify;

namespace {

const QString kTrack = u"u8jzPde0IgxLd6GncfBAep"_s;
const QString kAlbum = u"KdNnFRIBXuDL7DxtpYlSXp"_s;
const QString kPlaylist = u"OzdmenCkhvMdgaKjIg8xNb"_s;

} // namespace

TEST(SpotifyLinks, ValidIdsAre22Base62Characters)
{
    EXPECT_TRUE(isValidId(kTrack));
    EXPECT_FALSE(isValidId(kTrack.left(21)));
    EXPECT_FALSE(isValidId(QString(kTrack + u"x"_s)));
    EXPECT_FALSE(isValidId(u"u8jzPde0IgxLd6GncfBAe-"_s));
    EXPECT_FALSE(isValidId(u""_s));
}

TEST(SpotifyLinks, ParsesTheShapesUsersPaste)
{
    struct Case
    {
        QString text;
        LinkKind kind = LinkKind::Track;
        QString id;
    };
    const QList<Case> cases = {
        {u"https://open.spotify.com/track/"_s + kTrack, LinkKind::Track, kTrack},
        {u"https://open.spotify.com/track/"_s + kTrack + u"?si=abc123&utm_source=copy-link"_s, LinkKind::Track, kTrack},
        {u"https://open.spotify.com/intl-fr/track/"_s + kTrack, LinkKind::Track, kTrack},
        {u"https://open.spotify.com/intl-pt-BR/track/"_s + kTrack + u"?si=x"_s, LinkKind::Track, kTrack},
        {u"http://open.spotify.com/track/"_s + kTrack, LinkKind::Track, kTrack},
        {u"https://OPEN.SPOTIFY.COM/track/"_s + kTrack, LinkKind::Track, kTrack},
        {u"https://open.spotify.com/embed/track/"_s + kTrack, LinkKind::Track, kTrack},
        {u"https://play.spotify.com/track/"_s + kTrack, LinkKind::Track, kTrack},
        {u"  https://open.spotify.com/album/"_s + kAlbum + u"  "_s, LinkKind::Album, kAlbum},
        {u"https://open.spotify.com/playlist/"_s + kPlaylist, LinkKind::Playlist, kPlaylist},
        {u"https://open.spotify.com/user/someone/playlist/"_s + kPlaylist, LinkKind::Playlist, kPlaylist},
        {u"spotify:track:"_s + kTrack, LinkKind::Track, kTrack},
        {u"Spotify:album:"_s + kAlbum, LinkKind::Album, kAlbum},
        {u"spotify:user:someone:playlist:"_s + kPlaylist, LinkKind::Playlist, kPlaylist},
        {u"<https://open.spotify.com/track/"_s + kTrack + u">"_s, LinkKind::Track, kTrack},
        {u"(spotify:track:"_s + kTrack + u"),"_s, LinkKind::Track, kTrack},
    };
    for (const Case &c : cases) {
        const std::optional<Link> link = parseLink(c.text);
        ASSERT_TRUE(link.has_value()) << c.text.toStdString();
        EXPECT_EQ(link->kind, c.kind) << c.text.toStdString();
        EXPECT_EQ(link->id, c.id) << c.text.toStdString();
    }
}

TEST(SpotifyLinks, RejectsEverythingElse)
{
    const QStringList cases = {
        u""_s,
        u"Bicep - Glue"_s,
        u"https://open.spotify.com/artist/"_s + kTrack,         // artists aren't importable
        u"https://open.spotify.com/track/"_s + kTrack.left(10), // short ID
        u"https://open.spotify.com.evil.example/track/"_s + kTrack,
        u"https://evil.example/open.spotify.com/track/"_s + kTrack,
        u"https://spotify.link/abcdEFGH"_s, // shortened: would need a request
        u"ftp://open.spotify.com/track/"_s + kTrack,
        u"https://open.spotify.com/track/"_s + kTrack + u"/extra"_s,
        u"spotify:track"_s,
        u"spotify:episode:"_s + kTrack,
        u"spotify:track:"_s + kTrack + u":extra"_s,
        u"spotify:local:Artist:Album:Title:200"_s,
        QString(3000, u'a'),
    };
    for (const QString &text : cases) {
        EXPECT_FALSE(parseLink(text).has_value()) << text.left(80).toStdString();
    }
}

TEST(SpotifyLinks, WebUrlIsCanonical)
{
    EXPECT_EQ(webUrl({LinkKind::Track, kTrack}), u"https://open.spotify.com/track/"_s + kTrack);
    EXPECT_EQ(webUrl({LinkKind::Album, kAlbum}), u"https://open.spotify.com/album/"_s + kAlbum);
    EXPECT_EQ(webUrl({LinkKind::Playlist, kPlaylist}), u"https://open.spotify.com/playlist/"_s + kPlaylist);
}

TEST(SpotifyLinks, PastedTextKeepsOrderDropsDuplicatesAndCountsSkippedLines)
{
    const QString pasted = u"https://open.spotify.com/track/"_s + kTrack + u"?si=1\r\n"_s + //
                           u"\r\n"_s +                                                      // blank: not counted
                           u"Some Artist - Some Title\n"_s +                                // skipped line
                           u"spotify:album:"_s + kAlbum + u"\n"_s +                         //
                           u"https://open.spotify.com/track/"_s + kTrack + u"?si=2\n"_s +   // duplicate
                           u"   \t  \n"_s +                                                 // blank
                           u"spotify:playlist:"_s + kPlaylist + u" spotify:track:"_s + kTrack + u"\n"_s;
    const PastedLinks links = parsePastedLinks(pasted);
    ASSERT_EQ(links.links.size(), 3);
    EXPECT_EQ(links.links.at(0), (Link{LinkKind::Track, kTrack}));
    EXPECT_EQ(links.links.at(1), (Link{LinkKind::Album, kAlbum}));
    EXPECT_EQ(links.links.at(2), (Link{LinkKind::Playlist, kPlaylist}));
    EXPECT_EQ(links.duplicates, 2);
    EXPECT_EQ(links.skipped, 1);
}

TEST(SpotifyLinks, TabsAndManyLinksOnOneLine)
{
    const QString pasted = u"spotify:track:"_s + kTrack + u"\tspotify:album:"_s + kAlbum;
    const PastedLinks links = parsePastedLinks(pasted);
    EXPECT_EQ(links.links.size(), 2);
    EXPECT_EQ(links.skipped, 0);
}

TEST(SpotifyLinks, LinesBeyondTheBoundAreSkipped)
{
    QString pasted;
    for (int i = 0; i < kMaxPastedLines + 5; ++i) {
        pasted += u"not a link %1\n"_s.arg(i);
    }
    pasted += u"spotify:track:"_s + kTrack;
    const PastedLinks links = parsePastedLinks(pasted);
    EXPECT_TRUE(links.links.isEmpty()); // the link is past the bound
    EXPECT_EQ(links.skipped, kMaxPastedLines + 6);
}

TEST(SpotifyLinks, EmptyPaste)
{
    const PastedLinks links = parsePastedLinks(u""_s);
    EXPECT_TRUE(links.links.isEmpty());
    EXPECT_EQ(links.skipped, 0);
    EXPECT_EQ(links.duplicates, 0);
}
