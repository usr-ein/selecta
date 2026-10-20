// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "meta/Result.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QString>

#include <optional>

// Strict, defensive reading of service JSON. Every field is optional (Spotify removed fields without versioning in
// 2026), every value is type-checked, and text is bounded and stripped of control characters before it reaches a
// Release. Pure functions, thread-safe.
namespace selecta::meta::detail::json {

inline constexpr qsizetype kMaxText = 1000;     // characters kept from any text field
inline constexpr qsizetype kMaxListItems = 100; // artists, images, genres… read from one array

// The body as a JSON object, or BadResponse.
Result<QJsonObject> parseObject(const QByteArray &body, Service service);

// Text with control characters removed, whitespace runs collapsed to one space, trimmed, cut to `maxLength`.
QString cleanText(const QString &text, qsizetype maxLength = kMaxText);

QString text(const QJsonObject &object, QLatin1StringView key, qsizetype maxLength = kMaxText);
// A whole number (JSON numbers with no fractional part, below 2^53 in magnitude), else nullopt.
std::optional<qint64> integer(const QJsonObject &object, QLatin1StringView key);
std::optional<double> number(const QJsonObject &object, QLatin1StringView key);
std::optional<bool> boolean(const QJsonObject &object, QLatin1StringView key);
QJsonObject object(const QJsonObject &object, QLatin1StringView key);
QJsonArray array(const QJsonObject &object, QLatin1StringView key);

// A positive int (track numbers, totals), else nullopt.
std::optional<int> positiveInt(const QJsonObject &object, QLatin1StringView key);

} // namespace selecta::meta::detail::json
