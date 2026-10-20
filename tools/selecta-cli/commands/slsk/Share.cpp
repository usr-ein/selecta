// SPDX-License-Identifier: GPL-3.0-or-later
// selecta-cli slsk-share: share a folder on a local test network: answer searches (every query word must be a word of
// the file's path), browses and folder listings, and upload what peers ask for. Runs until --for runs out or Ctrl-C.
// It prints "sharing N files ..." once logged in, then one line per answer and upload.
//
//   selecta-cli slsk-share --server 127.0.0.1:2242 --user bob --dir ~/devnet-corpus/cbr_carl [--for 60]
//   ... --advertise-port 1 (peers can't reach us directly: they ask the server to make us dial back)
#include "Command.h"
#include "commands/slsk/SlskSession.h"
#include "slsk/Providers.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <map>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

using selecta::cli::slsk::say;
using selecta::cli::slsk::SlskSession;
namespace slsk = selecta::slsk;

QStringList wordsOf(const QString &text)
{
    QStringList words;
    QString current;
    const QString folded = text.toCaseFolded();
    for (const QChar c : folded) {
        if (c.isLetterOrNumber()) {
            current += c;
        } else if (!current.isEmpty()) {
            words.append(std::exchange(current, {}));
        }
    }
    if (!current.isEmpty()) {
        words.append(current);
    }
    return words;
}

// The shared folder: virtual paths `<folder name>\<subfolders…>\<file>`, hidden entries skipped.
class ShareIndex final : public slsk::ShareProvider
{
public:
    explicit ShareIndex(const QString &folder)
    {
        const QDir root(folder);
        const QString top = root.dirName();
        std::map<QString, QList<slsk::FileEntry>> byFolder;
        QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info = it.nextFileInfo();
            const QString relative = root.relativeFilePath(info.filePath());
            if (relative.split(u'/').constLast().startsWith(u'.') || relative.contains(u"/."_s) ||
                relative.startsWith(u'.')) {
                continue;
            }
            const QString relativeFolder = root.relativeFilePath(info.path());
            QString virtualFolder = top;
            if (relativeFolder != u"."_s) {
                virtualFolder += u'\\' + QString(relativeFolder).replace(u'/', u'\\');
            }
            byFolder[virtualFolder].append(slsk::FileEntry{slsk::RemotePath::fromText(info.fileName()),
                                                           static_cast<quint64>(info.size()),
                                                           info.suffix().toLower(),
                                                           {}});
            m_local.insert(slsk::RemotePath::fromText(virtualFolder + u'\\' + info.fileName()).raw, info.filePath());
        }
        for (auto &[path, files] : byFolder) {
            m_list.directories.append(slsk::SharedDirectory{slsk::RemotePath::fromText(path), files});
        }
    }

    QByteArray sharedFileList(const QString & /*username*/) override
    {
        return slsk::peer::SharedFileListResponse{m_list}.encode();
    }

    QList<slsk::SharedDirectory> folderContents(const QString & /*username*/, const slsk::RemotePath &folder) override
    {
        QList<slsk::SharedDirectory> found;
        std::ranges::copy_if(m_list.directories, std::back_inserter(found),
                             [&](const slsk::SharedDirectory &directory) { return directory.path == folder; });
        return found;
    }

    // Files whose full path contains every word of `query` as a word.
    QList<slsk::FileEntry> search(const QString &query) const
    {
        const QStringList wanted = wordsOf(query);
        QList<slsk::FileEntry> hits;
        if (wanted.isEmpty()) {
            return hits;
        }
        for (const slsk::SharedDirectory &directory : m_list.directories) {
            for (const slsk::FileEntry &file : directory.files) {
                const QString path = directory.path.display() + u'\\' + file.name.display();
                const QStringList words = wordsOf(path);
                if (std::ranges::all_of(wanted, [&](const QString &word) { return words.contains(word); })) {
                    hits.append(slsk::FileEntry{slsk::RemotePath::fromText(path), file.size, file.extension, {}});
                }
            }
        }
        return hits;
    }

    QString localPath(const slsk::RemotePath &path) const { return m_local.value(path.raw); }
    int fileCount() const { return static_cast<int>(m_local.size()); }
    int folderCount() const { return static_cast<int>(m_list.directories.size()); }

private:
    slsk::SharedFileList m_list;
    QHash<QByteArray, QString> m_local;
};

class Info final : public slsk::UserInfoProvider
{
public:
    slsk::peer::UserInfoResponse userInfo(const QString & /*username*/) override
    {
        return slsk::peer::UserInfoResponse{
            u"selecta-cli slsk-share (a test peer)"_s, std::nullopt, 1, 0, true, std::nullopt};
    }
};

// What the signal handlers below work on.
struct Sharing
{
    SlskSession *session = nullptr;
    ShareIndex *index = nullptr;
    std::map<slsk::TransferId, std::unique_ptr<QFile>> open; // upload sources until each upload ends
};

