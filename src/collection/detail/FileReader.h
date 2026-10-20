// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/StreamInfo.h"
#include "collection/Scanner.h"

#include <QString>
#include <QStringList>

// Internal: reading one file for scan(): tags, stream facts from the header, names from the path when untagged.
namespace selecta::collection::detail {

struct ReadOutcome
{
    enum class Status {
        Read,       // `file` is filled in
        Unreadable, // `reason` says why (the stream couldn't be opened)
        Vanished,   // the file is gone since the walk
    };
    Status status = Status::Unreadable;
    ScannedFile file;
    QString reason;
};

// Reads the file of `signature`. `relativePath` is its path under the root ("Artist/Album/01 - Title.flac"), for the
// name parser, and `siblings` the names of the other audio files in its folder. Read-only: tags through
// convert::readTags (read-only TagLib stream), stream facts through audio::FileSource. Threading: reentrant.
ReadOutcome readFile(const FileSignature &signature, const QString &relativePath, const QStringList &siblings,
                     const convert::TagReadOptions &tags);

// match::AudioQuality from a decoder's stream facts (the VBR average from the LAME tag when present).
match::AudioQuality qualityOf(const audio::StreamInfo &info);

// The number in "3", "03", "3/12"; 0 when there is none.
int leadingNumber(QStringView text);
// The year of "2019", "2019-05-03", "2019/05"; 0 when there is none.
int yearOf(QStringView date);

} // namespace selecta::collection::detail
