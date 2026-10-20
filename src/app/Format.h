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

} // namespace selecta::app::format
