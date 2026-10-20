// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/StreamInfo.h"
#include "convert/MetadataModel.h"
#include "convert/TagReader.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <taglib/taglib.h>
#include <taglib/tbytevector.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

// TagLib ≥ 2.3.2: its changelog lists hardening against crafted input across formats, and every tag Selecta reads
// comes from a stranger (ADR-003, conversion §9.4).
static_assert(TAGLIB_MAJOR_VERSION > 2 ||
                  (TAGLIB_MAJOR_VERSION == 2 &&
                   (TAGLIB_MINOR_VERSION > 3 || (TAGLIB_MINOR_VERSION == 3 && TAGLIB_PATCH_VERSION >= 2))),
              "Selecta needs TagLib 2.3.2 or later");

namespace selecta::convert::detail {

inline TagLib::String toTagLib(const QString &text)
{
    return {text.toStdString(), TagLib::String::UTF8};
}

inline QString fromTagLib(const TagLib::String &text)
{
    return QString::fromStdString(text.to8Bit(true));
}

inline TagLib::StringList toTagLib(const QStringList &values)
{
    TagLib::StringList list;
    for (const QString &v : values) {
        list.append(toTagLib(v));
    }
    return list;
}

inline QStringList fromTagLib(const TagLib::StringList &values)
{
    QStringList list;
    for (const TagLib::String &v : values) {
        list.append(fromTagLib(v));
    }
    return list;
}

inline TagLib::ByteVector toTagLib(const QByteArray &bytes)
{
    return {bytes.constData(), static_cast<unsigned int>(bytes.size())};
}

inline QByteArray fromTagLib(const TagLib::ByteVector &bytes)
{
    return {bytes.data(), static_cast<qsizetype>(bytes.size())};
}

// The container of a file on disk, detected from its content (Selecta's own probe), or Unknown.
audio::Container detectContainer(const QString &path);

// readTags()'s conversion of a TagLib property map: canonical keys first, aliases only for fields still missing,
// totals folded into "n/N", at most 512 fields, values bounded by `options`, every field marked `provenance`.
// `origin` names the file in log messages.
MetadataModel modelFromProperties(const TagLib::PropertyMap &properties, const TagReadOptions &options,
                                  Provenance provenance, const QString &origin);

} // namespace selecta::convert::detail
