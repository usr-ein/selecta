// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli meta-isrc <isrc>: what each service knows about one recording — Spotify's and Deezer's releases with
// that ISRC, then MusicBrainz's label, catalogue number and genres (D30). A manual smoke tool for the real services:
// run it by hand, sparingly; never from tests or CI.
#include "Command.h"
#include "MetaSession.h"
#include "meta/detail/Identifiers.h"

#include <QCommandLineParser>

#include <cstdio>

using namespace Qt::StringLiterals;
using selecta::cli::MetaSession;
using selecta::cli::printLine;

namespace {

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    MetaSession::addCommonOptions(parser);
    parser.addOption({u"no-enrich"_s, u"Skip MusicBrainz."_s});
    if (!parser.parse(QStringList{u"meta-isrc"_s} + arguments) || parser.positionalArguments().size() != 1) {
        printLine(stderr, u"usage: selecta-cli meta-isrc [--no-enrich] [--verbose] [--no-spotify] [--secrets file] "
                          u"<isrc>"_s);
        return selecta::cli::kExitUsage;
    }
    selecta::meta::ResolveQuery query;
    query.isrc = selecta::meta::detail::normalizedIsrc(parser.positionalArguments().constFirst());
    if (query.isrc.isEmpty()) {
        printLine(stderr, u"Not an ISRC: two letters, three letters or digits, two digits, five digits."_s);
        return selecta::cli::kExitUsage;
    }

    MetaSession session(MetaSession::commonOptions(parser));
    session.start();
    printLine(stdout, u"ISRC %1"_s.arg(query.isrc));
    const auto result = MetaSession::wait(session.resolver().resolve(query, selecta::meta::Priority::Interactive));
    if (!result.has_value()) {
        printLine(stderr, u"No answer in time."_s);
        return selecta::cli::kExitFailed;
    }
    if (!result->ok()) {
        printLine(stderr, result->error().toString());
        return selecta::cli::kExitFailed;
    }
    const selecta::meta::Resolution &resolution = result->value();
    printLine(stdout, selecta::cli::describeResolution(resolution).chopped(1));
    for (qsizetype i = 0; i < resolution.candidates.size(); ++i) {
        printLine(stdout,
                  selecta::cli::describeCandidate(static_cast<int>(i + 1), resolution.candidates.at(i)).chopped(1));
    }
    if (parser.isSet(u"no-enrich"_s)) {
        return selecta::cli::kExitOk;
    }
    selecta::meta::Release subject;
    if (!resolution.candidates.isEmpty()) {
        subject = resolution.candidates.first().release;
    }
    subject.isrc = query.isrc; // MusicBrainz looks the ISRC up even when no service had it
    return selecta::cli::printEnrichment(subject, session.musicBrainz()) ? selecta::cli::kExitOk
                                                                         : selecta::cli::kExitFailed;
}

const selecta::cli::CommandRegistration registration{
    u"meta-isrc", u"[--no-enrich] [--verbose] <isrc>",
    u"Look one recording up by ISRC: Spotify, Deezer, then MusicBrainz's label and genres. Manual smoke tool.", &run};

} // namespace
