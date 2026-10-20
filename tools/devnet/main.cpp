// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-devnet: a local Soulseek network for trying Selecta by hand (Network.h). A test server and peers that share
// a folder of folders (each subfolder is one peer) or a generated corpus of synthetic tones. It binds to 127.0.0.1
// only and never contacts any other host.
//
//   selecta-devnet --generate ~/devnet-corpus        # first run: write the corpus (needs ffmpeg), then serve it
//   selecta-devnet --library ~/devnet-corpus         # later runs; devnet.conf in the folder sets peer behaviour
//   selecta-devnet --library ~/music-test --speed 256 --queue alice=60 --refuse bob="Pending shutdown." --deny 10
#include "Behaviour.h"
#include "Corpus.h"
#include "Library.h"
#include "Network.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTime>

#include <cstdio>
#include <optional>

using namespace Qt::StringLiterals;
using selecta::devnet::Behaviours;

namespace {

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

void printLine(FILE *stream, const QString &text)
{
    const QByteArray line = text.toUtf8() + '\n';
    if (std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stream) !=
            static_cast<std::size_t>(line.size()) ||
        std::fflush(stream) != 0) {
        return; // stdout or stderr is gone: there is nowhere else to report it
    }
}

// An integer option within [lowest, highest], or nullopt after saying what's wrong.
std::optional<qint64> number(const QCommandLineParser &parser, const QCommandLineOption &option, qint64 lowest,
                             qint64 highest)
{
    bool ok = false;
    const qint64 value = parser.value(option).toLongLong(&ok);
    if (!ok || value < lowest || value > highest) {
        printLine(stderr,
                  u"--%1 takes a number from %2 to %3"_s.arg(option.names().constLast()).arg(lowest).arg(highest));
        return std::nullopt;
    }
    return value;
}

struct Options
{
    selecta::devnet::NetworkOptions network;
    QString library;
    QString generate;
    Behaviours behaviours; // from --queue and --refuse; they win over devnet.conf
    bool quiet = false;
};

struct Parser
{
    QCommandLineParser parser;
    QCommandLineOption port{
        {u"p"_s, u"port"_s}, u"Server port on 127.0.0.1; 0 picks a free one."_s, u"port"_s, u"2242"_s};
    QCommandLineOption library{u"library"_s, u"Share this folder: each subfolder is one peer."_s, u"dir"_s};
    QCommandLineOption generate{
        u"generate"_s, u"Write a synthetic corpus into this new folder (needs ffmpeg), then share it."_s, u"dir"_s};
    QCommandLineOption seed{u"seed"_s, u"Seed for the corpus and the peers' behaviour."_s, u"n"_s, u"1"_s};
    QCommandLineOption user{u"user"_s, u"Only this account may log in (with --password)."_s, u"name"_s};
    QCommandLineOption password{u"password"_s, u"Password of the --user account."_s, u"password"_s};
    QCommandLineOption speed{u"speed"_s, u"Upload speed per transfer in KiB/s, before each peer's own factor."_s,
                             u"KiB/s"_s, u"1024"_s};
    QCommandLineOption queueWait{
        u"queue-wait"_s, u"Longest a queued download waits for its upload to start, in seconds."_s, u"s"_s, u"2"_s};
    QCommandLineOption queue{u"queue"_s, u"This peer keeps each queued download this long before uploading it."_s,
                             u"peer=seconds"_s};
    QCommandLineOption refuse{u"refuse"_s, u"This peer refuses every download (default reason: Too many files)."_s,
                              u"peer[=reason]"_s};
    QCommandLineOption deny{u"deny"_s, u"Percentage of queue requests refused with a realistic reason."_s, u"percent"_s,
                            u"0"_s};
    QCommandLineOption searchDelay{u"search-delay"_s, u"Longest a peer takes to answer a search, in ms."_s, u"ms"_s,
                                   u"1200"_s};
    QCommandLineOption quiet{{u"q"_s, u"quiet"_s}, u"Log logins and transfers only, not every message."_s};

    Parser()
    {
        parser.setApplicationDescription(
            u"A local Soulseek network on 127.0.0.1 for trying Selecta: a test server, and peers sharing a folder of "
            "folders (each subfolder is one peer, named after it) or a generated corpus of synthetic tones with "
            "invented names. It never contacts any other host."_s);
        parser.addHelpOption();
        parser.addOptions(
            {port, library, generate, seed, user, password, speed, queueWait, queue, refuse, deny, searchDelay, quiet});
    }
};

bool readBehaviourOptions(const Parser &p, Behaviours &behaviours)
{
    const QStringList queues = p.parser.values(p.queue);
    for (const QString &value : queues) {
        if (!selecta::devnet::addQueueOption(behaviours, value)) {
            printLine(stderr, u"--queue takes <peer>=<seconds>, not %1"_s.arg(value));
            return false;
        }
    }
    const QStringList refusals = p.parser.values(p.refuse);
    for (const QString &value : refusals) {
        if (!selecta::devnet::addRefuseOption(behaviours, value)) {
            printLine(stderr, u"--refuse takes <peer> or <peer>=<reason>, not %1"_s.arg(value));
            return false;
        }
    }
    return true;
}

