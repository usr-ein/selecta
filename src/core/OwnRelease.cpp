// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/OwnRelease.h"

#include <QPointer>

namespace selecta::core {

void lookUpOwnRelease(QObject *context, const ResolveFunction &resolve, const SearchResult &result,
                      std::function<void(const std::optional<meta::Release> &release)> then)
{
    if (!resolve || (result.parsed.title.value.isEmpty() && result.parsed.artist.value.isEmpty())) {
        then(std::nullopt);
        return;
    }
    meta::ResolveQuery query;
    query.artist = result.parsed.artist.value;
    query.title = result.parsed.title.value;
    if (result.durationSec() > 0) {
        query.durationMs = qint64{result.durationSec()} * 1000;
    }
    resolve(query).then(context, [guard = QPointer<QObject>(context),
                                  then = std::move(then)](const meta::Result<meta::Resolution> &resolution) {
        if (!guard) {
            return;
        }
        std::optional<meta::Release> release;
        if (resolution.ok() && !resolution.value().candidates.isEmpty() &&
            resolution.value().confidence >= meta::Confidence::Likely) {
            release = resolution.value().candidates.constFirst().release;
        }
        then(release);
    });
}

} // namespace selecta::core
