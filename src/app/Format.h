// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <optional>

namespace selecta::core {
struct FileChoice;
}
namespace selecta::meta {
struct Release;
}

// Small text formatters shared by the views. Pure; main thread or any.
namespace selecta::app::format {

// "6:12", "1:02:03"; an empty string for an unknown length.
QString length(std::optional<qint64> milliseconds);
// A playhead time: "1:23", or with decimals = 1 "1:23.4". Negative times read as 0.
QString clock(double seconds, int decimals = 0);
// "MP3 320", "MP3 V0", "FLAC 16/44.1", "WAV 24/48".
QString quality(const core::FileChoice &file);
// "Neon Rain · Okuma Drift · 6:12": a release on one line.
QString release(const meta::Release &release);
// "+2 s", "−5 s": a file's length against the release.
QString lengthDelta(qint64 deltaSeconds);
// "9.4 MB", "820 KB", "1.2 GB" (decimal units, as the Finder shows them).
QString size(qint64 bytes);
// "1.2 MB/s", "340 KB/s"; an empty string for an unknown speed.
QString speed(qint64 bytesPerSecond);
// "just now", "12 min ago", "3 h ago", "2 days ago": how long ago `thenMs` was, at `nowMs` (Unix time, ms).
QString ago(qint64 thenMs, qint64 nowMs);
// "under a minute", "about 6 min", "about 1 h 20 min": a time left, rounded up to the minute.
QString about(qint64 milliseconds);

// Text from the network or from files (a peer, a file, a room, a message) can be anything, "<b>x</b>&y" included.
// A tooltip of such text: shown as it is. Qt shows a tooltip that looks like markup as rich text, so one that does is
// escaped and set in a paragraph that keeps its lines and doesn't wrap, as a plain tooltip shows; any other is kept.
QString plainToolTip(const QString &text);
// The words of a button, a menu item or an action made of such text: an "&" in it stays an "&", not a shortcut.
QString noMnemonic(const QString &text);

} // namespace selecta::app::format
