// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli collection scan <root>…: scan music folders, read-only, the way core does; print counts, timings, album
// folders and the files that couldn't be read. For debugging against real folders: it writes nothing anywhere and
// touches no network.
#include "Command.h"
#include "collection/Albums.h"
#include "collection/Scanner.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMap>

#include <cstdio>

using namespace Qt::StringLiterals;
using selecta::cli::printLine;

namespace {

constexpr int kDefaultShown = 25;

struct Options
{
    QStringList roots;
    int readers = 4;
    bool rescan = false; // scan a second time with the first scan as `known`: the stat-only path
    bool list = false;   // print every file read
    int shown = kDefaultShown;
};

QString usage()
{
    return u"usage: selecta-cli collection scan <root>… [--readers <n>] [--rescan] [--list] [--show <n>]"_s;
}

bool parse(const QStringList &arguments, Options &options, QString &error)
{
    if (arguments.isEmpty() || arguments.first() != u"scan"_s) {
        error = usage();
        return false;
    }
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        const bool hasValue = i + 1 < arguments.size();
        bool ok = true;
        if (argument == u"--readers"_s && hasValue) {
            options.readers = arguments.at(++i).toInt(&ok);
        } else if (argument == u"--show"_s && hasValue) {
            options.shown = arguments.at(++i).toInt(&ok);
        } else if (argument == u"--rescan"_s) {
            options.rescan = true;
        } else if (argument == u"--list"_s) {
            options.list = true;
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

QString describe(const selecta::collection::ScannedFile &file)
{
    const selecta::match::AudioQuality &q = file.quality;
    return u"%1 | %2 – %3 | %4 | %5 s | %6 kbps %7-bit %8 Hz%9"_s
        .arg(file.file.path, file.artist, file.title, file.album)
        .arg(static_cast<double>(file.durationMs) / 1000.0, 0, 'f', 1)
        .arg(q.bitrateKbps)
        .arg(q.bitDepth)
        .arg(q.sampleRateHz)
        .arg(file.namesFromFileName ? u" (names from the path)"_s : QString());
}

void printSummary(const selecta::collection::ScanResult &result, qint64 elapsedMs, const QString &label)
{
    const qsizetype read = result.added.size() + result.changed.size();
    printLine(stdout, u"%1: %2 ms; added %3, changed %4, removed %5, unreadable %6%7"_s.arg(label)
                          .arg(elapsedMs)
                          .arg(result.added.size())
                          .arg(result.changed.size())
                          .arg(result.removed.size())
                          .arg(result.unreadable.size())
                          .arg(elapsedMs > 0 && read > 0
                                   ? u" (%1 files/s)"_s.arg(
                                         static_cast<double>(read) * 1000.0 / static_cast<double>(elapsedMs), 0, 'f', 0)
                                   : QString()));
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    Options options;
    QString error;
    if (!parse(arguments, options, error)) {
        printLine(stderr, error);
        return selecta::cli::kExitUsage;
    }
    selecta::collection::ScanOptions scanOptions;
    scanOptions.readers = options.readers;
    printLine(stdout, u"roots      %1 (read-only, %2 readers)"_s.arg(options.roots.join(u", ")).arg(options.readers));

    QElapsedTimer timer;
    timer.start();
    int lastPercent = -1;
    const selecta::collection::ScanResult first =
        selecta::collection::scan(options.roots, {}, scanOptions, nullptr, [&lastPercent](int done, int total) {
            const int percent = total > 0 ? (done * 100) / total : 100;
            if (percent / 10 != lastPercent / 10) {
                lastPercent = percent;
                std::fprintf(stderr, "\r  reading %d/%d (%d%%)", done, total, percent); // NOLINT(cert-err33-c)
            }
        });
    const qint64 firstMs = timer.elapsed();
    std::fputs("\n", stderr); // NOLINT(cert-err33-c)
    printSummary(first, firstMs, u"first scan"_s);

    QMap<QString, int> codecs;
    int fromNames = 0;
    for (const selecta::collection::ScannedFile &file : first.added) {
        ++codecs[file.file.path.section(u'.', -1).toLower()];
        fromNames += file.namesFromFileName ? 1 : 0;
        if (options.list) {
            printLine(stdout, describe(file));
        }
    }
    QStringList formats;
    for (auto it = codecs.cbegin(); it != codecs.cend(); ++it) {
        formats.append(u"%1 %2"_s.arg(it.key()).arg(it.value()));
    }
    printLine(stdout, u"formats    %1"_s.arg(formats.join(u", ")));
    printLine(stdout, u"untagged   %1 file(s) named from their path"_s.arg(fromNames));

    timer.restart();
    QHash<QString, quint64> albumIds;
    const QList<selecta::match::LibraryAlbum> albums = selecta::collection::groupAlbums(first.added, &albumIds);
    const auto various = std::ranges::count_if(albums, [](const auto &album) { return album.variousArtists; });
    printLine(stdout, u"albums     %1 (%2 various artists), %3 tracks in albums, grouped in %4 ms"_s.arg(albums.size())
                          .arg(various)
                          .arg(albumIds.size())
                          .arg(timer.elapsed()));

    if (!first.unreadable.isEmpty()) {
        printLine(stdout, u"unreadable (first %1):"_s.arg(options.shown));
        for (qsizetype i = 0; i < first.unreadable.size() && i < options.shown; ++i) {
            printLine(stdout, u"  %1: %2"_s.arg(first.unreadable.at(i).first, first.unreadable.at(i).second));
        }
    }

    if (options.rescan) {
        QHash<QString, selecta::collection::FileSignature> known;
        for (const selecta::collection::ScannedFile &file : first.added) {
            known.insert(file.file.path, file.file);
        }
        timer.restart();
        const selecta::collection::ScanResult second = selecta::collection::scan(options.roots, known, scanOptions);
        printSummary(second, timer.elapsed(), u"rescan    "_s);
    }
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"collection", u"scan <root>… [--readers <n>] [--rescan] [--list] [--show <n>]",
    u"Scan music folders read-only like the collection scanner; print counts, timings, albums and unreadable files.",
    &run};

} // namespace
