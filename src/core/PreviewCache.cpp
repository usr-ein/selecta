// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/PreviewCache.h"

#include "library/Naming.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

Q_LOGGING_CATEGORY(lcPreviewCache, "selecta.core.previewcache")

constexpr int kIndexVersion = 1;
constexpr qsizetype kMaxEntries = 100'000; // far above what 2 GB holds; bounds a damaged index
constexpr int kMaxNameBytes = 120;         // a reserved file's name after its random prefix
constexpr int kReserveAttempts = 4;
constexpr qint64 kExpiryMs = std::chrono::duration_cast<std::chrono::milliseconds>(PreviewCache::kExpiry).count();

const QString &indexName()
{
    static const QString name = u"index.json"_s;
    return name;
}

bool usableText(const QString &text)
{
    return !text.isEmpty() && text.size() <= PreviewCache::kMaxTextChars;
}

} // namespace

PreviewCache::PreviewCache(const QString &directory, Options options)
    : m_options(std::move(options))
{
    if (!m_options.now) {
        m_options.now = [] { return QDateTime::currentMSecsSinceEpoch(); };
    }
    if (directory.isEmpty()) {
        return;
    }
    if (!QDir().mkpath(directory)) {
        qCWarning(lcPreviewCache) << "can't create the preview cache" << directory;
        return;
    }
    const QFileInfo folder(directory);
    m_directory = folder.canonicalFilePath(); // every path below is built on it: containment checks compare these
    m_usable = !m_directory.isEmpty() && folder.isDir() && folder.isWritable();
    if (!m_usable) {
        qCWarning(lcPreviewCache) << "the preview cache can't be written:" << directory;
        m_directory.clear();
        return;
    }
    load();
    removeStrays(); // half-written files from a quit or a crash, and anything else the index doesn't list
    expire();
    trim();
    save();
}

PreviewCache::PreviewCache(const QString &directory)
    : PreviewCache(directory, Options{})
{}

PreviewCache::~PreviewCache() = default;

std::optional<PreviewCache::File> PreviewCache::find(const QString &key)
{
    const auto found = m_entries.find(key);
    if (found == m_entries.end()) {
        return std::nullopt;
    }
    const QString path = pathOf(found->file);
    const bool intact = relativeOf(path) == found->file && QFileInfo(path).size() == found->bytes;
    if (!intact || (isExpired(found->usedAt) && !isPinned(path))) {
        deleteFile(found->file); // changed on disk: of no use any more
        m_entries.erase(found);
        save();
        return std::nullopt;
    }
    found->usedAt = m_options.now();
    File file{path, found->bytes, found->note};
    save();
    return file;
}

QString PreviewCache::reserve(const QString &name)
{
    if (!m_usable || !QDir().mkpath(m_directory)) { // the folder may have been emptied under us (macOS purges caches)
        return {};
    }
    const QString safe = library::naming::safeComponent(name.isEmpty() ? u"preview"_s : name, kMaxNameBytes);
    for (int attempt = 0; attempt < kReserveAttempts; ++attempt) {
        const QString path =
            m_directory + u'/' + QString::number(QRandomGenerator::global()->generate64(), 36) + u'-' + safe;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            file.close();
            pin(path);
            return path;
        }
        if (!QFileInfo::exists(path)) {
            qCWarning(lcPreviewCache) << "can't create" << path << ':' << file.errorString();
            return {};
        }
    }
    return {};
}

bool PreviewCache::commit(const QString &key, const QString &path, const QString &note)
{
    const std::optional<QString> relative = relativeOf(path);
    const qint64 bytes = QFileInfo(path).size();
    if (!m_usable || !usableText(key) || !relative || bytes <= 0) {
        qCWarning(lcPreviewCache) << "not kept in the preview cache:" << path;
        return false;
    }
    if (const auto earlier = m_entries.constFind(key); earlier != m_entries.cend() && earlier->file != *relative) {
        deleteFile(earlier->file);
    }
    m_entries.removeIf([&relative](const auto &entry) { return entry.value().file == *relative; });
    m_entries.insert(key, Entry{*relative, bytes, m_options.now(), note.left(kMaxTextChars)});
    expire();
    trim();
    save();
    return true;
}

void PreviewCache::abandon(const QString &path)
{
    unpin(path);
    const std::optional<QString> relative = relativeOf(path);
    if (!relative || isPinned(path)) {
        return; // gone already, not a file of the cache's, or still held by someone else
    }
    for (const Entry &entry : std::as_const(m_entries)) {
        if (entry.file == *relative) {
            return; // kept under a key: not the caller's to drop
        }
    }
    if (!QFile::remove(path)) {
        qCWarning(lcPreviewCache) << "couldn't delete" << path;
    }
}

