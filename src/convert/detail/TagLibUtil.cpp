// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert/detail/TagLibUtil.h"

#include "audio/ByteSource.h"
#include "audio/detail/ContainerProbe.h"

namespace selecta::convert::detail {

audio::Container detectContainer(const QString &path)
{
    Result<std::shared_ptr<audio::FileSource>> source = audio::FileSource::open(path);
    if (!source.ok()) {
        return audio::Container::Unknown;
    }
    audio::FileSource &file = *source.value();
    audio::detail::SourceReader reader(file, file.snapshot());
    audio::detail::ProbeResult probe;
    const audio::detail::ParseResult result = audio::detail::probeContainer(reader, qint64{64} * 1024 * 1024, probe);
    return result.ok() ? probe.container : audio::Container::Unknown;
}

} // namespace selecta::convert::detail
