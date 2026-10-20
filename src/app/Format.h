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
// "MP3 320", "MP3 V0", "FLAC 16/44.1", "WAV 24/48".
QString quality(const core::FileChoice &file);
// "Neon Rain · Okuma Drift · 6:12": a release on one line.
QString release(const meta::Release &release);
// "+2 s", "−5 s": a file's length against the release.
QString lengthDelta(qint64 deltaSeconds);

} // namespace selecta::app::format
