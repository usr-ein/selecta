// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/CollectionStore.h"

#include "storage/Database.h"

using namespace Qt::StringLiterals;

namespace selecta::core {
namespace {

using storage::Statement;

const QString kColumns = u"id, path, size, modified_ms, artist, title, album, album_artist, track_number, "
                         "disc_number, year, duration_ms, codec, bitrate, vbr, sample_rate, bit_depth, isrc, "
                         "names_from_file_name"_s;

CollectionFile readFile(const Statement &row)
{
    CollectionFile result;
    result.id = row.int64(0);
    collection::ScannedFile &file = result.file;
    file.file.path = row.text(1);
    file.file.size = row.int64(2);
    file.file.modifiedMs = row.int64(3);
    file.artist = row.text(4);
    file.title = row.text(5);
    file.album = row.text(6);
    file.albumArtist = row.text(7);
    file.trackNumber = static_cast<int>(row.int64(8));
    file.discNumber = static_cast<int>(row.int64(9));
    file.year = static_cast<int>(row.int64(10));
    file.durationMs = row.int64(11);
    file.quality.codec = static_cast<match::Codec>(row.int64(12));
    file.quality.bitrateKbps = static_cast<int>(row.int64(13));
    if (!row.isNull(14)) {
        file.quality.vbr = row.int64(14) != 0;
    }
    file.quality.sampleRateHz = static_cast<int>(row.int64(15));
    file.quality.bitDepth = static_cast<int>(row.int64(16));
    file.isrc = row.text(17);
    file.namesFromFileName = row.int64(18) != 0;
    return result;
}

// Whether `key` (a comparison key) is `folderKey` or inside it.
bool isUnder(const QString &key, const QString &folderKey)
{
    return key == folderKey || key.startsWith(folderKey.endsWith(u'/') ? folderKey : folderKey + u'/');
}

} // namespace

bool CollectionStore::apply(const QList<collection::ScannedFile> &added, const QList<collection::ScannedFile> &changed,
                            const QStringList &removed)
{
    storage::Transaction transaction(m_db);
    if (!transaction.isActive()) {
        return false;
    }
    Statement upsert = m_db.prepare(
        u"INSERT INTO collection_files (path, path_key, size, modified_ms, artist, title, album, album_artist, "
        "track_number, disc_number, year, duration_ms, codec, bitrate, vbr, sample_rate, bit_depth, isrc, "
        "names_from_file_name) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, "
        "?18, "
        "?19) ON CONFLICT (path_key) DO UPDATE SET path = excluded.path, size = excluded.size, "
        "modified_ms = excluded.modified_ms, artist = excluded.artist, title = excluded.title, album = excluded.album, "
        "album_artist = excluded.album_artist, track_number = excluded.track_number, "
        "disc_number = excluded.disc_number, year = excluded.year, duration_ms = excluded.duration_ms, "
        "codec = excluded.codec, bitrate = excluded.bitrate, vbr = excluded.vbr, sample_rate = excluded.sample_rate, "
        "bit_depth = excluded.bit_depth, isrc = excluded.isrc, names_from_file_name = excluded.names_from_file_name;");
    Statement remove = m_db.prepare(u"DELETE FROM collection_files WHERE path_key = ?1;");
    if (!upsert.isValid() || !remove.isValid()) {
        return false;
    }
    const auto write = [&upsert](const collection::ScannedFile &file) {
        upsert.reset();
        upsert.bind(1, file.file.path)
            .bind(2, collection::comparisonKey(file.file.path))
            .bind(3, file.file.size)
            .bind(4, file.file.modifiedMs)
            .bind(5, file.artist)
            .bind(6, file.title)
            .bind(7, file.album)
            .bind(8, file.albumArtist)
            .bind(9, file.trackNumber)
            .bind(10, file.discNumber)
            .bind(11, file.year)
            .bind(12, file.durationMs)
            .bind(13, static_cast<int>(file.quality.codec))
            .bind(14, file.quality.bitrateKbps);
        if (file.quality.vbr) {
            upsert.bind(15, *file.quality.vbr ? 1 : 0);
        } else {
            upsert.bindNull(15);
        }
        upsert.bind(16, file.quality.sampleRateHz)
            .bind(17, file.quality.bitDepth)
            .bind(18, file.isrc)
            .bind(19, file.namesFromFileName ? 1 : 0);
        return upsert.run();
    };
    for (const collection::ScannedFile &file : added) {
        if (!write(file)) {
            return false;
        }
    }
    for (const collection::ScannedFile &file : changed) {
        if (!write(file)) {
            return false;
        }
    }
    for (const QString &path : removed) {
        remove.reset();
        if (!remove.bind(1, collection::comparisonKey(path)).run()) {
            return false;
        }
    }
    return transaction.commit();
}

QHash<QString, collection::FileSignature> CollectionStore::signatures()
{
    QHash<QString, collection::FileSignature> result;
    Statement query = m_db.prepare(u"SELECT path, size, modified_ms FROM collection_files;");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        const QString path = query.text(0);
        result.insert(path, {path, query.int64(1), query.int64(2)});
    }
    return result;
}

QHash<QString, collection::FileSignature> CollectionStore::signaturesUnder(const QString &folder)
{
    const QString folderKey = collection::comparisonKey(folder);
    QHash<QString, collection::FileSignature> result;
    Statement query = m_db.prepare(u"SELECT path, size, modified_ms, path_key FROM collection_files;");
    while (query.isValid() && query.step() == Statement::Step::Row) {
        if (isUnder(query.text(3), folderKey)) {
            const QString path = query.text(0);
            result.insert(path, {path, query.int64(1), query.int64(2)});
        }
    }
    return result;
}

QList<CollectionFile> CollectionStore::all()
{
    QList<CollectionFile> files;
    Statement query = m_db.prepare(u"SELECT %1 FROM collection_files ORDER BY path;"_s.arg(kColumns));
    while (query.isValid() && query.step() == Statement::Step::Row) {
        files.append(readFile(query));
    }
    return files;
}

int CollectionStore::count()
{
    Statement query = m_db.prepare(u"SELECT COUNT(*) FROM collection_files;");
    return query.isValid() && query.step() == Statement::Step::Row ? static_cast<int>(query.int64(0)) : 0;
}

int CollectionStore::removeOutside(const QStringList &roots)
{
    QStringList rootKeys;
    for (const QString &root : roots) {
        rootKeys.append(collection::comparisonKey(root));
    }
    QList<qint64> outside;
    {
        Statement query = m_db.prepare(u"SELECT id, path_key FROM collection_files;");
        while (query.isValid() && query.step() == Statement::Step::Row) {
            const QString key = query.text(1);
            const bool inside =
                std::ranges::any_of(rootKeys, [&key](const QString &rootKey) { return isUnder(key, rootKey); });
            if (!inside) {
                outside.append(query.int64(0));
            }
        }
    }
    if (outside.isEmpty()) {
        return 0;
    }
    storage::Transaction transaction(m_db);
    Statement remove = m_db.prepare(u"DELETE FROM collection_files WHERE id = ?1;");
    if (!transaction.isActive() || !remove.isValid()) {
        return -1;
    }
    for (const qint64 id : std::as_const(outside)) {
        remove.reset();
        if (!remove.bind(1, id).run()) {
            return -1;
        }
    }
    return transaction.commit() ? static_cast<int>(outside.size()) : -1;
}

} // namespace selecta::core
