// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "share/ShareIndex.h"
#include "share/detail/IndexData.h"
#include "share/detail/Walk.h"

#include <QList>

#include <memory>

// Internal: a finished walk → the arrays of a ShareIndex (word index, lookups, the precompressed share list).
namespace selecta::share::detail {

class IndexBuilder
{
public:
    // The index of `walk`, or null when `cancel` was raised meanwhile. Folders whose subtree holds no file are left
    // out. Threading: reentrant.
    static std::shared_ptr<const ShareIndex> build(Walk walk, const QList<ShareRoot> &roots,
                                                   const audio::CancellationFlag *cancel);
    // An index that shares nothing, with the empty list's payload.
    static std::shared_ptr<const ShareIndex> empty(const QList<ShareRoot> &roots);
    static std::unique_ptr<const IndexData> emptyData(const QList<ShareRoot> &roots);

private:
    static std::shared_ptr<const ShareIndex> wrap(std::unique_ptr<const IndexData> data);
};

} // namespace selecta::share::detail
