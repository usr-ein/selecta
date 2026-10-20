// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QTemporaryDir>

// A temporary folder tree of synthetic audio for one test (tests never touch anything outside it).
namespace selecta::collection::test {

using Tags = QMap<QString, QString>; // canonical key ("ARTIST", "TITLE", "TRACKNUMBER", …) → value

enum class Format { Mp3, Mp3Vbr, Mp3NoLameTag, Flac16, Flac24, Wav, Aiff, M4aAac, M4aAacFastStart, M4aAlac, OggFlac };

// Synthetic audio of `seconds` (a tone), cached per format and length, so trees build fast.
QByteArray audioBytes(Format format, double seconds);

class TestTree
{
public:
    TestTree();

    bool isValid() const { return m_dir.isValid(); }
    QString root() const { return m_dir.path(); }
    QString path(const QString &relative) const { return m_dir.filePath(relative); }

    // Writes synthetic audio at `relative` (folders created), then its tags through convert::writeTags (none when
    // empty). Returns the absolute path; empty (and a failed test expectation) on failure.
    QString add(const QString &relative, Format format = Format::Mp3, const Tags &tags = {},
                double seconds = 2.0) const;
    // Writes raw bytes at `relative`.
    QString addBytes(const QString &relative, const QByteArray &bytes) const;
    // Creates the folder `relative` (and its parents).
    QString addFolder(const QString &relative) const;
    // Sets a file's modification time.
    static bool setModified(const QString &path, qint64 msSinceEpoch);

private:
    QTemporaryDir m_dir;
};

// The signatures of a scan's files, keyed by path: `known` for the next scan.
QHash<QString, FileSignature> signaturesOf(const ScanResult &result);

// The scanned file with this path, or nullptr.
const ScannedFile *findFile(const QList<ScannedFile> &files, const QString &path);

} // namespace selecta::collection::test
