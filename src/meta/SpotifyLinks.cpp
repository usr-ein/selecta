// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/SpotifyLinks.h"

#include <QSet>
#include <QStringList>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace selecta::meta::spotify {

namespace {

std::optional<LinkKind> kindFromName(QStringView name)
{
    if (name.compare(u"track"_s, Qt::CaseInsensitive) == 0) {
        return LinkKind::Track;
    }
    if (name.compare(u"album"_s, Qt::CaseInsensitive) == 0) {
        return LinkKind::Album;
    }
    if (name.compare(u"playlist"_s, Qt::CaseInsensitive) == 0) {
        return LinkKind::Playlist;
    }
    return std::nullopt;
}

QString kindName(LinkKind kind)
{
    switch (kind) {
    case LinkKind::Track:
        return u"track"_s;
    case LinkKind::Album:
        return u"album"_s;
    case LinkKind::Playlist:
        return u"playlist"_s;
    }
    return {};
}

// "spotify:track:<id>", also the old "spotify:user:<name>:playlist:<id>".
std::optional<Link> parseUri(QStringView text)
{
    const QList<QStringView> parts = text.split(u':');
    if (parts.size() < 3 || parts.first().compare(u"spotify"_s, Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    const QStringView id = parts.last();
    const std::optional<LinkKind> kind = kindFromName(parts.at(parts.size() - 2));
    const bool shapeOk = parts.size() == 3 || (parts.size() == 5 && parts.at(1) == u"user"_s);
    if (!shapeOk || !kind.has_value() || !isValidId(id)) {
        return std::nullopt;
    }
    return Link{*kind, id.toString()};
}

// https://open.spotify.com/[intl-xx/][embed/]<kind>/<id>[?…], also /user/<name>/playlist/<id>.
std::optional<Link> parseUrl(QStringView text)
{
    const QUrl url(text.toString(), QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != u"https"_s && url.scheme() != u"http"_s)) {
        return std::nullopt;
    }
    const QString host = url.host().toLower();
    if (host != u"open.spotify.com"_s && host != u"play.spotify.com"_s) {
        return std::nullopt;
    }
    QStringList segments = url.path().split(u'/', Qt::SkipEmptyParts);
    if (!segments.isEmpty() && segments.first().startsWith(u"intl-"_s, Qt::CaseInsensitive)) {
        segments.removeFirst();
    }
    if (!segments.isEmpty() && segments.first() == u"embed"_s) {
        segments.removeFirst();
    }
    if (segments.size() == 4 && segments.first() == u"user"_s) {
        segments = segments.mid(2);
    }
    if (segments.size() != 2) {
        return std::nullopt;
    }
    const std::optional<LinkKind> kind = kindFromName(segments.first());
    if (!kind.has_value() || !isValidId(segments.last())) {
        return std::nullopt;
    }
    return Link{*kind, segments.last()};
}

QStringView stripWrapping(QStringView token)
{
    static constexpr QStringView kWrapping = u"<>()[]{}\"',;";
    while (!token.isEmpty() && kWrapping.contains(token.front())) {
        token = token.mid(1);
    }
    while (!token.isEmpty() && kWrapping.contains(token.back())) {
        token.chop(1);
    }
    return token;
}

} // namespace

bool isValidId(QStringView id)
{
    return id.size() == kIdLength && std::ranges::all_of(id, [](QChar c) {
               return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
           });
}

std::optional<Link> parseLink(QStringView text)
{
    const QStringView trimmed = stripWrapping(text.trimmed());
    if (trimmed.isEmpty() || trimmed.size() > 2048) {
        return std::nullopt;
    }
    if (trimmed.startsWith(u"spotify:"_s, Qt::CaseInsensitive)) {
        return parseUri(trimmed);
    }
    return parseUrl(trimmed);
}

QString webUrl(const Link &link)
{
    return u"https://open.spotify.com/%1/%2"_s.arg(kindName(link.kind), link.id);
}

PastedLinks parsePastedLinks(QStringView text)
{
    PastedLinks result;
    QSet<QString> seen;
    int lineNumber = 0;
    for (const QStringView line : text.tokenize(u'\n')) {
        const QStringView trimmedLine = line.trimmed();
        if (trimmedLine.isEmpty()) {
            continue;
        }
        if (++lineNumber > kMaxPastedLines) {
            ++result.skipped;
            continue;
        }
        bool lineHadLink = false;
        const QList<QStringView> tokens = trimmedLine.split(u' ', Qt::SkipEmptyParts);
        for (const QStringView token : tokens) {
            const QList<QStringView> pieces = token.split(u'\t', Qt::SkipEmptyParts);
            for (const QStringView piece : pieces) {
                const std::optional<Link> link = parseLink(piece);
                if (!link.has_value()) {
                    continue;
                }
                lineHadLink = true;
                const QString key = kindName(link->kind) + u':' + link->id;
                if (seen.contains(key)) {
                    ++result.duplicates;
                    continue;
                }
                seen.insert(key);
                result.links.append(*link);
            }
        }
        if (!lineHadLink) {
            ++result.skipped;
        }
    }
    return result;
}

} // namespace selecta::meta::spotify