std::optional<Options> parse(const QCoreApplication &app)
{
    Parser p;
    p.parser.process(app);
    const auto portValue = number(p.parser, p.port, 0, 65535);
    const auto seedValue = number(p.parser, p.seed, 0, 0xFFFF'FFFF);
    const auto speedValue = number(p.parser, p.speed, 0, qint64{1024} * 1024);
    const auto waitValue = number(p.parser, p.queueWait, 0, qint64{24} * 3600);
    const auto denyValue = number(p.parser, p.deny, 0, 100);
    const auto delayValue = number(p.parser, p.searchDelay, 0, 60'000);
    if (!portValue || !seedValue || !speedValue || !waitValue || !denyValue || !delayValue) {
        return std::nullopt;
    }
    Options options;
    options.network.port = static_cast<quint16>(*portValue);
    options.network.seed = static_cast<quint32>(*seedValue);
    options.network.uploadRate = *speedValue * 1024;
    options.network.queueWait = std::chrono::seconds(*waitValue);
    options.network.denyPercent = static_cast<int>(*denyValue);
    options.network.searchDelay = std::chrono::milliseconds(*delayValue);
    options.library = p.parser.value(p.library);
    options.generate = p.parser.value(p.generate);
    options.quiet = p.parser.isSet(p.quiet);
    if (p.parser.isSet(p.user) != p.parser.isSet(p.password)) {
        printLine(stderr, u"--user and --password go together"_s);
        return std::nullopt;
    }
    options.network.user = p.parser.value(p.user);
    options.network.password = p.parser.value(p.password);
    if (options.library.isEmpty() == options.generate.isEmpty()) {
        printLine(stderr, u"pass either --library <dir> or --generate <dir> (see --help)"_s);
        return std::nullopt;
    }
    if (!readBehaviourOptions(p, options.behaviours)) {
        return std::nullopt;
    }
    return options;
}

// Writes the corpus and returns its folder, or an empty string.
QString generateCorpus(const QString &directory, quint32 seed)
{
    if (const QDir existing(directory); existing.exists() && !existing.isEmpty()) {
        printLine(stderr, u"%1 already holds files: share it with --library, or pick a new folder"_s.arg(directory));
        return {};
    }
    const QString ffmpeg = selecta::devnet::findFfmpeg();
    if (ffmpeg.isEmpty()) {
        printLine(stderr, u"ffmpeg isn't on PATH (brew bundle installs it)"_s);
        return {};
    }
    const selecta::devnet::CorpusPlan plan = selecta::devnet::planCorpus(seed);
    printLine(stdout, u"writing %1 tracks and %2 other files into %3"_s.arg(plan.tracks.size())
                          .arg(plan.extras.size())
                          .arg(QDir::toNativeSeparators(directory)));
    QString error;
    const bool written = selecta::devnet::writeCorpus(plan, directory, ffmpeg, &error,
                                                      [](const QString &path) { printLine(stdout, u"  "_s + path); });
    if (!written) {
        printLine(stderr, error);
        return {};
    }
    return directory;
}

// The folder's devnet.conf, if any, under the command line's --queue and --refuse.
Behaviours behavioursFor(const QString &folder, const Behaviours &fromCommandLine)
{
    Behaviours behaviours;
    QFile file(QDir(folder).filePath(selecta::devnet::behaviourFileName()));
    if (file.open(QIODevice::ReadOnly)) {
        QStringList errors;
        behaviours = selecta::devnet::parseBehaviours(QString::fromUtf8(file.readAll()), &errors);
        for (const QString &error : std::as_const(errors)) {
            printLine(stderr, u"%1, ignored: %2"_s.arg(file.fileName(), error));
        }
    }
    for (auto it = fromCommandLine.constBegin(); it != fromCommandLine.constEnd(); ++it) {
        behaviours.insert(it.key(), it.value());
    }
    return behaviours;
}

} // namespace

int main(int argc, char *argv[])
{
    const QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(u"selecta-devnet"_s);
    std::optional<Options> options = parse(app);
    if (!options) {
        return kExitUsage;
    }
    const QString folder =
        options->generate.isEmpty() ? options->library : generateCorpus(options->generate, options->network.seed);
    if (folder.isEmpty()) {
        return kExitFailed;
    }
    if (!QFileInfo(folder).isDir()) {
        printLine(stderr, u"%1 isn't a folder"_s.arg(folder));
        return kExitFailed;
    }
    QStringList skipped;
    const QList<selecta::devnet::LibraryPeer> peers = selecta::devnet::scanLibrary(folder, &skipped);
    for (const QString &name : std::as_const(skipped)) {
        printLine(stderr, u"skipped %1: not a valid Soulseek username"_s.arg(name));
    }
    if (peers.isEmpty()) {
        printLine(stderr, u"nothing to share: %1 has no subfolders"_s.arg(folder));
        return kExitFailed;
    }
    options->network.behaviours = behavioursFor(folder, options->behaviours);

    selecta::devnet::DevNetwork network(options->network);
    network.setVerbose(!options->quiet);
    QObject::connect(&network, &selecta::devnet::DevNetwork::logLine, &network, [](const QString &line) {
        printLine(stdout, QTime::currentTime().toString(u"HH:mm:ss.zzz"_s) + u"  "_s + line);
    });
    QString error;
    if (!network.start(&error)) {
        printLine(stderr, error);
        return kExitFailed;
    }
    for (const selecta::devnet::LibraryPeer &peer : peers) {
        if (!network.addPeer(peer, &error)) {
            printLine(stderr, error);
            return kExitFailed;
        }
    }
    printLine(stdout, u"selecta-devnet is up. Server for Selecta's settings: 127.0.0.1:%1"_s.arg(network.port()));
    printLine(stdout, u"peers: %1"_s.arg(network.peerNames().join(u", "_s)));
    printLine(stdout, options->network.user.isEmpty() ? u"logins: any username and password"_s
                                                      : u"logins: only %1"_s.arg(options->network.user));
    if (const QString tracks = QDir(folder).filePath(u"tracklist.txt"_s); QFileInfo::exists(tracks)) {
        printLine(stdout, u"tracks to look for: %1"_s.arg(QDir::toNativeSeparators(tracks)));
    }
    return QCoreApplication::exec();
}
