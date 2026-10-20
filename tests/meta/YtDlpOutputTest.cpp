// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/YtDlpOutput.h"

#include "meta/YtDlpRunner.h"
#include "meta/support/Fixtures.h"

#include <QDir>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::meta;
namespace yt = selecta::meta::detail::ytdlp;
using selecta::meta::testing::fixture;

namespace {

QList<QByteArray> lines(std::initializer_list<const char *> text)
{
    QList<QByteArray> out;
    for (const char *line : text) {
        out.append(QByteArray(line));
    }
    return out;
}

ErrorCode codeOf(std::initializer_list<const char *> text, int exitCode = 1, bool runtimePassed = true)
{
    return yt::classifyFailure(lines(text), exitCode, runtimePassed).code;
}

YouTubeCandidate video(const QString &id, const QString &title, const QString &channel, int durationSec,
                       qint64 views = 0, bool verified = false)
{
    YouTubeCandidate candidate;
    candidate.id = id;
    candidate.title = title;
    candidate.channel = channel;
    candidate.durationSec = durationSec;
    candidate.viewCount = views;
    candidate.channelVerified = verified;
    return candidate;
}

Release track(const QString &title, qint64 durationMs, const QStringList &artists = {u"Kestrel Moon"_s})
{
    Release release;
    release.title = title;
    release.artists = artists;
    release.durationMs = durationMs;
    release.label = u"Lantern Grove Records"_s;
    return release;
}

QString picked(const QList<YouTubeCandidate> &candidates, const Release &release)
{
    const std::optional<YouTubePick> pick = YtDlpRunner::pick(candidates, release);
    return pick.has_value() ? pick->candidate.id : QString();
}

} // namespace

TEST(YtDlpArguments, EveryRunCarriesTheSafetyFlags)
{
    const QStringList safety = yt::safetyArguments(QString(), QString());
    for (const char *flag : {"--ignore-config", "--no-plugin-dirs", "--no-cookies", "--no-cookies-from-browser",
                             "--no-remote-components", "--no-cache-dir"}) {
        EXPECT_TRUE(safety.contains(QString::fromLatin1(flag))) << flag;
    }
    EXPECT_FALSE(safety.contains(u"--js-runtimes"_s));
    const QStringList withDeno = yt::safetyArguments(u"/opt/homebrew/bin/deno"_s, u"/caches/yt-dlp"_s);
    EXPECT_EQ(withDeno.at(withDeno.indexOf(u"--js-runtimes"_s) + 1), u"deno:/opt/homebrew/bin/deno"_s);
    EXPECT_EQ(withDeno.at(withDeno.indexOf(u"--cache-dir"_s) + 1), u"/caches/yt-dlp"_s);
    EXPECT_FALSE(withDeno.contains(u"--no-cache-dir"_s));
    for (const QStringList &arguments : {yt::versionArguments({}, {}), yt::searchArguments({}, {}, u"a b"_s, 10),
                                         yt::fetchArguments({}, {}, u"KmTopic00Ex"_s, u"/cache/yt"_s)}) {
        EXPECT_TRUE(arguments.mid(0, safety.size()) == safety);
        for (const char *forbidden : {"--cookies", "--cookies-from-browser", "--exec", "-U", "--update-to",
                                      "--username", "--password", "--netrc", "--config-locations", "--batch-file",
                                      "--load-info-json", "--impersonate", "--extractor-args"}) {
            EXPECT_FALSE(arguments.contains(QString::fromLatin1(forbidden))) << forbidden;
        }
    }
}

