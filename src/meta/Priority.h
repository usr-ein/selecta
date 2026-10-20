// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtTypes>

namespace selecta::meta {

// Which lane a request waits in at its service's pacing gate. Interactive requests (a preview the user asked for, a
// search typed by hand, the inspector) start before any queued background request (Get tracks batches, pasted-link
// imports, the arrival pipeline's enrichment), so a 100-line batch never starves a preview. Within a lane, first come
// first served; neither lane changes the service's limits.
enum class Priority : quint8 {
    Interactive,
    Background,
};

} // namespace selecta::meta