bool PreviewCache::remove(const QString &key)
{
    const auto found = m_entries.find(key);
    if (found == m_entries.end()) {
        return false;
    }
    deleteFile(found->file);
    m_entries.erase(found);
    save();
    return true;
}

void PreviewCache::pin(const QString &pathOrPrefix)
{
    if (!pathOrPrefix.isEmpty()) {
        ++m_pins[pathOrPrefix];
    }
}

void PreviewCache::unpin(const QString &pathOrPrefix)
{
    const auto found = m_pins.find(pathOrPrefix);
    if (found != m_pins.end() && --found.value() <= 0) {
        m_pins.erase(found);
    }
}

std::optional<QString> PreviewCache::recall(const QString &key)
{
    const auto found = m_notes.find(key);
    if (found == m_notes.end()) {
        return std::nullopt;
    }
    if (isExpired(found->usedAt)) {
        m_notes.erase(found);
        save();
        return std::nullopt;
    }
    found->usedAt = m_options.now();
    const QString value = found->value;
    save();
    return value;
}

void PreviewCache::remember(const QString &key, const QString &value)
{
    if (!m_usable || !usableText(key) || !usableText(value)) {
        return;
    }
    m_notes.insert(key, Note{value, m_options.now()});
    expire(); // keeps them within kMaxNotes
    save();
}

void PreviewCache::forget(const QString &key)
{
    if (m_notes.remove(key) > 0) {
        save();
    }
}

qint64 PreviewCache::totalBytes() const
{
    if (!m_usable) {
        return 0;
    }
    const QString index = indexPath();
    qint64 total = 0;
    QDirIterator it(m_directory, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo file = it.nextFileInfo();
        if (file.filePath() != index) {
            total += file.size();
        }
    }
    return total;
}

void PreviewCache::clear()
{
    if (!m_usable) {
        return;
    }
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (isPinned(pathOf(it->file))) {
            ++it; // playing: stays
            continue;
        }
        deleteFile(it->file);
        it = m_entries.erase(it);
    }
    removeStrays();
    m_notes.clear();
    save();
}

void PreviewCache::load()
{
    QFile file(indexPath());
    if (!file.exists()) {
        return;
    }
    if (file.size() > kMaxIndexBytes || !file.open(QIODevice::ReadOnly)) {
        qCWarning(lcPreviewCache) << "the preview cache's index can't be read; starting empty";
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.read(kMaxIndexBytes + 1), &error);
    const QJsonObject root = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        root.value("version"_L1).toInteger() != kIndexVersion) {
        qCWarning(lcPreviewCache) << "the preview cache's index is damaged or from another version; starting empty";
        return;
    }
    QSet<QString> files;
    const QJsonArray entries = root.value("files"_L1).toArray();
    for (const QJsonValue &value : entries) {
        if (m_entries.size() >= kMaxEntries) {
            break;
        }
        const QJsonObject object = value.toObject();
        const QString key = object.value("key"_L1).toString();
        const QString name = object.value("file"_L1).toString();
        const qint64 bytes = object.value("bytes"_L1).toInteger(-1);
        const qint64 usedAt = object.value("used"_L1).toInteger(-1);
        const QString note = object.value("note"_L1).toString();
        if (!usableText(key) || note.size() > kMaxTextChars || bytes <= 0 || usedAt < 0 || files.contains(name) ||
            m_entries.contains(key)) {
            continue;
        }
        const QString path = pathOf(name);
        if (path.isEmpty() || relativeOf(path) != name || QFileInfo(path).size() != bytes) {
            continue; // gone, changed, or not the cache's: a file left there goes with the strays
        }
        files.insert(name);
        m_entries.insert(key, Entry{name, bytes, usedAt, note});
    }
    const QJsonArray notes = root.value("notes"_L1).toArray();
    for (const QJsonValue &value : notes) {
        if (m_notes.size() >= kMaxNotes) {
            break;
        }
        const QJsonObject object = value.toObject();
        const QString key = object.value("key"_L1).toString();
        const QString text = object.value("value"_L1).toString();
        const qint64 usedAt = object.value("used"_L1).toInteger(-1);
        if (usableText(key) && usableText(text) && usedAt >= 0 && !m_notes.contains(key)) {
            m_notes.insert(key, Note{text, usedAt});
        }
    }
}

