// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Release.h"

#include <QString>
#include <QStringView>

// Identifier and date helpers shared by the service clients. Pure functions, thread-safe.
namespace selecta::meta::detail {

// An ISRC in canonical form (12 characters, upper case, hyphens and spaces removed: "GB-CFB-17-00229" →
// "GBCFB1700229"), or empty when it isn't one (2 letters, 3 alphanumerics, 7 digits).
QString normalizedIsrc(QStringView text);

// True for a MusicBrainz identifier (a lower-case UUID).
bool isValidMbid(QStringView text);

// A UPC/EAN barcode: 8 to 14 digits, kept as written; empty otherwise.
QString normalizedBarcode(QStringView text);

// A release date and its precision from a service's text: "2017", "2017-06", "2017-06-09" (longer ISO timestamps
// are cut to the day). Unknown or placeholder dates ("0000", "", "1900-01-01"-style zeros) give {"", Unknown}.
// `precisionHint` ("year", "month", "day", as Spotify sends it) is used only when it agrees with the text.
std::pair<QString, DatePrecision> releaseDate(const QString &text, const QString &precisionHint = {});

} // namespace selecta::meta::detail
