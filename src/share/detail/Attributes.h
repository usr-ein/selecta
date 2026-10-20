// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "collection/Scanner.h"
#include "match/Types.h"
#include "share/detail/Descriptor.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QHash>
#include <QString>

// Internal: the audio attributes build() shares (sharing note §3.4). Not thread-safe per object; one per build().
namespace selecta::share::detail {

// A file's attributes as Nicotine+ packs them: lossless files (a known bit depth) send duration, sample rate and bit
// depth; lossy ones bitrate, duration and the VBR flag when known. 0 means absent: values outside 0 < v < 2^32 are
// never sent.
struct Attributes
{
    quint32 bitrate = 0;    // kbps
    quint32 duration = 0;   // whole seconds
    quint32 sampleRate = 0; // Hz
    quint8 bitDepth = 0;    // bits
    quint8 vbr = 0;         // 0 unknown, 1 constant, 2 variable

    slsk::FileAttributes toWire() const;

    friend bool operator==(const Attributes &, const Attributes &) = default;
};

// From stream facts as the collection keeps them.
Attributes attributesOf(const match::AudioQuality &quality, qint64 durationMs);

// Where the build gets an audio file's attributes: the collection's entry when it is current, else a header probe.
class AttributeSource
{
public:
    explicit AttributeSource(const QHash<QString, collection::ScannedFile> &knownAudio);

    // `keyPath`: the file's path as collection::scan() reports it; `nativePath`: the bytes to open; `identity`: what
    // the walk saw, with `modifiedMs`. Never fails: a file nothing can be learnt about gets no attributes.
    Attributes audio(const QString &keyPath, const QByteArray &nativePath, const FileIdentity &identity,
                     qint64 modifiedMs);

private:
    const collection::ScannedFile *known(const QString &keyPath);

    const QHash<QString, collection::ScannedFile> &m_known;
    QHash<QString, const collection::ScannedFile *> m_byComparisonKey; // built on the first miss
    bool m_indexed = false;
};

// audio::probeStreamInfo() on the file, through openChecked(): nothing if it isn't the walked file any more.
Attributes probe(const QByteArray &nativePath, const FileIdentity &identity);

// While alive, reads on this thread never materialise iCloud placeholders (macOS: the thread's
// IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES policy is off; restored on destruction). Elsewhere it does nothing.
class MaterializationOff
{
public:
    MaterializationOff();
    ~MaterializationOff();
    MaterializationOff(const MaterializationOff &) = delete;
    MaterializationOff &operator=(const MaterializationOff &) = delete;
    MaterializationOff(MaterializationOff &&) = delete;
    MaterializationOff &operator=(MaterializationOff &&) = delete;

private:
    int m_previous = -1; // the thread's policy before, or -1 when it couldn't be read
};

} // namespace selecta::share::detail
