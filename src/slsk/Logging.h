// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLoggingCategory>

// Logging categories of the protocol library. Dropped and malformed messages are logged here, never silently ignored.
Q_DECLARE_LOGGING_CATEGORY(lcSlskServer)   // selecta.slsk.server: the server session
Q_DECLARE_LOGGING_CATEGORY(lcSlskPeer)     // selecta.slsk.peer: peer connections and messages
Q_DECLARE_LOGGING_CATEGORY(lcSlskTransfer) // selecta.slsk.transfer: downloads and uploads
Q_DECLARE_LOGGING_CATEGORY(lcSlskDistrib)  // selecta.slsk.distrib: the distributed network
