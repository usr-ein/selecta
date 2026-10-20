// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/Scanner.h"

#include "collection/detail/FileReader.h"
#include "collection/detail/Walk.h"

#include <QDir>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

namespace selecta::collection {

namespace {

constexpr int kMaxReaders = 16;

struct Job
{
    qsizetype file = 0; // index into Walk::files
    bool known = false; // changed rather than added
};

// The files whose signature differs from `known`, and the known paths seen on disk (matched exactly, else by key).
std::vector<Job> compare(const detail::Walk &walk, const QHash<QString, FileSignature> &known, QSet<QString> &seenKnown)
{
    QHash<QString, QString> knownByKey;
    knownByKey.reserve(known.size());
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        knownByKey.insert(comparisonKey(it.key()), it.key());
    }
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < walk.files.size(); ++i) {
        const detail::WalkedFile &file = walk.files[i];
        QString knownPath = known.contains(file.signature.path) ? file.signature.path : knownByKey.value(file.key);
        if (!knownPath.isEmpty() && seenKnown.contains(knownPath)) {
            knownPath.clear(); // two paths on disk for one known entry: the second is new
        }
        if (knownPath.isEmpty()) {
            jobs.push_back({static_cast<qsizetype>(i), false});
            continue;
        }
        seenKnown.insert(knownPath);
        const FileSignature &before = known.value(knownPath);
        if (before.size != file.signature.size || before.modifiedMs != file.signature.modifiedMs) {
            jobs.push_back({static_cast<qsizetype>(i), true});
        }
    }
    return jobs;
}

// Reads the jobs on up to `readers` threads of their own; `progress` runs on the calling thread.
std::vector<detail::ReadOutcome> readAll(const detail::Walk &walk, const std::vector<Job> &jobs,
                                         const ScanOptions &options, const audio::CancellationFlag *cancel,
                                         const std::function<void(int, int)> &progress, bool &cancelled)
{
    const auto total = static_cast<qsizetype>(jobs.size());
    std::vector<detail::ReadOutcome> outcomes(jobs.size());
    std::vector<bool> finished(jobs.size(), false);
    if (progress) {
        progress(0, static_cast<int>(total));
    }
    if (total == 0) {
        return outcomes;
    }
    std::atomic<qsizetype> next{0};
    std::mutex mutex;
    std::condition_variable wake;
    qsizetype done = 0;
    int running = 0;
    const auto work = [&] {
        while (cancel == nullptr || !cancel->isCancelled()) {
            const qsizetype i = next.fetch_add(1);
            if (i >= total) {
                break;
            }
            const Job &job = jobs[static_cast<std::size_t>(i)];
            const detail::WalkedFile &file = walk.files[static_cast<std::size_t>(job.file)];
            const detail::WalkedFolder &folder = walk.folders[static_cast<std::size_t>(file.folder)];
            const QString &root = walk.roots.at(folder.root);
            const QString relative = file.signature.path.mid(root.size() + 1);
            detail::ReadOutcome outcome = detail::readFile(file.signature, relative, folder.audioNames, options.tags);
            const std::scoped_lock lock(mutex);
            outcomes[static_cast<std::size_t>(i)] = std::move(outcome);
            finished[static_cast<std::size_t>(i)] = true;
            ++done;
            wake.notify_one();
        }
        const std::scoped_lock lock(mutex);
        --running;
        wake.notify_one();
    };
    const int threads = static_cast<int>(std::min<qsizetype>(std::clamp(options.readers, 1, kMaxReaders), total));
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    {
        const std::scoped_lock lock(mutex);
        running = threads;
    }
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back(work);
    }
    qsizetype reported = 0;
    std::unique_lock lock(mutex);
    while (running > 0 || reported != done) {
        wake.wait(lock, [&] { return reported != done || running == 0; });
        if (reported != done && progress) {
            reported = done;
            const auto doneNow = static_cast<int>(reported);
            lock.unlock(); // the callback may take its time: readers keep going
            progress(doneNow, static_cast<int>(total));
            lock.lock();
        } else {
            reported = done;
        }
    }
    lock.unlock();
    for (std::thread &thread : pool) {
        thread.join();
    }
    cancelled = std::ranges::find(finished, false) != finished.end();
    if (cancelled) {
        // Unfinished slots stay as they are (Unreadable with no reason): drop them, keep what was read.
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            if (!finished[i]) {
                outcomes[i].status = detail::ReadOutcome::Status::Vanished;
            }
        }
    }
    return outcomes;
}

bool lessByPath(const ScannedFile &a, const ScannedFile &b)
{
    return a.file.path < b.file.path;
}

} // namespace

QString comparisonKey(const QString &path)
{
    const QString absolute = QDir::isAbsolutePath(path) ? path : QDir::current().absoluteFilePath(path);
    return QDir::cleanPath(absolute).normalized(QString::NormalizationForm_C);
}

ScanResult scan(const QStringList &roots, const QHash<QString, FileSignature> &known, const ScanOptions &options,
                const audio::CancellationFlag *cancel, const std::function<void(int done, int total)> &progress)
{
    ScanResult result;
    const detail::Walk walk = detail::walk(roots, options, cancel);
    result.unreadable = walk.unreadable;
    if (walk.cancelled) {
        result.cancelled = true;
        return result;
    }
    QSet<QString> seenKnown;
    const std::vector<Job> jobs = compare(walk, known, seenKnown);
    bool cancelled = false;
    const std::vector<detail::ReadOutcome> outcomes = readAll(walk, jobs, options, cancel, progress, cancelled);
    QSet<QString> vanished; // keys of files gone between the walk and the read
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        const detail::ReadOutcome &outcome = outcomes[i];
        const detail::WalkedFile &file = walk.files[static_cast<std::size_t>(jobs[i].file)];
        switch (outcome.status) {
        case detail::ReadOutcome::Status::Read:
            (jobs[i].known ? result.changed : result.added).append(outcome.file);
            break;
        case detail::ReadOutcome::Status::Unreadable:
            result.unreadable.append({file.signature.path, outcome.reason});
            break;
        case detail::ReadOutcome::Status::Vanished:
            vanished.insert(file.key);
            break;
        }
    }
    std::ranges::sort(result.added, lessByPath);
    std::ranges::sort(result.changed, lessByPath);
    if (cancelled) {
        result.cancelled = true;
        return result; // removals need the whole picture
    }
    if (!walk.complete) {
        return result;
    }
    // Known paths under a root that weren't seen are gone, unless their folder couldn't be looked at.
    QStringList rootKeys;
    for (const QString &root : walk.roots) {
        rootKeys.append(comparisonKey(root));
    }
    const auto under = [](const QString &key, const QStringList &folders) {
        return std::ranges::any_of(folders, [&key](const QString &folder) { return detail::isUnder(key, folder); });
    };
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        if (seenKnown.contains(it.key())) {
            continue;
        }
        const QString key = comparisonKey(it.key());
        if (!under(key, rootKeys) || under(key, walk.blindFolders) || walk.presentUnread.contains(key)) {
            continue;
        }
        result.removed.append(it.key());
    }
    // A file that vanished during the read is gone too (it was seen, so it isn't in the loop above).
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        if (seenKnown.contains(it.key()) && vanished.contains(comparisonKey(it.key()))) {
            result.removed.append(it.key());
        }
    }
    result.removed.sort();
    result.removed.removeDuplicates();
    return result;
}

} // namespace selecta::collection
