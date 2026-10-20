// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/DeezerClient.h"

#include <QJsonObject>
#include <QString>

#include <optional>

// Deezer "Simple API" JSON → meta's value types. Every field is optional and type-checked; numeric IDs become
// strings; preview URLs are kept only from Deezer's CDN over HTTPS. Pure functions, thread-safe.
namespace selecta::meta::detail::deezer {

inline constexpr int kMaxSearchHits = 50;

// A track object (search hit or full track). Nullopt when it has no usable ID or title.
std::optional<DeezerTrack> track(const QJsonObject &object);
// The tracks of a search answer ("data": […]), at most kMaxSearchHits.
QList<DeezerTrack> searchHits(const QJsonObject &object);
// An album object. Nullopt when it has no usable ID or title.
std::optional<DeezerAlbum> album(const QJsonObject &object);

// Deezer reports errors inside HTTP 200 answers: {"error": {"type", "message", "code"}}. The code, if any.
std::optional<int> errorCode(const QJsonObject &object);

inline constexpr int kQuotaError = 4; // too many requests: back off
inline constexpr int kServiceBusyError = 700;
inline constexpr int kNotFoundError = 800; // "no data"

} // namespace selecta::meta::detail::deezer