TEST(YtDlpArguments, SearchAndFetchCommandLines)
{
    const QStringList search = yt::searchArguments({}, {}, u"Kestrel Moon Glass Harbour"_s, 10);
    EXPECT_TRUE(search.contains(u"--flat-playlist"_s));
    EXPECT_TRUE(search.contains(u"-J"_s));
    EXPECT_EQ(search.at(search.size() - 2), u"--"_s); // the query can never be read as an option
    EXPECT_EQ(search.last(), u"ytsearch10:Kestrel Moon Glass Harbour"_s);

    const QStringList fetch = yt::fetchArguments({}, {}, u"-mTopic00Ex"_s, u"/cache/yt"_s);
    EXPECT_EQ(fetch.at(fetch.size() - 2), u"--"_s);
    EXPECT_EQ(fetch.last(), u"https://www.youtube.com/watch?v=-mTopic00Ex"_s); // a full URL, never a bare ID
    EXPECT_EQ(fetch.at(fetch.indexOf(u"-P"_s) + 1), u"/cache/yt"_s);
    EXPECT_EQ(fetch.at(fetch.indexOf(u"-o"_s) + 1), u"%(id)s.%(ext)s"_s);
    EXPECT_EQ(fetch.at(fetch.indexOf(u"-f"_s) + 1), u"bestaudio[ext=m4a]/bestaudio[acodec=opus]/bestaudio"_s);
    EXPECT_EQ(fetch.at(fetch.indexOf(u"--fixup"_s) + 1), u"never"_s);
    for (const char *flag : {"--no-part", "--no-playlist", "--no-mtime", "--force-overwrites", "--newline"}) {
        EXPECT_TRUE(fetch.contains(QString::fromLatin1(flag))) << flag;
    }
    EXPECT_FALSE(fetch.contains(u"--max-downloads"_s)); // it would turn a success into exit 101
}

TEST(YtDlpOutput, VideoIdsAndSearchText)
{
    EXPECT_TRUE(yt::isValidVideoId(u"KmTopic00Ex"));
    EXPECT_TRUE(yt::isValidVideoId(u"-_aZ09-_aZ0"));
    EXPECT_FALSE(yt::isValidVideoId(u"short"));
    EXPECT_FALSE(yt::isValidVideoId(u"KmTopic00Ex1"));
    EXPECT_FALSE(yt::isValidVideoId(u"KmTopic0/Ex"));
    EXPECT_EQ(yt::cleanSearchText(u"  Kestrel\nMoon \t Glass\u0007 Harbour  "_s), u"Kestrel Moon Glass Harbour"_s);
    EXPECT_EQ(yt::cleanSearchText(QString(500, u'x')).size(), 200);
}

TEST(YtDlpOutput, Versions)
{
    EXPECT_EQ(yt::parseVersion("2026.08.19\n"), QDate(2026, 8, 19));
    EXPECT_EQ(yt::parseVersion("\n2025.11.12.232838\n"), QDate(2025, 11, 12)); // nightly
    EXPECT_EQ(yt::versionText("2025.11.12.232838\n"), u"2025.11.12.232838"_s);
    EXPECT_FALSE(yt::parseVersion("yt-dlp 2026").has_value());
    EXPECT_FALSE(yt::parseVersion("2026.13.40").has_value());
    EXPECT_FALSE(yt::parseVersion("").has_value());
    EXPECT_LT(QDate(2025, 7, 21), YtDlpRunner::minimumVersion()); // the version the research found on this machine
}

TEST(YtDlpOutput, SearchEntries)
{
    const Result<QList<YouTubeCandidate>> result = yt::parseSearch(fixture(u"youtube/search_glass_harbour.json"_s));
    ASSERT_TRUE(result.ok()) << result.error().toString().toStdString();
    const QList<YouTubeCandidate> &entries = result.value();
    ASSERT_EQ(entries.size(), 8); // the broken ID and the channel entry are dropped
    const YouTubeCandidate &first = entries.at(0);
    EXPECT_EQ(first.id, u"KmArtist0Ex"_s);
    EXPECT_EQ(first.title, u"Kestrel Moon - Glass Harbour (Extended Mix) [Official Audio]"_s);
    EXPECT_EQ(first.channel, u"Kestrel Moon"_s);
    EXPECT_EQ(first.channelId, u"UCkestrelmoon000000000a"_s);
    EXPECT_EQ(first.durationSec, 392);
    EXPECT_EQ(first.viewCount, 52000);
    EXPECT_TRUE(first.channelVerified);
    EXPECT_TRUE(first.liveStatus.isEmpty());
    EXPECT_EQ(first.watchUrl(), QUrl(u"https://www.youtube.com/watch?v=KmArtist0Ex"_s));
    EXPECT_EQ(entries.at(5).durationSec, 412); // a float duration
    EXPECT_EQ(entries.at(6).liveStatus, u"was_live"_s);
    EXPECT_FALSE(entries.at(7).durationSec.has_value());
    EXPECT_FALSE(entries.at(7).viewCount.has_value());

    EXPECT_EQ(yt::parseSearch("this is not json").error().code, ErrorCode::BadResponse);
    EXPECT_EQ(yt::parseSearch(R"({"_type":"playlist"})").error().code, ErrorCode::BadResponse);
    EXPECT_TRUE(yt::parseSearch(R"({"entries":[]})").value().isEmpty());
    EXPECT_TRUE(yt::parseSearch(R"({"entries":[1, "x", null, {"id": 5}]})").value().isEmpty());
}

