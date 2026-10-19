// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtTypes>

// The local debugging protocol between a running Selecta (started with --debug-server, in builds with
// SELECTA_DEVTOOLS) and `selecta-cli app …`.
//
// Transport: a local socket (a Unix domain socket in the user's temporary folder, accessible to the same user only).
// Messages: one JSON object per line, UTF-8, at most kMaxLineBytes bytes.
//   request:  {"id": <int>, "cmd": "<command>", "args": {…}}
//   response: {"id": <int>, "ok": true, "result": …} or {"id": <int>, "ok": false, "error": "<message>"}
// Commands: ping, state, views, tree, screenshot, navigate, click, type, appearance, log, quit.
namespace selecta::base::debugprotocol {

inline constexpr char kSocketName[] = "selecta-debug";
inline constexpr qsizetype kMaxLineBytes = 64 * 1024;
inline constexpr int kTreeDefaultDepth = 8;
inline constexpr int kTreeMaxDepth = 32;
inline constexpr int kLogMaxLines = 2000;

} // namespace selecta::base::debugprotocol
