// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/detail/TitleInfo.h"

#include "match/VersionClassifier.h"

#include <QRegularExpression>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace selecta::meta::detail {

namespace {

// Derivative uploads a DJ never wants unless the line asks for them (the note §14.5, spotDL's forbidden words
// inverted: versions such as remix, extended, dub or edit are never junk).
constexpr std::array<QStringView, 9> kJunkWords = {u"slowed",  u"nightcore", u"8d",          u"reverb",  u"karaoke",
                                                   u"tribute", u"cover",     u"bassboosted", u"reverbed"};
constexpr std::array<QStringView, 5> kJunkPhrases = {u"sped up", u"speed up", u"bass boosted", u"in the style of",
                                                     u"8d audio"};

// Tokens of `all` that aren't in `core` (multiset difference, order kept).
QStringList remainder(const QStringList &all, const QStringList &core)
{
    QStringList rest = all;
    for (const QString &token : core) {
        rest.removeOne(token);
    }
    return rest;
}

} // namespace

TitleInfo analyseTitle(const QString &title, const QString &explicitVersion)
{
    TitleInfo info;
    const match::TitleKeys keys = match::Normalizer::titleKeys(title);
    info.core = keys.field;
    info.coreDisplay = match::VersionClassifier::decompose(title).core;
    info.version = keys.version;
    info.featured = keys.featured;
    if (!explicitVersion.trimmed().isEmpty()) {
        if (const std::optional<match::VersionInfo> version = match::VersionClassifier::classify(explicitVersion)) {
            info.version = *version;
        }
    }
    const QStringList rest = remainder(match::Normalizer::tokens(title), info.core.key.split(u' ', Qt::SkipEmptyParts));
    const QString restText = rest.join(u' ');
    for (const QStringView word : kJunkWords) {
        if (rest.contains(word)) {
            info.junk.append(word.toString());
        }
    }
    for (const QStringView phrase : kJunkPhrases) {
        if (QString(u' ' + restText + u' ').contains(QString(u' ' + phrase.toString() + u' '))) {
            info.junk.append(phrase.toString());
        }
    }
    info.djMix = info.version.mixed || restText.contains(u"mix cut"_s);
    return info;
}

QString primaryArtist(const QString &credit)
{
    static const QRegularExpression kSeparators(
        u"\\s+(?:&|and|x|vs\\.?|feat\\.?|ft\\.?|featuring|with|pres\\.?|presents)\\s+|\\s*[,;/]\\s*"_s,
        QRegularExpression::CaseInsensitiveOption);
    const QString first = credit.split(kSeparators, Qt::SkipEmptyParts).value(0).trimmed();
    return first.isEmpty() ? credit.trimmed() : first;
}

QStringList releaseArtistKeys(const Release &release, const TitleInfo &title)
{
    QStringList keys;
    for (const QString &artist : release.artists) {
        const QString key = match::Normalizer::normalize(artist, match::FieldKind::Artist);
        if (!key.isEmpty() && !keys.contains(key)) {
            keys.append(key);
        }
    }
    for (const QString &featured : title.featured) {
        if (!featured.isEmpty() && !keys.contains(featured)) {
            keys.append(featured);
        }
    }
    return keys;
}

QString versionWords(const match::VersionInfo &version)
{
    return version.isExplicit() ? version.raw : QString();
}

} // namespace selecta::meta::detail
