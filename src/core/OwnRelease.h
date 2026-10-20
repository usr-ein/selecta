// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/SearchResults.h"
#include "meta/ReleaseResolver.h"

#include <QFuture>
#include <QObject>

#include <functional>
#include <optional>

namespace selecta::core {

// Looks a release up (the resolver in the app; Background or Interactive priority is the caller's choice).
using ResolveFunction = std::function<QFuture<meta::Result<meta::Resolution>>(const meta::ResolveQuery &)>;

// The release of a search result's own track (D29: every download is tagged from its release): `then` gets it, or
// nothing when the result names no track, there is no resolver, or the lookup isn't sure enough. `then` runs on
// `context`'s thread, not at all when `context` is gone first; at once when there is nothing to look up.
void lookUpOwnRelease(QObject *context, const ResolveFunction &resolve, const SearchResult &result,
                      std::function<void(const std::optional<meta::Release> &release)> then);

} // namespace selecta::core
