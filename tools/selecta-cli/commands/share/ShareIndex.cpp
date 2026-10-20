// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli share index <root>…: build the share index of folders the way core does, read-only, and print counts,
// timings, the files that can't be shared, and the answers peers would get: searches, the browse list, folder
// listings and upload lookups. Touches no network and writes nothing anywhere.
//
//   selecta-cli share index ~/Music --search "daft punk -live" --browse --show 40
//   ... --exclude ~/Music/Inbox --phrase "banned words" --folder "Music\Artist\Album" --lookup "Music\Artist\x.mp3"
//
// Without a collection scan to take attributes from, every audio file's header is probed (core passes the collection's
// facts instead), so the first build of a large folder takes longer here than in the app.
#include "share/ShareIndex.h"

#include "Command.h"
#include "share/Open.h"
#include "slsk/PeerMessages.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <cstdio>

using namespace Qt::StringLiterals;
using selecta::cli::printLine;
namespace share = selecta::share;
namespace slsk = selecta::slsk;

namespace {

constexpr int kDefaultShown = 25;

struct Options
{
    QStringList roots;
    QStringList aliases;
    QStringList excluded;
    QStringList searches;
    QStringList phrases;
    QStringList folders;
    QStringList lookups;
    int cap = share::kDefaultSearchCap;
    bool browse = false;
    int shown = kDefaultShown;
};

QString usage()
{
    return u"usage: selecta-cli share index <root>… [--alias <name>]… [--exclude <folder>]… [--search <query>]… "
           "[--phrase <excluded phrase>]… [--cap <n>] [--browse] [--folder <virtual path>]… "
           "[--lookup <virtual path>]… [--show <n>]"_s;
}

bool parse(const QStringList &arguments, Options &options, QString &error)
{
    if (arguments.isEmpty() || arguments.first() != u"index"_s) {
        error = usage();
        return false;
    }
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        const bool hasValue = i + 1 < arguments.size();
        bool ok = true;
        if (argument == u"--alias"_s && hasValue) {
            options.aliases.append(arguments.at(++i));
        } else if (argument == u"--exclude"_s && hasValue) {
            options.excluded.append(QFileInfo(arguments.at(++i)).absoluteFilePath());
        } else if (argument == u"--search"_s && hasValue) {
            options.searches.append(arguments.at(++i));
        } else if (argument == u"--phrase"_s && hasValue) {
            options.phrases.append(arguments.at(++i));
        } else if (argument == u"--folder"_s && hasValue) {
            options.folders.append(arguments.at(++i));
        } else if (argument == u"--lookup"_s && hasValue) {
            options.lookups.append(arguments.at(++i));
        } else if (argument == u"--cap"_s && hasValue) {
            options.cap = arguments.at(++i).toInt(&ok);
        } else if (argument == u"--show"_s && hasValue) {
            options.shown = arguments.at(++i).toInt(&ok);
        } else if (argument == u"--browse"_s) {
            options.browse = true;
        } else if (argument.startsWith(u"--"_s)) {
            ok = false;
        } else {
            options.roots.append(QFileInfo(argument).absoluteFilePath());
        }
        if (!ok) {
            error = u"bad argument: %1\n%2"_s.arg(argument, usage());
            return false;
        }
    }
    if (options.roots.isEmpty()) {
        error = usage();
        return false;
    }
    return true;
}

QString megabytes(qint64 bytes)
{
    constexpr qint64 kKilo = 1000;
    if (bytes < 10 * kKilo) {
        return u"%1 B"_s.arg(bytes);
    }
    if (bytes < 10 * kKilo * kKilo) {
        return u"%1 KB"_s.arg(bytes / kKilo);
    }
    return u"%1 MB"_s.arg(static_cast<double>(bytes) / 1e6, 0, 'f', 1);
}

QString describe(const slsk::FileEntry &file)
{
    const slsk::FileAttributes &a = file.attributes;
    QStringList facts{megabytes(static_cast<qint64>(file.size))};
    if (a.bitrate) {
        facts.append(u"%1 kbps%2"_s.arg(*a.bitrate).arg(a.vbr.value_or(false) ? u" VBR"_s : QString()));
    }
    if (a.sampleRate) {
        facts.append(u"%1 Hz"_s.arg(*a.sampleRate));
    }
    if (a.bitDepth) {
        facts.append(u"%1-bit"_s.arg(*a.bitDepth));
    }
    if (a.duration) {
        facts.append(u"%1:%2"_s.arg(*a.duration / 60).arg(*a.duration % 60, 2, 10, QChar(u'0')));
    }
    return u"%1  [%2]"_s.arg(file.name.display(), facts.join(u", "_s));
}

