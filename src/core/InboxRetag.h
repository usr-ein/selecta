// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/MetadataModel.h"
#include "library/FileOps.h"
#include "library/Naming.h"

#include <QString>
#include <QStringList>

namespace selecta::core {

// New tags for an Inbox single (PRD DL-4, META-4): edited by hand, or from another release.
struct RetagRequest
{
    QString path;      // the Inbox file
    QString inboxRoot; // the file must be inside it
    library::naming::Templates templates;
    library::naming::Cleaner cleaner;
    convert::MetadataModel changes; // written over the file's own tags; a front cover replaces the embedded one
    QStringList cleared;            // fields to remove
};

struct RetagResult
{
    bool ok = false;
    QString error;
    QString path;                // where the file is now: renamed after its new tags (DL-13)
    convert::MetadataModel tags; // as written
    qint64 sizeBytes = 0;
};

// Writes the new tags into a copy next to the file, checks the copy, swaps it in with one atomic rename (a crash
// leaves the old file or the new one, never half of one), then renames the file after its new artist and title when
// the naming template gives another name. A cover in `changes` is processed for CDJs first (JPEG, at most 800×800).
//
// Threading: blocking file work; call it from a worker. Failure modes: an unreadable or vanished file, a tag write or
// check that fails, a full disk: the Inbox file is left as it was, and `error` says why.
RetagResult retagInboxFile(const RetagRequest &request, const library::FileOps &fileOps);

} // namespace selecta::core