TEST(YtDlpOutput, ManyEntriesAreBounded)
{
    QByteArray json = R"({"entries":[)";
    for (int i = 0; i < 100; ++i) {
        json += QByteArray(i > 0 ? "," : "") + R"({"id":"AAAAAAAAA)" + QByteArray::number(10 + i) + R"("})";
    }
    json += "]}";
    EXPECT_EQ(yt::parseSearch(json).value().size(), YtDlpRunner::kMaxSearchResults);
}

TEST(YtDlpOutput, MarkerLines)
{
    const auto progress = yt::parseProgress("SELECTA-PROGRESS 8192 40960.0 1");
    ASSERT_TRUE(progress.has_value());
    EXPECT_EQ(progress->downloadedBytes, 8192);
    EXPECT_EQ(progress->totalBytes, 40960);
    EXPECT_EQ(yt::parseProgress("SELECTA-PROGRESS 100 NA NA")->totalBytes, 0);
    EXPECT_FALSE(yt::parseProgress("SELECTA-PROGRESS NA NA NA").has_value());
    EXPECT_FALSE(yt::parseProgress("SELECTA-PROGRESS -5 10").has_value());
    EXPECT_FALSE(yt::parseProgress("SELECTA-PROGRESS 1e300 10").has_value());
    EXPECT_FALSE(yt::parseProgress("[download] 10% of 4MiB").has_value());

    const auto format = yt::parseFormat("SELECTA-FORMAT 140 m4a mp4a.40.2 129.478 201");
    ASSERT_TRUE(format.has_value());
    EXPECT_EQ(format->formatId, u"140"_s);
    EXPECT_EQ(format->extension, u"m4a"_s);
    EXPECT_EQ(format->codec, u"mp4a.40.2"_s);
    EXPECT_TRUE(yt::parseFormat("SELECTA-FORMAT 251-drc webm none NA NA")->codec.isEmpty());
    EXPECT_FALSE(yt::parseFormat("SELECTA-FORMAT 140 ../../x").has_value()); // the extension names a file
    EXPECT_FALSE(yt::parseFormat("SELECTA-FORMAT 140").has_value());

    EXPECT_EQ(yt::parseFile("SELECTA-FILE /Users/x/Library/Caches/Selecta Previews/yt/KmTopic00Ex.m4a"),
              u"/Users/x/Library/Caches/Selecta Previews/yt/KmTopic00Ex.m4a"_s); // spaces kept
    EXPECT_FALSE(yt::parseFile("SELECTA-FILE ").has_value());
    EXPECT_FALSE(yt::parseFile("WARNING: SELECTA-FILE /x").has_value());
}

