// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/Json.h"

#include <QJsonDocument>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;
using namespace selecta::meta;
namespace json = selecta::meta::detail::json;

namespace {

QJsonObject object(const QByteArray &text)
{
    return QJsonDocument::fromJson(text).object();
}

} // namespace

TEST(Json, ParseObjectRejectsMalformedAndNonObjects)
{
    EXPECT_FALSE(json::parseObject("{", Service::Deezer).ok());
    EXPECT_FALSE(json::parseObject("", Service::Deezer).ok());
    EXPECT_FALSE(json::parseObject("[1,2]", Service::Deezer).ok());
    EXPECT_FALSE(json::parseObject("\"text\"", Service::Deezer).ok());
    EXPECT_EQ(json::parseObject("{", Service::Deezer).error().code, ErrorCode::BadResponse);
    EXPECT_TRUE(json::parseObject("{\"a\":1}", Service::Deezer).ok());
}

TEST(Json, TextIsTypeCheckedCleanedAndBounded)
{
    const QJsonObject o = object(R"({"s":"  a \t b\u0000c\u202e  ", "n": 5, "long": "xxxxxxxxxx"})");
    EXPECT_EQ(json::text(o, "s"_L1), u"a bc"_s);  // whitespace collapsed, NUL and RLO removed
    EXPECT_TRUE(json::text(o, "n"_L1).isEmpty()); // not a string
    EXPECT_TRUE(json::text(o, "missing"_L1).isEmpty());
    EXPECT_EQ(json::text(o, "long"_L1, 4), u"xxxx"_s);
}

TEST(Json, CleanTextNeverCutsASurrogatePair)
{
    const QString emoji = QString::fromUtf8("ab\xF0\x9F\x8E\xB5"); // "ab🎵": the note is two UTF-16 units
    EXPECT_EQ(json::cleanText(emoji, 3), u"ab"_s);
    EXPECT_EQ(json::cleanText(emoji, 4), emoji);
}

TEST(Json, IntegersMustBeWholeAndInRange)
{
    const QJsonObject o = object(R"({"a": 3, "b": 3.5, "c": "3", "d": 1e300, "e": -7, "f": 9007199254740993})");
    EXPECT_EQ(json::integer(o, "a"_L1), 3);
    EXPECT_FALSE(json::integer(o, "b"_L1).has_value());
    EXPECT_FALSE(json::integer(o, "c"_L1).has_value());
    EXPECT_FALSE(json::integer(o, "d"_L1).has_value());
    EXPECT_EQ(json::integer(o, "e"_L1), -7);
    EXPECT_FALSE(json::integer(o, "f"_L1).has_value()); // beyond 2^53: not exact
}

TEST(Json, PositiveIntRejectsZeroNegativeAndHuge)
{
    const QJsonObject o = object(R"({"a": 1, "z": 0, "n": -1, "h": 3000000000})");
    EXPECT_EQ(json::positiveInt(o, "a"_L1), 1);
    EXPECT_FALSE(json::positiveInt(o, "z"_L1).has_value());
    EXPECT_FALSE(json::positiveInt(o, "n"_L1).has_value());
    EXPECT_FALSE(json::positiveInt(o, "h"_L1).has_value());
}

TEST(Json, BooleansAndNumbers)
{
    const QJsonObject o = object(R"({"t": true, "x": 1, "d": 126.05})");
    EXPECT_EQ(json::boolean(o, "t"_L1), true);
    EXPECT_FALSE(json::boolean(o, "x"_L1).has_value());
    EXPECT_DOUBLE_EQ(json::number(o, "d"_L1).value_or(0), 126.05);
    EXPECT_FALSE(json::number(o, "t"_L1).has_value());
}
