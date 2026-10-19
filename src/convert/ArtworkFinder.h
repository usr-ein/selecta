// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QSize>
#include <QString>

#include <optional>

namespace selecta::convert {

// An image file found next to an audio file (CONV-5). The caller lists the folder; this code never touches the disk.
struct CoverCandidate
{
    QString relativePath; // e.g. "cover.jpg" or "Scans/Front.png", '/'-separated, relative to the album folder
    QSize size;           // pixel size when known (inspectImage()), else invalid
};

struct CoverChoice
{
    int index = -1;       // into the candidate list
    bool guessed = false; // chosen by elimination rather than by name: show it for confirmation (CONV-5)
};

// Picks the folder cover (conversion §5.1): album folder before a Scans/, Artwork/ or Covers/ subfolder; exact names
// (cover, folder, front) first; then names containing a whole word front, cover, folder or albumart, excluding back,
// cd, disc, inlay, inside, tray, booklet and label; otherwise the only image, or the largest roughly square one
// (aspect 0.8–1.25), marked guessed. nullopt when nothing qualifies. Embedded art always wins over this (the caller's
// rule). Pure function.
std::optional<CoverChoice> chooseFolderCover(const QList<CoverCandidate> &candidates);

} // namespace selecta::convert
