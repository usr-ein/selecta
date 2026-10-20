// SPDX-License-Identifier: GPL-3.0-or-later
#include "collection/Albums.h"

#include "match/Normalizer.h"
#include "match/RemotePathParser.h"

#include <QMap>
#include <QSet>

#include <algorithm>
#include <array>
#include <optional>

using namespace Qt::StringLiterals;

namespace selecta::collection {

namespace {

constexpr qsizetype kMinNamedTracks = 2; // a folder named like an album needs at least two tracks
constexpr quint64 kIdMask = 0x7FFF'FFFF'FFFF'FFFFULL;

QString parentOf(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    if (slash < 0) {
        return {};
    }
    return slash == 0 ? u"/"_s : path.left(slash);
}

QString nameOf(const QString &path)
{
    return path.mid(path.lastIndexOf(u'/') + 1);
}

// "CD1", "CD 2", "Disc 2", "Disk-3", "cd02", "Disc 1 - The Night": a disc of the album in the folder above.
bool isDiscFolderName(QStringView name)
{
    static constexpr std::array<QStringView, 4> kPrefixes{u"disc", u"disk", u"cd", u"dvd"};
    const QString lower = name.toString().trimmed().toLower();
    for (const QStringView prefix : kPrefixes) {
        if (!lower.startsWith(prefix)) {
            continue;
        }
        qsizetype i = prefix.size();
        while (i < lower.size() && (lower[i] == u' ' || lower[i] == u'.' || lower[i] == u'_' || lower[i] == u'-')) {
            ++i;
        }
        const qsizetype digits = i;
        while (i < lower.size() && lower[i].isDigit() && i - digits < 3) {
            ++i;
        }
        if (i == digits || i - digits > 2) {
            continue;
        }
        return i == lower.size() || !lower[i].isLetterOrNumber();
    }
    return false;
}

// A year in a folder name: "(2019)", "[2019]", " - 2019 - " or a trailing " 2019".
bool hasFolderYear(QStringView name)
{
    for (qsizetype i = 0; i + 4 <= name.size(); ++i) {
        const QStringView four = name.sliced(i, 4);
        if (!std::ranges::all_of(four, [](QChar c) { return c >= u'0' && c <= u'9'; })) {
            continue;
        }
        const bool century = four.startsWith(u"19") || four.startsWith(u"20");
        const QChar before = i > 0 ? name[i - 1] : QChar(u' ');
        const QChar after = i + 4 < name.size() ? name[i + 4] : QChar(u' ');
        const bool bounded = (before == u'(' || before == u'[' || before == u' ' || before == u'-') &&
                             (after == u')' || after == u']' || after == u' ' || after == u'-');
        if (century && bounded) {
            return true;
        }
    }
    return false;
}

// The most frequent value by normalised key (the first spelling seen stands for its key); ties go to the first key.
class Tally
{
public:
    void add(const QString &value, const QString &key)
    {
        if (key.isEmpty()) {
            return;
        }
        if (!m_spelling.contains(key)) {
            m_spelling.insert(key, value.trimmed());
            m_order.append(key);
        }
        ++m_counts[key];
        ++m_total;
    }

