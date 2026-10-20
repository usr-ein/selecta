// SPDX-License-Identifier: GPL-3.0-or-later
#include "match/Types.h"

#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace selecta::match {

namespace {

constexpr std::array<std::pair<VersionClass, QStringView>, 13> kClassNames{{
    {VersionClass::Unspecified, u"unspecified"},
    {VersionClass::Original, u"original"},
    {VersionClass::Extended, u"extended"},
    {VersionClass::Radio, u"radio"},
    {VersionClass::Remix, u"remix"},
    {VersionClass::Vip, u"vip"},
    {VersionClass::Dub, u"dub"},
    {VersionClass::Instrumental, u"instrumental"},
    {VersionClass::Vocal, u"vocal"},
    {VersionClass::Acapella, u"acapella"},
    {VersionClass::Live, u"live"},
    {VersionClass::Acoustic, u"acoustic"},
    {VersionClass::Demo, u"demo"},
}};

} // namespace

QString tierName(Tier tier)
{
    switch (tier) {
    case Tier::None:
        return u"none"_s;
    case Tier::Likely:
        return u"likely"_s;
    case Tier::Exact:
        return u"exact"_s;
    }
    return u"none"_s;
}

QString relationName(Relation relation)
{
    switch (relation) {
    case Relation::None:
        return u"none"_s;
    case Relation::Same:
        return u"same"_s;
    case Relation::Upgrade:
        return u"upgrade"_s;
    case Relation::OtherVersion:
        return u"other version"_s;
    }
    return u"none"_s;
}

QString sourceName(Source source)
{
    switch (source) {
    case Source::Collection:
        return u"collection"_s;
    case Source::Inbox:
        return u"inbox"_s;
    case Source::History:
        return u"history"_s;
    }
    return u"collection"_s;
}

QString versionClassName(VersionClass versionClass)
{
    for (const auto &[value, name] : kClassNames) {
        if (value == versionClass) {
            return name.toString();
        }
    }
    return u"unspecified"_s;
}

std::optional<VersionClass> versionClassFromName(QStringView name)
{
    for (const auto &[value, known] : kClassNames) {
        if (known == name) {
            return value;
        }
    }
    return std::nullopt;
}

QString compatibilityName(VersionCompatibility compatibility)
{
    switch (compatibility) {
    case VersionCompatibility::Same:
        return u"same"_s;
    case VersionCompatibility::ProbablySame:
        return u"probably same"_s;
    case VersionCompatibility::Unsure:
        return u"unsure"_s;
    case VersionCompatibility::OtherVersion:
        return u"other version"_s;
    }
    return u"unsure"_s;
}

} // namespace selecta::match
