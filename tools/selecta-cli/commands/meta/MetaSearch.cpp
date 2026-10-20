// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli meta-search "artist - title": resolve one tracklist line to ranked releases the way Get tracks does
// (Spotify when a Client ID and secret are known, else Deezer), optionally with MusicBrainz's label and genres.
// A manual smoke tool for the real services: run it by hand, sparingly; never from tests or CI.
#include "Command.h"
#include "MetaSession.h"
#include "match/Tracklist.h"

#include <QCommandLineParser>

#include <cstdio>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;
using selecta::cli::MetaSession;
using selecta::cli::printLine;

namespace {

constexpr int kShownCandidates = 5;
constexpr qsizetype kMaxLines = 20;

std::optional<selecta::meta::ResolveQuery> queryFor(const QString &text, const QCommandLineParser &parser)
{
    const selecta::match::TracklistLine line = selecta::match::TracklistParser::parseLine(text);
    if (line.kind != selecta::match::TracklistLineKind::Track) {
        printLine(stderr, u"Not a track line (empty, a heading, a link or an ID): %1"_s.arg(text));
        return std::nullopt;
    }
    selecta::meta::ResolveQuery query;
    query.artist = line.artist;
    query.title = line.title;
    query.label = parser.isSet(u"label"_s) ? parser.value(u"label"_s) : line.labelHint;
    if (line.durationSec > 0) {
        query.durationMs = qint64{line.durationSec} * 1000;
    }
    if (parser.isSet(u"length"_s)) {
        query.durationMs = selecta::cli::parseLength(parser.value(u"length"_s));
        if (!query.durationMs.has_value()) {
            printLine(stderr, u"--length takes m:ss or seconds."_s);
            return std::nullopt;
        }
    }
    return query;
}

// Resolves one line and prints the outcome. False when the resolution failed.
bool resolveLine(const selecta::meta::ResolveQuery &query, const QCommandLineParser &parser, MetaSession &session)
{
    printLine(stdout, u"\nline: artist \"%1\", title \"%2\", length %3%4"_s.arg(
                          query.artist, query.title, selecta::cli::formatLength(query.durationMs),
                          query.label.isEmpty() ? QString() : u", label \"%1\""_s.arg(query.label)));
    const auto result = MetaSession::wait(session.resolver().resolve(query, selecta::meta::Priority::Interactive));
    if (!result.has_value()) {
        printLine(stderr, u"No answer in time."_s);
        return false;
    }
    if (!result->ok()) {
        printLine(stderr, result->error().toString());
        return false;
    }
    const selecta::meta::Resolution &resolution = result->value();
    printLine(stdout, selecta::cli::describeResolution(resolution).chopped(1));
    const qsizetype shown = parser.isSet(u"all"_s)
                                ? resolution.candidates.size()
                                : std::min<qsizetype>(kShownCandidates, resolution.candidates.size());
    for (qsizetype i = 0; i < shown; ++i) {
        printLine(stdout,
                  selecta::cli::describeCandidate(static_cast<int>(i + 1), resolution.candidates.at(i)).chopped(1));
    }
    if (shown < resolution.candidates.size()) {
        printLine(stdout, u"   … %1 more (--all)"_s.arg(resolution.candidates.size() - shown));
    }
    if (parser.isSet(u"enrich"_s) && !resolution.candidates.isEmpty()) {
        return selecta::cli::printEnrichment(resolution.candidates.first().release, session.musicBrainz());
    }
    return true;
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    MetaSession::addCommonOptions(parser);
    parser.addOption({u"length"_s, u"The track's length, for the ±3 s comparison."_s, u"m:ss"_s});
    parser.addOption({u"label"_s, u"A label hint."_s, u"name"_s});
    parser.addOption({u"enrich"_s, u"Also ask MusicBrainz for the first candidate's label and genres."_s});
    parser.addOption({u"all"_s, u"List every candidate, not only the first five."_s});
    const bool parsed = parser.parse(QStringList{u"meta-search"_s} + arguments);
    const QStringList lines = parsed ? parser.positionalArguments() : QStringList();
    if (lines.isEmpty() || lines.size() > kMaxLines) {
        printLine(stderr, u"usage: selecta-cli meta-search [--length m:ss] [--label name] [--enrich] [--all] "
                          u"[--verbose] [--no-spotify] [--secrets file] \"artist - title\" [up to 20 lines]"_s);
        return selecta::cli::kExitUsage;
    }
    QList<selecta::meta::ResolveQuery> queries;
    for (const QString &text : lines) {
        const std::optional<selecta::meta::ResolveQuery> query = queryFor(text, parser);
        if (!query.has_value()) {
            return selecta::cli::kExitUsage;
        }
        queries.append(*query);
    }

    MetaSession session(MetaSession::commonOptions(parser));
    session.start(); // one sign-in for all the lines
    int failures = 0;
    for (const selecta::meta::ResolveQuery &query : std::as_const(queries)) {
        failures += resolveLine(query, parser, session) ? 0 : 1;
    }
    return failures == 0 ? selecta::cli::kExitOk : selecta::cli::kExitFailed;
}

const selecta::cli::CommandRegistration registration{
    u"meta-search", u"[--length m:ss] [--enrich] [--verbose] \"artist - title\" […]",
    u"Resolve a tracklist line to ranked releases (Spotify or Deezer), as Get tracks does. Manual smoke tool.", &run};

} // namespace
