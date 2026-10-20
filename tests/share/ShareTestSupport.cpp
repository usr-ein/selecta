// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShareTestSupport.h"

#include "SyntheticAudio.h"
#include "slsk/PeerMessages.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace selecta::share::test {

Tree::Tree()
{
    EXPECT_TRUE(m_dir.isValid());
}

QString Tree::file(const QString &relative, const QByteArray &bytes) const
{
    QString absolute = path(relative);
    if (!QDir().mkpath(QFileInfo(absolute).path())) {
        ADD_FAILURE() << "can't create the folder of " << absolute.toStdString();
        return {};
    }
    QFile file(absolute);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        ADD_FAILURE() << "can't write " << absolute.toStdString();
        return {};
    }
    return absolute;
}

QByteArray Tree::nativeFile(const QByteArray &relative, const QByteArray &bytes) const
{
    QByteArray absolute = QFile::encodeName(root()) + '/' + relative;
    const int fd = ::open(absolute.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); // NOLINT(*-vararg)
    if (fd < 0) {
        ADD_FAILURE() << "can't create " << absolute.toStdString();
        return {};
    }
    const ssize_t written = ::write(fd, bytes.constData(), static_cast<size_t>(bytes.size()));
    ::close(fd);
    if (written != bytes.size()) {
        ADD_FAILURE() << "can't write " << absolute.toStdString();
        return {};
    }
    return absolute;
}

QString Tree::folder(const QString &relative) const
{
    const QString absolute = path(relative);
    EXPECT_TRUE(QDir().mkpath(absolute)) << absolute.toStdString();
    return absolute;
}

bool Tree::symlink(const QString &target, const QString &relative) const
{
    const QString link = path(relative);
    QDir().mkpath(QFileInfo(link).path());
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
}

bool setHidden([[maybe_unused]] const QString &path)
{
#ifdef UF_HIDDEN
    struct stat status{};
    const QByteArray native = QFile::encodeName(path);
    if (::lstat(native.constData(), &status) != 0) {
        return false;
    }
    return ::lchflags(native.constData(), status.st_flags | UF_HIDDEN) == 0;
#else
    return false;
#endif
}

qint64 modifiedMs(const struct stat &status)
{
#ifdef Q_OS_DARWIN
    const struct timespec &time = status.st_mtimespec;
#else
    const struct timespec &time = status.st_mtim;
#endif
    return (static_cast<qint64>(time.tv_sec) * 1000) + (static_cast<qint64>(time.tv_nsec) / 1'000'000);
}

bool setMode(const QString &path, int mode)
{
    return ::chmod(QFile::encodeName(path).constData(), static_cast<mode_t>(mode)) == 0;
}

QByteArray mp3Cbr320()
{
    static const QByteArray bytes = testing::makeMp3(testing::tones({440.0, 1250.0}, 2.0), {.bitrateKbps = 320});
    return bytes;
}

QByteArray mp3Vbr()
{
    static const QByteArray bytes =
        testing::makeMp3(testing::tones({440.0, 1250.0}, 2.0), {.vbr = true, .vbrQuality = 2});
    return bytes;
}

QByteArray flac(int bits)
{
    static const QByteArray flac16 =
        testing::makeFlac(testing::quantised(testing::tones({440.0}, 2.0), 16), {.bits = 16});
    static const QByteArray flac24 =
        testing::makeFlac(testing::quantised(testing::tones({440.0}, 2.0, 48000), 24), {.bits = 24});
    return bits == 24 ? flac24 : flac16;
}

collection::ScannedFile scanned(const QString &path, const match::AudioQuality &quality, qint64 durationMs)
{
    collection::ScannedFile file;
    struct stat status{};
    EXPECT_EQ(::stat(QFile::encodeName(path).constData(), &status), 0) << path.toStdString();
    file.file = {path, static_cast<qint64>(status.st_size), modifiedMs(status)};
    file.quality = quality;
    file.durationMs = durationMs;
    return file;
}

BuildResult buildTree(const Tree &tree, const QHash<QString, collection::ScannedFile> &knownAudio,
                      const BuildOptions &options)
{
    return build({{tree.root(), u"Music"_s}}, knownAudio, options);
}

slsk::SharedFileList decodedList(const ShareIndex &index)
{
    const std::optional<slsk::peer::SharedFileListResponse> decoded =
        slsk::peer::SharedFileListResponse::decode(index.sharedFileList());
    EXPECT_TRUE(decoded.has_value());
    return decoded ? decoded->list : slsk::SharedFileList{};
}

QStringList listedFiles(const ShareIndex &index)
{
    QStringList files;
    const slsk::SharedFileList list = decodedList(index);
    for (const slsk::SharedDirectory &directory : list.directories) {
        for (const slsk::FileEntry &file : directory.files) {
            files.append(directory.path.display() + u'\\' + file.name.display());
        }
    }
    return files;
}

QStringList listedFolders(const ShareIndex &index)
{
    QStringList folders;
    const slsk::SharedFileList list = decodedList(index);
    for (const slsk::SharedDirectory &directory : list.directories) {
        folders.append(directory.path.display());
    }
    return folders;
}

QStringList displayPaths(const QList<slsk::FileEntry> &entries)
{
    QStringList paths;
    for (const slsk::FileEntry &entry : entries) {
        paths.append(entry.name.display());
    }
    return paths;
}

QStringList unreadablePaths(const BuildResult &result)
{
    QStringList paths;
    for (const auto &[path, reason] : result.unreadable) {
        paths.append(path);
    }
    return paths;
}

std::optional<slsk::FileEntry> listedEntry(const ShareIndex &index, const QString &virtualPath)
{
    const slsk::SharedFileList list = decodedList(index);
    for (const slsk::SharedDirectory &directory : list.directories) {
        for (const slsk::FileEntry &file : directory.files) {
            if (directory.path.display() + u'\\' + file.name.display() == virtualPath) {
                return file;
            }
        }
    }
    return std::nullopt;
}

} // namespace selecta::share::test
