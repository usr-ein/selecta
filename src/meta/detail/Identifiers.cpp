// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/Identifiers.h"

#include <QDate>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail {

QString normalizedIsrc(QStringView text)
{
    if (text.size() > 32) {
        return {};
    }
    QString isrc;
    isrc.reserve(12);
    for (const QChar c : text) {
        if (c == u'-' || c == u' ') {
            continue;
        }
        isrc += c.toUpper();
    }
    static const QRegularExpression kIsrc(u"^[A-Z]{2}[A-Z0-9]{3}[0-9]{7}$"_s);
    return kIsrc.match(isrc).hasMatch() ? isrc : QString();
}

bool isValidMbid(QStringView text)
{
    static const QRegularExpression kUuid(u"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"_s);
    return kUuid.matchView(text).hasMatch();
}

QString normalizedBarcode(QStringView text)
{
    static const QRegularExpression kBarcode(u"^[0-9]{8,14}$"_s);
    const QStringView trimmed = text.trimmed();
    return kBarcode.matchView(trimmed).hasMatch() ? trimmed.toString() : QString();
}

std::pair<QString, DatePrecision> releaseDate(const QString &text, const QString &precisionHint)
{
    static const QRegularExpression kDate(u"^(\\d{4})(?:-(\\d{2})(?:-(\\d{2}))?)?"_s);
    const QRegularExpressionMatch match = kDate.match(text.trimmed());
    if (!match.hasMatch()) {
        return {QString(), DatePrecision::Unknown};
    }
    const int year = match.captured(1).toInt();
    if (year < 1000 || year > 2999) {
        return {QString(), DatePrecision::Unknown}; // "0000" and other placeholders
    }
    const int month = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
    const int day = match.captured(3).isEmpty() ? 0 : match.captured(3).toInt();
    DatePrecision precision = DatePrecision::Year;
    if (month >= 1 && month <= 12) {
        precision = DatePrecision::Month;
        if (day >= 1 && QDate(year, month, day).isValid()) {
            precision = DatePrecision::Day;
        }
    }
    // A coarser hint wins ("2017-01-01" with precision "year" is just 2017).
    if (precisionHint == u"year"_s) {
        precision = DatePrecision::Year;
    } else if (precisionHint == u"month"_s && precision == DatePrecision::Day) {
        precision = DatePrecision::Month;
    }
    switch (precision) {
    case DatePrecision::Year:
        return {u"%1"_s.arg(year, 4, 10, u'0'), precision};
    case DatePrecision::Month:
        return {u"%1-%2"_s.arg(year, 4, 10, u'0').arg(month, 2, 10, u'0'), precision};
    case DatePrecision::Day:
        return {u"%1-%2-%3"_s.arg(year, 4, 10, u'0').arg(month, 2, 10, u'0').arg(day, 2, 10, u'0'), precision};
    case DatePrecision::Unknown:
        break;
    }
    return {QString(), DatePrecision::Unknown};
}

} // namespace selecta::meta::detail