TEST(YtDlpOutput, FailuresMapToClearErrors)
{
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: This content isn't available, try again later. The current "
                      "session has been rate-limited by YouTube for up to an hour."}),
              ErrorCode::RateLimited);
    EXPECT_EQ(codeOf({"ERROR: Unable to download API page: HTTP Error 429: Too Many Requests"}),
              ErrorCode::RateLimited);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: Sign in to confirm you’re not a bot. Use --cookies"}),
              ErrorCode::BotCheck);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: YouTube is requiring a captcha challenge before playback"}),
              ErrorCode::BotCheck);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: Sign in to confirm your age. This video may be inappropriate"}),
              ErrorCode::AgeRestricted);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: Private video. Sign in if you've been granted access"}),
              ErrorCode::Unavailable); // a sign-in hint, but not a bot check
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: The uploader has not made this video available in your country"}),
              ErrorCode::GeoBlocked);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: This video is DRM protected"}), ErrorCode::Unavailable);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: Video unavailable. This video has been removed"}),
              ErrorCode::Unavailable);
    const auto noRuntime = {"WARNING: [youtube] No supported JavaScript runtime could be found.",
                            "ERROR: [youtube] AAAAAAAAAAA: Requested format is not available."};
    EXPECT_EQ(codeOf(noRuntime), ErrorCode::ToolMissingRuntime);
    EXPECT_EQ(codeOf({"ERROR: [youtube] AAAAAAAAAAA: Requested format is not available."}, 1, false),
              ErrorCode::ToolMissingRuntime);
    EXPECT_EQ(codeOf({"WARNING: [youtube] AAAAAAAAAAA: n challenge solving failed: Some formats may be missing.",
                      "ERROR: [youtube] AAAAAAAAAAA: Requested format is not available."}),
              ErrorCode::ToolTooOld);
    EXPECT_EQ(codeOf({"ERROR: unable to download video data: HTTP Error 403: Forbidden"}), ErrorCode::ToolTooOld);
    EXPECT_EQ(codeOf({"yt-dlp: error: no such option: --no-remote-components"}, 2), ErrorCode::ToolTooOld);
    EXPECT_EQ(codeOf({"ERROR: Unable to download API page: <urlopen error [Errno 8] nodename nor servname provided>"}),
              ErrorCode::Network);
    EXPECT_EQ(codeOf({"ERROR: File is larger than max-filesize (300000000 bytes > 209715200 bytes). Aborting."}),
              ErrorCode::TooLarge);
    EXPECT_EQ(codeOf({"yt-dlp: error: invalid --progress-template"}, 2), ErrorCode::ToolFailed);
    EXPECT_EQ(codeOf({}, 1), ErrorCode::ToolFailed);

    EXPECT_TRUE(yt::startsPause(ErrorCode::RateLimited));
    EXPECT_TRUE(yt::startsPause(ErrorCode::BotCheck));
    EXPECT_FALSE(yt::startsPause(ErrorCode::AgeRestricted));
    const MetaError limited = yt::classifyFailure(lines({"ERROR: HTTP Error 429: Too Many Requests"}), 1, true);
    EXPECT_EQ(limited.retryAfter, std::chrono::seconds(3600));
    EXPECT_EQ(limited.service, Service::YtDlp);
}

TEST(YtDlpOutput, GenericFailuresNameTheReasonWithoutTheHomeFolder)
{
    const QByteArray line = "ERROR: [generic] KmTopic00Ex: something odd in " + QDir::homePath().toUtf8() + "/x";
    const MetaError e = yt::classifyFailure({line}, 1, true);
    EXPECT_EQ(e.code, ErrorCode::ToolFailed);
    EXPECT_TRUE(e.message.contains(u"something odd in ~/x"_s)) << e.message.toStdString();
    EXPECT_FALSE(e.message.contains(QDir::homePath()));
    EXPECT_FALSE(e.message.contains(u"[generic]"_s));
}

// --- pick(): the labelled table of §12.3 ----------------------------------------------------------------------------

TEST(YtDlpPick, PrefersTheTopicChannelWithinThreeSeconds)
{
    const QList<YouTubeCandidate> results = yt::parseSearch(fixture(u"youtube/search_glass_harbour.json"_s)).value();
    const std::optional<YouTubePick> pick =
        YtDlpRunner::pick(results, track(u"Glass Harbour - Extended Mix"_s, 391000));
    ASSERT_TRUE(pick.has_value());
    EXPECT_EQ(pick->candidate.id, u"KmTopic00Ex"_s); // over the artist's own upload with more views
    EXPECT_TRUE(pick->reasons.contains(u"official Topic channel"_s));
    EXPECT_EQ(pick->reasons.first(), u"length Δ 0 s"_s);
    // The radio edit goes to its own Topic upload; the remix to the remix.
    EXPECT_EQ(picked(results, track(u"Glass Harbour - Radio Edit"_s, 201500)), u"KmTopic00Re"_s);
    EXPECT_EQ(
        picked(results, track(u"Glass Harbour - Night Tram Remix"_s, 412000, {u"Kestrel Moon"_s, u"Night Tram"_s})),
        u"NightTram01"_s);
}

