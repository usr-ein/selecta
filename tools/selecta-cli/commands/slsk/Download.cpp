// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli slsk-download: download one file from a user on a local test network. An existing target file is
// resumed. Prints each state change and the progress about once a second.
//
//   selecta-cli slsk-download --server 127.0.0.1:2242 --user alice bob 'Music\Album\01 Track.flac' [--to file]
#include "Command.h"
#include "commands/slsk/SlskSession.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <memory>

using namespace Qt::StringLiterals;

namespace {

using selecta::cli::slsk::say;
using selecta::cli::slsk::SlskSession;

// The download goes to one local file, opened for appending: its size is the resume offset.
class FileStorage final : public selecta::slsk::DownloadStorage
{
public:
    explicit FileStorage(const QString &path)
        : m_file(path)
    {}

    QIODevice *openDownload(const selecta::slsk::DownloadInfo & /*download*/) override
    {
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            say(stderr, u"can't write %1: %2"_s.arg(m_file.fileName(), m_file.errorString()));
            return nullptr;
        }
        return &m_file;
    }

    void closeDownload(const selecta::slsk::DownloadInfo & /*download*/, QIODevice *device) override
    {
        device->close();
    }

private:
    QFile m_file;
};

QString percent(quint64 done, quint64 total)
{
    return total > 0 ? u"%1%"_s.arg(done * 100 / total) : u"?"_s;
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    SlskSession::addOptions(parser);
    parser.addOption({u"to"_s, u"Where to write the file (default: its name, here)."_s, u"file"_s});
    parser.addOption({u"size"_s, u"The size from the search result, if the uploader reports 0."_s, u"bytes"_s});
    parser.addOption(
        {u"max-wait"_s, u"Seconds to wait in the uploader's queue and transfer."_s, u"seconds"_s, u"300"_s});
    parser.addPositionalArgument(u"peer"_s, u"The uploader."_s, u"<peer>"_s);
    parser.addPositionalArgument(u"path"_s, u"The file's path as the uploader shares it."_s, u"<path>"_s);
    if (!parser.parse(QStringList{u"slsk-download"_s} + arguments) || parser.positionalArguments().size() != 2) {
        say(stderr, parser.errorText().isEmpty() ? u"usage: selecta-cli slsk-download [options] <peer> <path>"_s
                                                 : parser.errorText());
        return selecta::cli::kExitUsage;
    }
    SlskSession session;
    bool sizeOk = true;
    bool waitOk = false;
    const quint64 size = parser.isSet(u"size"_s) ? parser.value(u"size"_s).toULongLong(&sizeOk) : 0;
    const int maxWait = parser.value(u"max-wait"_s).toInt(&waitOk);
    if (!session.readOptions(parser) || !sizeOk || !waitOk || maxWait <= 0) {
        return selecta::cli::kExitUsage;
    }
    const QString peer = parser.positionalArguments().at(0);
    const selecta::slsk::RemotePath path = selecta::slsk::RemotePath::fromText(parser.positionalArguments().at(1));
    const QString target = parser.isSet(u"to"_s) ? parser.value(u"to"_s) : path.display().section(u'\\', -1);
    if (target.isEmpty()) {
        say(stderr, u"no file name in that path: pass --to"_s);
        return selecta::cli::kExitUsage;
    }
    if (!session.logIn()) {
        return selecta::cli::kExitFailed;
    }
    selecta::governor::Governor &governor = session.governor();
    selecta::governor::Request request;
    request.peer = peer;
    selecta::governor::Decision decision = governor.request(selecta::governor::Operation::QueueDownload, request);
    if (!decision.permit) {
        say(stderr, u"governor: no download now: "_s + decision.explanation);
        return selecta::cli::kExitFailed;
    }

    selecta::slsk::TransferManager &transfers = session.client().transfers();
    FileStorage storage(target);
    transfers.setDownloadStorage(&storage);
    struct Progress
    {
        std::optional<selecta::slsk::DownloadInfo> last;
        bool started = false;
        QElapsedTimer sinceProgress;
    } progress;
    std::optional<selecta::slsk::DownloadInfo> &last = progress.last;
    const QObject receiver; // goes before `progress`
    QObject::connect(&transfers, &selecta::slsk::TransferManager::downloadChanged, &receiver,
                     [state = &progress, governorAt = &governor, peer](const selecta::slsk::DownloadInfo &info) {
                         const bool newState = !state->last || state->last->state != info.state;
                         state->last = info;
                         if (info.state == selecta::slsk::DownloadState::Transferring && !state->started) {
                             state->started = true;
                             governorAt->downloadStarted(peer);
                         }
                         if (info.placeInQueue) {
                             say(stdout, u"place in queue: %1"_s.arg(*info.placeInQueue));
                         } else if (newState && !info.isTerminal()) {
                             say(stdout, QString::fromLatin1(selecta::slsk::downloadStateName(info.state)));
                         }
                     });
    QObject::connect(&transfers, &selecta::slsk::TransferManager::downloadProgress, &receiver,
                     [state = &progress](selecta::slsk::TransferId, quint64 done, quint64 total) {
                         if (!state->sinceProgress.isValid() || state->sinceProgress.elapsed() >= 1000 ||
                             done == total) {
                             state->sinceProgress.start();
                             say(stdout, u"%1 (%2 of %3 bytes)"_s.arg(percent(done, total)).arg(done).arg(total));
                         }
                     });
    const selecta::slsk::TransferId id =
        transfers.download(selecta::slsk::DownloadRequest{peer, path, size, false}, std::move(*decision.permit));
    if (id == 0) {
        say(stderr, u"the download couldn't be queued"_s);
        return selecta::cli::kExitFailed;
    }
    governor.downloadQueued(peer);
    const bool finished =
        SlskSession::waitUntil([&] { return last && last->isTerminal(); }, std::chrono::seconds(maxWait));
    if (!finished) {
        transfers.cancelDownload(id);
    }
    const bool completed = last && last->state == selecta::slsk::DownloadState::Completed;
    if (progress.started) {
        governor.downloadFinished(peer, static_cast<qint64>(last ? last->received : 0), completed);
    } else {
        governor.downloadWithdrawn(peer);
    }
    if (last && last->rejection) {
        governor.peerDenied(peer, last->rejection->text);
    }
    transfers.setDownloadStorage(nullptr);
    session.client().server().disconnectFromServer();
    if (completed) {
        say(stdout, u"done: %1 (%2 bytes, %3 resumed)"_s.arg(QFileInfo(target).absoluteFilePath())
                        .arg(last->size)
                        .arg(last->startOffset));
        return selecta::cli::kExitOk;
    }
    QString why = u"gave up after %1 s"_s.arg(maxWait);
    if (last && last->isTerminal()) {
        why = QString::fromLatin1(selecta::slsk::transferErrorName(last->error));
        if (last->rejection) {
            why += u": "_s + last->rejection->text;
        } else if (!last->detail.isEmpty()) {
            why += u": "_s + last->detail;
        }
    }
    say(stderr, u"download failed: "_s + why);
    return selecta::cli::kExitFailed;
}

const selecta::cli::CommandRegistration registration{
    u"slsk-download", u"--server <host:port> --user <name> [--to <file>] <peer> <path>",
    u"Download one file from a user on a local Soulseek test network (resumes an existing file).", &run};

} // namespace
