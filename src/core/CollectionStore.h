// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace selecta::storage {
class Database;
}

namespace selecta::core {

// A file of the collection index, with its row id (the match::LibraryTrack id).
struct CollectionFile
{
    qint64 id = 0;
    collection::ScannedFile file;
};

// Reads and writes the collection index (table collection_files). Uses the Database on its owning thread; failures
// are reported through return values and the database's lastError().
class CollectionStore
{
public:
    explicit CollectionStore(storage::Database &db)
        : m_db(db)
    {}

    // Applies a scan in one transaction: adds or replaces `added` and `changed` (by path), removes `removed`.
    [[nodiscard]] bool apply(const QList<collection::ScannedFile> &added, const QList<collection::ScannedFile> &changed,
                             const QStringList &removed);
    // Every file's signature, keyed by path: the `known` of the next scan.
    QHash<QString, collection::FileSignature> signatures();
    // The signatures of the files under `folder` (and below).
    QHash<QString, collection::FileSignature> signaturesUnder(const QString &folder);
    QList<CollectionFile> all();
    int count();
    // Forgets every file outside `roots` (folders removed from Settings). Returns how many went, or -1 on failure.
    int removeOutside(const QStringList &roots);

private:
    storage::Database &m_db;
};

} // namespace selecta::core
