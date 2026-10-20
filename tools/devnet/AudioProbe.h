// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "slsk/Types.h"

#include <QByteArrayView>
#include <QString>

namespace selecta::devnet {

// A shared file's Soulseek attributes, read from its header the way sharing clients do
// (references/soulseek-protocol.md §6.2): lossy files carry {bitrate, duration, VBR}, lossless ones {duration, sample
// rate, bit depth}, anything else nothing. Only MP3, FLAC, WAV and AIFF are understood; a file that doesn't parse
// gets no attributes, never an error.
//
// probeAudio() reads at most a few kilobytes past any ID3v2 tag. The others are pure, for tests: `head` starts at
// the file's first byte (MP3: at the first byte after any ID3v2 tag) and `fileSize` is what the file holds from there.
slsk::FileAttributes probeAudio(const QString &path);
slsk::FileAttributes probeMp3(QByteArrayView head, quint64 audioBytes);
slsk::FileAttributes probeFlac(QByteArrayView head);
slsk::FileAttributes probeWav(QByteArrayView head, quint64 fileSize);
slsk::FileAttributes probeAiff(QByteArrayView head);

// The size of an ID3v2 tag at the start of `head` (header and footer included), or 0.
quint64 id3v2Size(QByteArrayView head);

} // namespace selecta::devnet