void onSearch(const Sharing &sharing, const slsk::IncomingSearch &search)
{
    // Our own searches come back too (Soulfind sends every search to everyone).
    if (search.username == sharing.session->username()) {
        return;
    }
    const QList<slsk::FileEntry> hits = sharing.index->search(search.query);
    if (sharing.session->client().answerSearch(search, hits, slsk::Client::UploadAvailability{true, 0, 0})) {
        say(stdout, u"answered %1's search \"%2\" with %3 files"_s.arg(search.username, search.query).arg(hits.size()));
    }
}

void onUploadRequested(Sharing &sharing, const QString &user, const slsk::RemotePath &path)
{
    slsk::TransferManager &transfers = sharing.session->client().transfers();
    auto file = std::make_unique<QFile>(sharing.index->localPath(path));
    if (file->fileName().isEmpty() || !file->open(QIODevice::ReadOnly)) {
        transfers.denyUpload(user, path, u"File not shared."_s);
        say(stdout, u"refused %1: %2 isn't shared"_s.arg(user, path.display()));
        return;
    }
    const slsk::TransferId id =
        transfers.startUpload(slsk::UploadRequest{user, path, static_cast<quint64>(file->size())}, file.get());
    if (id != 0) {
        say(stdout, u"uploading %1 to %2"_s.arg(path.display(), user));
        sharing.open.emplace(id, std::move(file));
    }
}

void onUploadChanged(Sharing &sharing, const slsk::UploadInfo &info)
{
    if (!info.isTerminal()) {
        return;
    }
    say(stdout,
        u"upload of %1 to %2: %3 %4"_s.arg(info.request.path.display(), info.request.username,
                                           QString::fromLatin1(slsk::uploadStateName(info.state)), info.detail));
    sharing.open.erase(info.id); // closed after the terminal signal, as TransferManager asks
}

int run(const QStringList &arguments, const selecta::cli::Context & /*context*/)
{
    QCommandLineParser parser;
    SlskSession::addOptions(parser);
    parser.addOption({u"dir"_s, u"The folder to share."_s, u"folder"_s});
    parser.addOption({u"for"_s, u"Seconds to stay online (default: until Ctrl-C)."_s, u"seconds"_s, u"0"_s});
    if (!parser.parse(QStringList{u"slsk-share"_s} + arguments) || !parser.positionalArguments().isEmpty()) {
        say(stderr, parser.errorText().isEmpty() ? u"slsk-share takes no positional arguments"_s : parser.errorText());
        return selecta::cli::kExitUsage;
    }
    SlskSession session;
    bool ok = false;
    const int seconds = parser.value(u"for"_s).toInt(&ok);
    const QString folder = parser.value(u"dir"_s);
    if (!session.readOptions(parser) || !ok || seconds < 0) {
        return selecta::cli::kExitUsage;
    }
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
        say(stderr, u"--dir takes a folder to share"_s);
        return selecta::cli::kExitUsage;
    }
    ShareIndex index(QFileInfo(folder).absoluteFilePath());
    Info info;
    slsk::Client &client = session.client();
    client.peers().setShareProvider(&index);
    client.peers().setUserInfoProvider(&info);
    client.server().setSharedCounts(static_cast<quint32>(index.folderCount()), static_cast<quint32>(index.fileCount()));
    Sharing sharing{&session, &index, {}};
    const QObject receiver; // goes before `sharing`
    QObject::connect(&client, &slsk::Client::searchRequestReceived, &receiver,
                     [state = &sharing](const slsk::IncomingSearch &search) { onSearch(*state, search); });
    QObject::connect(&client.transfers(), &slsk::TransferManager::uploadRequested, &receiver,
                     [state = &sharing](const QString &user, const slsk::RemotePath &path, bool /*legacy*/) {
                         onUploadRequested(*state, user, path);
                     });
    QObject::connect(&client.transfers(), &slsk::TransferManager::uploadChanged, &receiver,
                     [state = &sharing](const slsk::UploadInfo &upload) { onUploadChanged(*state, upload); });

    if (!session.logIn()) {
        return selecta::cli::kExitFailed;
    }
    say(stdout, u"sharing %1 files in %2 folders as %3"_s.arg(index.fileCount())
                    .arg(index.folderCount())
                    .arg(session.username()));
    const auto limit = seconds > 0 ? std::chrono::milliseconds(std::chrono::seconds(seconds))
                                   : std::chrono::milliseconds(std::chrono::hours(24 * 365));
    SlskSession::waitUntil([&client] { return !client.server().isLoggedIn(); }, limit);
    const bool stillOnline = client.server().isLoggedIn();
    client.server().disconnectFromServer();
    if (!stillOnline) {
        say(stderr, u"the server ended the session"_s);
        return selecta::cli::kExitFailed;
    }
    return selecta::cli::kExitOk;
}

const selecta::cli::CommandRegistration registration{
    u"slsk-share", u"--server <host:port> --user <name> --dir <folder> [--for <s>] [--advertise-port <port>]",
    u"Share a folder on a local Soulseek test network: answer searches and browses, upload on request.", &run};

} // namespace
