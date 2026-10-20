// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestTree.h"

#include "SyntheticAudio.h"
#include "convert/TagWriter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>

#include <gtest/gtest.h>
#include <map>
#include <utility>

namespace selecta::collection::test {

namespace {

QByteArray generate(Format format, double seconds)
{
    const testing::Pcm pcm = testing::tones({440.0, 1250.0}, seconds);
    switch (format) {
    case Format::Mp3:
        return testing::makeMp3(pcm);
    case Format::Mp3Vbr:
        return testing::makeMp3(pcm, {.vbr = true, .vbrQuality = 2});
    case Format::Mp3NoLameTag:
        return testing::makeMp3(pcm, {.bitrateKbps = 192, .lameTag = false});
    case Format::Flac16:
        return testing::makeFlac(testing::quantised(pcm, 16), {.bits = 16});
    case Format::Flac24:
        return testing::makeFlac(testing::quantised(pcm, 24), {.bits = 24});
    case Format::Wav:
        return testing::makeWav(pcm, {.bits = 24});
    case Format::Aiff:
        return testing::makeAiff(pcm);
    case Format::M4aAac:
        return testing::makeM4a(pcm);
    case Format::M4aAacFastStart:
        return testing::makeM4a(pcm, {.fastStart = true});
    case Format::M4aAlac:
        return testing::makeM4a(pcm, {.alac = true});
    case Format::OggFlac:
        return testing::makeOggFlac(pcm);
    }
    return {};
}

} // namespace

QByteArray audioBytes(Format format, double seconds)
{
    static QMutex mutex;
    static std::map<std::pair<int, double>, QByteArray> cache;
    const QMutexLocker locker(&mutex);
    const std::pair<int, double> key{static_cast<int>(format), seconds};
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, generate(format, seconds)).first;
    }
    return it->second;
}

TestTree::TestTree()
{
    EXPECT_TRUE(m_dir.isValid());
}

QString TestTree::addFolder(const QString &relative) const
{
    QString full = path(relative);
    EXPECT_TRUE(QDir().mkpath(full)) << full.toStdString();
    return full;
}

QString TestTree::addBytes(const QString &relative, const QByteArray &bytes) const
{
    QString full = path(relative);
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile file(full);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        ADD_FAILURE() << "can't write " << full.toStdString();
        return {};
    }
    return full;
}

QString TestTree::add(const QString &relative, Format format, const Tags &tags, double seconds) const
{
    QString full = addBytes(relative, audioBytes(format, seconds));
    if (full.isEmpty() || tags.isEmpty()) {
        return full;
    }
    convert::MetadataModel model;
    for (auto it = tags.cbegin(); it != tags.cend(); ++it) {
        model.set(it.key(), it.value(), convert::Provenance::User);
    }
    const audio::Result<void> written = convert::writeTags(full, model);
    if (!written.ok()) {
        ADD_FAILURE() << "can't tag " << full.toStdString() << ": " << written.error().toString().toStdString();
        return {};
    }
    return full;
}

bool TestTree::setModified(const QString &path, qint64 msSinceEpoch)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return false;
    }
    return file.setFileTime(QDateTime::fromMSecsSinceEpoch(msSinceEpoch), QFileDevice::FileModificationTime);
}

QHash<QString, FileSignature> signaturesOf(const ScanResult &result)
{
    QHash<QString, FileSignature> known;
    for (const QList<ScannedFile> *list : {&result.added, &result.changed}) {
        for (const ScannedFile &file : *list) {
            known.insert(file.file.path, file.file);
        }
    }
    return known;
}

const ScannedFile *findFile(const QList<ScannedFile> &files, const QString &path)
{
    for (const ScannedFile &file : files) {
        if (file.file.path == path) {
            return &file;
        }
    }
    return nullptr;
}

} // namespace selecta::collection::test