TEST(YtDlpPick, LengthWindowIsThreeSeconds)
{
    const QList<YouTubeCandidate> results{
        video(u"AAAAAAAAAA1"_s, u"Kestrel Moon - Glass Harbour"_s, u"Someone"_s, 205),
        video(u"AAAAAAAAAA2"_s, u"Kestrel Moon - Glass Harbour"_s, u"Someone"_s, 206)};
    EXPECT_EQ(picked(results, track(u"Glass Harbour"_s, 202400)), u"AAAAAAAAAA1"_s); // 202 s: Δ 3 fits, Δ 4 doesn't
    EXPECT_TRUE(picked(results, track(u"Glass Harbour"_s, 210000)).isEmpty());
    Release noLength = track(u"Glass Harbour"_s, 0);
    noLength.durationMs.reset();
    EXPECT_FALSE(YtDlpRunner::pick(results, noLength).has_value()); // ±3 s can't be checked
}

TEST(YtDlpPick, DecoysAreSkipped)
{
    const QList<YouTubeCandidate> results{
        video(u"AAAAAAAAAA1"_s, u"Kestrel Moon - Glass Harbour (Slowed + Reverb)"_s, u"Lofi"_s, 202, 9000000),
        video(u"AAAAAAAAAA2"_s, u"Kestrel Moon - Glass Harbour (Mixed)"_s, u"Mixes"_s, 202, 5000000),
        video(u"AAAAAAAAAA3"_s, u"Kestrel Moon - Glass Harbour (Live at Harbourfront)"_s, u"Venue"_s, 202, 4000000),
        video(u"AAAAAAAAAA4"_s, u"Kestrel Moon - Harbour Lights"_s, u"Kestrel Moon"_s, 202, 3000000),
        video(u"AAAAAAAAAA5"_s, u"Kestrel Moon - Glass Harbour (Official Video)"_s, u"Some Label"_s, 202, 1000)};
    EXPECT_EQ(picked(results, track(u"Glass Harbour"_s, 202000)), u"AAAAAAAAAA5"_s);
    YouTubeCandidate live = video(u"AAAAAAAAAA6"_s, u"Kestrel Moon - Glass Harbour"_s, u"Kestrel Moon"_s, 202);
    live.liveStatus = u"is_live"_s;
    EXPECT_TRUE(picked({live}, track(u"Glass Harbour"_s, 202000)).isEmpty());
    // Asked for, the derivative is fine.
    EXPECT_EQ(picked(results, track(u"Glass Harbour (Slowed + Reverb)"_s, 202000)), u"AAAAAAAAAA1"_s);
}

TEST(YtDlpPick, ArtistAndLabelChannelsThenViews)
{
    const QList<YouTubeCandidate> results{
        video(u"AAAAAAAAAA1"_s, u"Kestrel Moon - Glass Harbour"_s, u"Random Reuploads"_s, 202, 9000000, true),
        video(u"AAAAAAAAAA2"_s, u"Kestrel Moon - Glass Harbour"_s, u"Lantern Grove Records"_s, 202, 1000),
        video(u"AAAAAAAAAA3"_s, u"Kestrel Moon - Glass Harbour"_s, u"Another Reupload"_s, 202, 50)};
    const std::optional<YouTubePick> pick = YtDlpRunner::pick(results, track(u"Glass Harbour"_s, 202000));
    ASSERT_TRUE(pick.has_value());
    EXPECT_EQ(pick->candidate.id, u"AAAAAAAAAA2"_s); // the label's channel
    EXPECT_TRUE(pick->reasons.contains(u"the artist's or label's channel"_s));
    const QList<YouTubeCandidate> strangers{results.at(0), results.at(2)};
    EXPECT_EQ(picked(strangers, track(u"Glass Harbour"_s, 202000)), u"AAAAAAAAAA1"_s); // verified, then views
}

TEST(YtDlpPick, SearchTextNamesTheVersion)
{
    EXPECT_EQ(YtDlpRunner::searchText(track(u"Glass Harbour - Extended Mix"_s, 391000)),
              u"Kestrel Moon Glass Harbour Extended Mix"_s);
    EXPECT_EQ(YtDlpRunner::searchText(track(u"Glass Harbour (Night Tram Remix)"_s, 412000)),
              u"Kestrel Moon Glass Harbour Night Tram Remix"_s);
    EXPECT_EQ(YtDlpRunner::searchText(track(u"Glass Harbour"_s, 202000, {u"Kestrel Moon feat. Clara Vale"_s})),
              u"Kestrel Moon Glass Harbour"_s);
}