void printBrowse(const share::ShareIndex &index, int shown)
{
    const std::optional<slsk::peer::SharedFileListResponse> decoded =
        slsk::peer::SharedFileListResponse::decode(index.sharedFileList());
    if (!decoded) {
        printLine(stderr, u"browse     the share list doesn't decode"_s);
        return;
    }
    printLine(stdout, u"browse     %1 folders (first %2 lines):"_s.arg(decoded->list.directories.size()).arg(shown));
    int lines = 0;
    for (const slsk::SharedDirectory &directory : decoded->list.directories) {
        if (lines++ >= shown) {
            return;
        }
        printLine(stdout, u"  %1 (%2 files)"_s.arg(directory.path.display()).arg(directory.files.size()));
        for (const slsk::FileEntry &file : directory.files) {
            if (lines++ >= shown) {
                return;
            }
            printLine(stdout, u"    "_s + describe(file));
        }
    }
}

void printSearches(const share::ShareIndex &index, const Options &options)
{
    for (const QString &query : options.searches) {
        QElapsedTimer timer;
        timer.start();
        const QList<slsk::FileEntry> results = index.search(query, options.phrases, options.cap);
        const double ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        printLine(stdout, u"search     \"%1\": %2 files in %3 ms"_s.arg(query).arg(results.size()).arg(ms, 0, 'f', 3));
        for (qsizetype i = 0; i < results.size() && i < options.shown; ++i) {
            printLine(stdout, u"  "_s + describe(results.at(i)));
        }
    }
}

void printFolders(const share::ShareIndex &index, const Options &options)
{
    for (const QString &folder : options.folders) {
        const QList<slsk::SharedDirectory> listing = index.folderContents(slsk::RemotePath::fromText(folder));
        if (listing.isEmpty()) {
            printLine(stdout, u"folder     \"%1\": unknown"_s.arg(folder));
            continue;
        }
        printLine(
            stdout,
            u"folder     \"%1\": %2 files"_s.arg(listing.first().path.display()).arg(listing.first().files.size()));
        for (const slsk::FileEntry &file : listing.first().files) {
            printLine(stdout, u"  "_s + describe(file));
        }
    }
}

void printLookups(const share::ShareIndex &index, const Options &options)
{
    for (const QString &path : options.lookups) {
        const std::optional<share::SharedFile> file = index.lookup(slsk::RemotePath::fromText(path));
        if (!file) {
            printLine(stdout, u"lookup     \"%1\": File not shared."_s.arg(path));
            continue;
        }
        const share::Opened opened = share::open(*file);
        printLine(stdout, u"lookup     \"%1\" → %2 (%3)"_s.arg(
                              path, file->localPath, opened.ok() ? u"opens"_s : opened.error + u" "_s + opened.detail));
    }
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    Options options;
    QString error;
    if (!parse(arguments, options, error)) {
        printLine(stderr, error);
        return selecta::cli::kExitUsage;
    }
    QList<share::ShareRoot> roots;
    for (qsizetype i = 0; i < options.roots.size(); ++i) {
        const QString &root = options.roots.at(i);
        roots.append({root, i < options.aliases.size() ? options.aliases.at(i) : QFileInfo(root).fileName()});
    }
    share::BuildOptions buildOptions;
    buildOptions.excludedFolders = options.excluded;

    QElapsedTimer timer;
    timer.start();
    const share::BuildResult result = share::build(roots, {}, buildOptions);
    const qint64 elapsedMs = timer.elapsed();
    const share::ShareIndex &index = *result.index;

    QStringList shared;
    const QList<share::ShareRoot> built = index.roots();
    for (const share::ShareRoot &root : built) {
        shared.append(u"%1 → %2"_s.arg(root.path, root.alias));
    }
    printLine(stdout, u"roots      %1 (read-only)"_s.arg(shared.join(u", "_s)));
    const share::ShareIndex::Counts counts = index.counts();
    printLine(stdout,
              u"built      in %1 ms: %2 files in %3 folders; index %4, share list %5 compressed"_s.arg(elapsedMs)
                  .arg(counts.files)
                  .arg(counts.folders)
                  .arg(megabytes(index.memoryBytes()), megabytes(index.sharedFileList().size())));
    if (!result.unreadable.isEmpty()) {
        printLine(stdout, u"unreadable %1 (first %2):"_s.arg(result.unreadable.size()).arg(options.shown));
        for (qsizetype i = 0; i < result.unreadable.size() && i < options.shown; ++i) {
            printLine(stdout, u"  %1: %2"_s.arg(result.unreadable.at(i).first, result.unreadable.at(i).second));
        }
    }
    printSearches(index, options);
    printFolders(index, options);
    printLookups(index, options);
    if (options.browse) {
        printBrowse(index, options.shown);
    }
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"share",
    u"index <root>… [--alias <name>]… [--exclude <folder>]… [--search <query>]… [--phrase <text>]… [--cap <n>] "
    u"[--browse] [--folder <virtual path>]… [--lookup <virtual path>]… [--show <n>]",
    u"Build the share index of folders read-only; print counts, timings, unshareable files and peers' answers.", &run};

} // namespace