    QString bestKey() const
    {
        QString best;
        qsizetype count = 0;
        for (const QString &key : m_order) {
            if (m_counts.value(key) > count) {
                best = key;
                count = m_counts.value(key);
            }
        }
        return best;
    }
    QString best() const { return m_spelling.value(bestKey()); }
    qsizetype bestCount() const { return m_counts.value(bestKey()); }
    qsizetype distinct() const { return m_order.size(); }
    qsizetype total() const { return m_total; }

private:
    QHash<QString, qsizetype> m_counts;
    QHash<QString, QString> m_spelling;
    QStringList m_order;
    qsizetype m_total = 0;
};

QString albumKeyOf(const QString &album)
{
    return match::Normalizer::albumKey(album).key;
}

QString artistKeyOf(const QString &artist)
{
    return match::Normalizer::normalize(artist, match::FieldKind::Artist);
}

bool isVariousArtistsName(const QString &artist)
{
    static const QSet<QString> kNames{
        u"various artists"_s, u"various"_s,        u"va"_s, u"v a"_s, u"verschiedene interpreten"_s,
        u"artistes divers"_s, u"varios artistas"_s};
    return kNames.contains(artistKeyOf(artist));
}

// FNV-1a over the folder's comparison key: the same folder gets the same id on every scan.
quint64 stableId(const QString &folderKey)
{
    quint64 hash = 0xCBF29CE484222325ULL;
    for (const QChar c : folderKey) {
        hash ^= c.unicode();
        hash *= 0x100000001B3ULL;
    }
    hash &= kIdMask;
    return hash == 0 ? 1 : hash;
}

struct Group
{
    QString folder;
    QList<qsizetype> files;
};

struct AlbumFacts
{
    match::LibraryAlbum album;
    QList<qsizetype> members;
};

// What the folder's name says: an album when it is structured like one ("Artist - Album", a scene name, "Album
// (2019)"), with the album, artist and year it names.
struct FolderNaming
{
    bool album = false;
    bool variousArtists = false;
    QString title;
    QString artist;
    int year = 0;
};

FolderNaming folderNaming(const Group &group, const QList<ScannedFile> &files)
{
    QStringList names;
    names.reserve(group.files.size());
    for (const qsizetype i : group.files) {
        names.append(nameOf(files.at(i).file.path));
    }
    const QList<match::ParsedRemote> parsed = match::RemotePathParser::parseFolder(group.folder, names);
    FolderNaming naming;
    if (parsed.isEmpty()) {
        return naming;
    }
    const match::ParsedRemote &first = parsed.front();
    const match::ParseRule rule = first.album.rule;
    const bool structured = rule == match::ParseRule::ArtistAlbumFolder ||
                            rule == match::ParseRule::AlbumArtistFolder || rule == match::ParseRule::SceneFolder ||
                            rule == match::ParseRule::VariousArtistsFolder;
    naming.album = structured || (rule == match::ParseRule::AlbumFolder && hasFolderYear(nameOf(group.folder)));
    naming.variousArtists = first.variousArtists;
    naming.title = first.album.value;
    naming.artist = first.albumArtist.isSet() ? first.albumArtist.value : QString();
    naming.year = first.year.isSet() ? first.year.value : 0;
    return naming;
}

// Lidarr's various-artists rule over the album's track artists.
bool artistsVary(const QList<qsizetype> &members, const QList<ScannedFile> &files)
{
    Tally artists;
    for (const qsizetype i : members) {
        artists.add(files.at(i).artist, artistKeyOf(files.at(i).artist));
    }
    if (artists.total() < kAlbumMinTracks) {
        return false;
    }
    const auto total = static_cast<double>(artists.total());
    return static_cast<double>(artists.bestCount()) < kVariousArtistsTopShare * total ||
           static_cast<double>(artists.distinct()) > kVariousArtistsDistinct * total;
}

std::optional<AlbumFacts> albumOf(const Group &group, const QList<ScannedFile> &files)
{
    Tally albums;
    for (const qsizetype i : group.files) {
        albums.add(files.at(i).album, albumKeyOf(files.at(i).album));
    }
    const QString albumKey = albums.bestKey();
    const bool tagged = albums.bestCount() >= kAlbumMinTracks;
    const FolderNaming naming = tagged ? FolderNaming{} : folderNaming(group, files);
    // Members: the tracks of the album, and untagged tracks unless the folder also holds other albums' tracks.
    const bool otherAlbums = albums.distinct() > 1;
    AlbumFacts facts;
    for (const qsizetype i : group.files) {
        const QString key = albumKeyOf(files.at(i).album);
        if (key == albumKey || (key.isEmpty() && !otherAlbums)) {
            facts.members.append(i);
        }
    }
    if (!tagged) {
        const auto numbered =
            std::ranges::count_if(facts.members, [&files](qsizetype i) { return files.at(i).trackNumber > 0; });
        const bool named = naming.album && facts.members.size() >= kMinNamedTracks &&
                           (numbered >= kMinNamedTracks || albums.bestCount() > 0);
        if (!named) {
            return std::nullopt;
        }
    }
    Tally albumArtists;
    Tally artists;
    QHash<int, qsizetype> years;
    for (const qsizetype i : std::as_const(facts.members)) {
        const ScannedFile &file = files.at(i);
        albumArtists.add(file.albumArtist, artistKeyOf(file.albumArtist));
        artists.add(file.artist, artistKeyOf(file.artist));
        if (file.year > 0) {
            ++years[file.year];
        }
    }
    match::LibraryAlbum &album = facts.album;
    album.source = match::Source::Collection;
    album.title = albums.bestCount() > 0 ? albums.best() : naming.title;
    if (album.title.isEmpty()) {
        album.title = nameOf(group.folder);
    }
    album.variousArtists = isVariousArtistsName(albumArtists.best()) || naming.variousArtists ||
                           (albumArtists.total() == 0 && artistsVary(facts.members, files));
    if (album.variousArtists) {
        album.artist = u"Various Artists"_s;
    } else if (albumArtists.total() > 0) {
        album.artist = albumArtists.best();
    } else if (artists.total() > 0) {
        album.artist = artists.best();
    } else {
        album.artist = naming.artist;
    }
    qsizetype yearVotes = 0;
    for (auto it = years.cbegin(); it != years.cend(); ++it) {
        if (it.value() > yearVotes || (it.value() == yearVotes && it.key() < album.year)) {
            album.year = it.key();
            yearVotes = it.value();
        }
    }
    if (album.year == 0) {
        album.year = naming.year;
    }
    return facts;
}

} // namespace

QList<match::LibraryAlbum> groupAlbums(const QList<ScannedFile> &files, QHash<QString, quint64> *albumIdOfPath)
{
    // Album folders by comparison key (sorted): a file's folder, or the folder above a disc folder.
    QMap<QString, Group> groups;
    for (qsizetype i = 0; i < files.size(); ++i) {
        QString folder = parentOf(files.at(i).file.path);
        if (isDiscFolderName(nameOf(folder))) {
            folder = parentOf(folder);
        }
        if (folder.isEmpty()) {
            continue;
        }
        Group &group = groups[comparisonKey(folder)];
        if (group.folder.isEmpty()) {
            group.folder = folder;
        }
        group.files.append(i);
    }
    QList<match::LibraryAlbum> albums;
    QSet<quint64> used;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        std::optional<AlbumFacts> facts = albumOf(it.value(), files);
        if (!facts) {
            continue;
        }
        quint64 id = stableId(it.key());
        while (used.contains(id)) {
            id = (id % kIdMask) + 1; // a collision within this scan: the next free id
        }
        used.insert(id);
        facts->album.id = id;
        albums.append(facts->album);
        if (albumIdOfPath != nullptr) {
            for (const qsizetype i : std::as_const(facts->members)) {
                albumIdOfPath->insert(files.at(i).file.path, id);
            }
        }
    }
    return albums;
}

} // namespace selecta::collection
