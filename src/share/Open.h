// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "share/ShareIndex.h"

#include <QIODevice>
#include <QString>

#include <memory>

namespace selecta::share {

// A shared file opened for an upload, or why it couldn't be.
struct Opened
{
    // Read-only, seekable, positioned at 0; its size() is the indexed size. Null on failure.
    std::unique_ptr<QIODevice> device;
    // On failure, the exact denial for the peer (UploadDenied or a refused TransferRequest): "File not shared." or
    // "File read error.". Empty on success.
    QString error;
    // On failure, what happened, for the local log only: never send it to a peer.
    QString detail;

    bool ok() const { return device != nullptr; }
};

// Opens `file` (from ShareIndex::lookup()) for an upload: open(2) with O_NOFOLLOW (and O_NONBLOCK, so a FIFO swapped
// in can't block), then fstat(2) on what was opened: it must be a regular file with the device, inode and size the
// index recorded, and its data must be on this Mac (not an iCloud placeholder). Nothing is read: the first read is the
// transfer's.
//
// Errors:
// - "File not shared.": the file is gone, was replaced (another inode, a symlink, not a regular file) or its size
//   changed since it was indexed. The index is stale for this file; a rebuild will tell.
// - "File read error.": it is the indexed file but can't be read now (permissions, an I/O error, no file descriptors
//   left, its data only in iCloud).
//
// Threading: reentrant; call it from any thread. The device, like any QIODevice, is used from one thread at a time.
// Ownership: the caller owns the device (slsk::TransferManager borrows it until the upload's terminal state). Blocking:
// as long as open(2) takes on the file's volume, which is immediate on a local disk; never reads.
Opened open(const SharedFile &file);

} // namespace selecta::share