bool PreviewCache::save()
{
    if (!m_usable || !QDir().mkpath(m_directory)) {
        return false;
    }
    QJsonArray entries;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        QJsonObject object{
            {"key"_L1, it.key()}, {"file"_L1, it->file}, {"bytes"_L1, it->bytes}, {"used"_L1, it->usedAt}};
        if (!it->note.isEmpty()) {
            object.insert("note"_L1, it->note);
        }
        entries.append(object);
    }
    QJsonArray notes;
    for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) {
        notes.append(QJsonObject{{"key"_L1, it.key()}, {"value"_L1, it->value}, {"used"_L1, it->usedAt}});
    }
    const QJsonObject root{{"version"_L1, kIndexVersion}, {"files"_L1, entries}, {"notes"_L1, notes}};
    QSaveFile out(indexPath());
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
        qCWarning(lcPreviewCache) << "couldn't write" << indexPath() << ':' << out.errorString();
        return false;
    }
    return true;
}

void PreviewCache::removeStrays()
{
    QSet<QString> listed;
    for (const Entry &entry : std::as_const(m_entries)) {
        listed.insert(entry.file);
    }
    const QString index = indexPath();
    // Symbolic links are listed (and deleted as links), never followed into another folder.
    QDirIterator it(m_directory, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path == index || isPinned(path) || listed.contains(path.mid(m_directory.size() + 1))) {
            continue;
        }
        if (!QFile::remove(path)) {
            qCWarning(lcPreviewCache) << "couldn't delete" << path;
        }
    }
}

void PreviewCache::expire()
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (isExpired(it->usedAt) && !isPinned(pathOf(it->file))) {
            deleteFile(it->file);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    m_notes.removeIf([this](const auto &note) { return isExpired(note.value().usedAt); });
    if (m_notes.size() > kMaxNotes) {
        QList<std::pair<qint64, QString>> byUse;
        for (auto it = m_notes.cbegin(); it != m_notes.cend(); ++it) {
            byUse.append({it->usedAt, it.key()});
        }
        std::ranges::sort(byUse);
        for (qsizetype i = 0; i < byUse.size() - kMaxNotes; ++i) {
            m_notes.remove(byUse.at(i).second);
        }
    }
}

void PreviewCache::trim()
{
    qint64 total = 0;
    for (const Entry &entry : std::as_const(m_entries)) {
        total += entry.bytes;
    }
    if (total <= m_options.maxBytes) {
        return;
    }
    QList<std::pair<qint64, QString>> byUse; // least recently used first
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        byUse.append({it->usedAt, it.key()});
    }
    std::ranges::sort(byUse);
    for (const auto &use : std::as_const(byUse)) {
        if (total <= m_options.maxBytes) {
            break;
        }
        const Entry entry = m_entries.value(use.second);
        if (isPinned(pathOf(entry.file))) {
            continue;
        }
        deleteFile(entry.file);
        total -= entry.bytes;
        m_entries.remove(use.second);
    }
}

bool PreviewCache::isPinned(const QString &path) const
{
    if (path.isEmpty()) {
        return false;
    }
    for (auto it = m_pins.cbegin(); it != m_pins.cend(); ++it) {
        if (path.startsWith(it.key())) {
            return true;
        }
    }
    return false;
}

bool PreviewCache::isExpired(qint64 usedAt) const
{
    return usedAt < m_options.now() - kExpiryMs;
}

QString PreviewCache::pathOf(const QString &relative) const
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(u'\\') ||
        QDir::cleanPath(relative) != relative || relative == u".."_s || relative.startsWith(u"../"_s) ||
        relative == indexName()) {
        return {};
    }
    return m_directory + u'/' + relative;
}

std::optional<QString> PreviewCache::relativeOf(const QString &path) const
{
    const QFileInfo info(path);
    if (m_directory.isEmpty() || path.isEmpty() || info.isSymLink() || !info.isFile()) {
        return std::nullopt;
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !canonical.startsWith(m_directory + u'/')) {
        return std::nullopt;
    }
    QString relative = canonical.mid(m_directory.size() + 1);
    if (relative == indexName()) {
        return std::nullopt;
    }
    return relative;
}

void PreviewCache::deleteFile(const QString &relative) const
{
    const QString path = pathOf(relative);
    if (!path.isEmpty() && relativeOf(path) == relative && !QFile::remove(path)) {
        qCWarning(lcPreviewCache) << "couldn't delete" << path;
    }
}

QString PreviewCache::indexPath() const
{
    return m_directory + u'/' + indexName();
}

} // namespace selecta::core
