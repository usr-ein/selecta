// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace selecta::slsk {

// Registers the metatypes of every value type slsk signals carry (messages, infos, errors, QTcpSocket*), so they work
// in queued connections across threads and in QSignalSpy. The slsk QObjects call it from their constructors; calling
// it again is cheap. Thread-safe.
void registerMetaTypes();

} // namespace selecta::slsk
