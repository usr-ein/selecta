// SPDX-License-Identifier: GPL-3.0-or-later
#include "share/detail/Attributes.h"

#include "audio/Decoder.h"
#include "collection/detail/FileReader.h"

#include <cmath>
#include <limits>
#include <memory>

#ifdef Q_OS_DARWIN
#include <sys/resource.h>
#endif

using namespace Qt::StringLiterals;

namespace selecta::share::detail {

namespace {

quint32 positive32(qint64 value)
{
    return value > 0 && value <= std::numeric_limits<quint32>::max() ? static_cast<quint32>(value) : 0;
}

} // namespace

slsk::FileAttributes Attributes::toWire() const
{
    slsk::FileAttributes wire;
    if (bitrate != 0) {
        wire.bitrate = bitrate;
    }
    if (duration != 0) {
        wire.duration = duration;
    }
    if (vbr != 0) {
        wire.vbr = vbr == 2;
    }
    if (sampleRate != 0) {
        wire.sampleRate = sampleRate;
    }
    if (bitDepth != 0) {
        wire.bitDepth = bitDepth;
    }
    return wire;
}

Attributes attributesOf(const match::AudioQuality &quality, qint64 durationMs)
{
    Attributes attributes;
    attributes.duration = positive32(durationMs / 1000);
    if (quality.bitDepth > 0) {
        attributes.sampleRate = positive32(quality.sampleRateHz);
        attributes.bitDepth = quality.bitDepth <= std::numeric_limits<quint8>::max()
                                  ? static_cast<quint8>(quality.bitDepth)
                                  : quint8{0};
    } else {
        attributes.bitrate = positive32(quality.bitrateKbps);
        if (quality.vbr) {
            attributes.vbr = *quality.vbr ? 2 : 1;
        }
    }
    return attributes;
}

AttributeSource::AttributeSource(const QHash<QString, collection::ScannedFile> &knownAudio)
    : m_known(knownAudio)
{}

const collection::ScannedFile *AttributeSource::known(const QString &keyPath)
{
    if (const auto it = m_known.constFind(keyPath); it != m_known.cend()) {
        return &it.value();
    }
    if (m_known.isEmpty()) {
        return nullptr;
    }
    // Another spelling of the same path (NFD, a trailing slash in a root): compare as the collection does.
    if (!m_indexed) {
        m_indexed = true;
        m_byComparisonKey.reserve(m_known.size());
        for (auto it = m_known.cbegin(); it != m_known.cend(); ++it) {
            m_byComparisonKey.insert(collection::comparisonKey(it.key()), &it.value());
        }
    }
    return m_byComparisonKey.value(collection::comparisonKey(keyPath), nullptr);
}

Attributes AttributeSource::audio(const QString &keyPath, const QByteArray &nativePath, const FileIdentity &identity,
                                  qint64 modifiedMs)
{
    if (const collection::ScannedFile *file = known(keyPath)) {
        // A file changed since the collection read it has other facts: an entry is only used while it is current.
        if (file->file.size >= 0 && static_cast<quint64>(file->file.size) == identity.size &&
            file->file.modifiedMs == modifiedMs) {
            return attributesOf(file->quality, file->durationMs);
        }
    }
    return probe(nativePath, identity);
}

Attributes probe(const QByteArray &nativePath, const FileIdentity &identity)
{
    const CheckedDescriptor descriptor = openChecked(nativePath, identity);
    if (descriptor.fd < 0) {
        return {};
    }
    auto source = std::make_shared<DescriptorSource>(descriptor.fd, static_cast<qint64>(identity.size),
                                                     QString::fromUtf8(nativePath));
    const audio::Result<audio::StreamInfo> stream = audio::probeStreamInfo(std::move(source));
    if (!stream.ok()) {
        return {};
    }
    const audio::StreamInfo &info = stream.value();
    const double seconds = info.durationSeconds();
    return attributesOf(collection::detail::qualityOf(info), seconds > 0 ? std::llround(seconds * 1000.0) : 0);
}

MaterializationOff::MaterializationOff()
{
#ifdef Q_OS_DARWIN
    m_previous = ::getiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD);
    if (m_previous >= 0) {
        (void)::setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD,
                               IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
    }
#endif
}

MaterializationOff::~MaterializationOff()
{
#ifdef Q_OS_DARWIN
    if (m_previous >= 0) {
        (void)::setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, m_previous);
    }
#endif
}

} // namespace selecta::share::detail
