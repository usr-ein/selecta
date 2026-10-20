// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convert/Transcoder.h"

#include <QFile>
#include <QIODevice>

#include <functional>
#include <memory>

namespace selecta::convert::detail {

// Test seam: given the open temporary file, returns the device the output is written through (a test wraps the file
// in a device that fails like a full disk). Returning null keeps the file itself.
using DeviceWrapper = std::function<std::unique_ptr<QIODevice>(QFile &file)>;

// transcode() with the seam.
Result<TranscodeReport> transcode(const TranscodeRequest &request, const audio::CancellationFlag *cancel,
                                  const TranscodeProgress &progress, const DeviceWrapper &wrapDevice);

} // namespace selecta::convert::detail
