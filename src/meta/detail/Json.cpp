// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/Json.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail::json {

Result<QJsonObject> parseObject(const QByteArray &body, Service service)
{
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) {
        return makeError(service, ErrorCode::BadResponse,
                         u"The answer isn't valid JSON (%1 at byte %2)."_s.arg(error.errorString()).arg(error.offset));
    }
    if (!document.isObject()) {
        return makeError(service, ErrorCode::BadResponse, u"The answer isn't a JSON object."_s);
    }
    return document.object();
}

QString cleanText(const QString &text, qsizetype maxLength)
{
    QString out;
    out.reserve(std::min(text.size(), maxLength));
    bool pendingSpace = false;
    for (const QChar c : text) {
        if (out.size() >= maxLength) {
            break;
        }
        const QChar::Category category = c.category();
        if (c.isSpace()) {
            pendingSpace = !out.isEmpty();
            continue;
        }
        if (category == QChar::Other_Control || category == QChar::Other_Format ||
            category == QChar::Other_NotAssigned) {
            continue;
        }
        if (pendingSpace) {
            out += u' ';
            pendingSpace = false;
        }
        out += c;
    }
    // Never leave half a surrogate pair at the cut.
    if (!out.isEmpty() && out.back().isHighSurrogate()) {
        out.chop(1);
    }
    return out;
}

QString text(const QJsonObject &object, QLatin1StringView key, qsizetype maxLength)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? cleanText(value.toString(), maxLength) : QString();
}

std::optional<qint64> integer(const QJsonObject &object, QLatin1StringView key)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double d = value.toDouble();
    constexpr double kLimit = 9007199254740992.0; // 2^53: every integer below is exact in a double
    if (!std::isfinite(d) || std::abs(d) >= kLimit || std::trunc(d) != d) {
        return std::nullopt;
    }
    return static_cast<qint64>(d);
}

std::optional<double> number(const QJsonObject &object, QLatin1StringView key)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble())) {
        return std::nullopt;
    }
    return value.toDouble();
}

std::optional<bool> boolean(const QJsonObject &object, QLatin1StringView key)
{
    const QJsonValue value = object.value(key);
    if (!value.isBool()) {
        return std::nullopt;
    }
    return value.toBool();
}

QJsonObject object(const QJsonObject &object, QLatin1StringView key)
{
    return object.value(key).toObject();
}

QJsonArray array(const QJsonObject &object, QLatin1StringView key)
{
    return object.value(key).toArray();
}

std::optional<int> positiveInt(const QJsonObject &object, QLatin1StringView key)
{
    const std::optional<qint64> value = integer(object, key);
    if (!value.has_value() || *value <= 0 || *value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

} // namespace selecta::meta::detail::json
