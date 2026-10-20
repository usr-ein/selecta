// SPDX-License-Identifier: GPL-3.0-or-later
#include "Corpus.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>

using namespace Qt::StringLiterals;

namespace selecta::devnet {
namespace {

// --- invented names --------------------------------------------------------------------------------------------------

constexpr std::array kFirstNames = {u"Kel",  u"Orrin", u"Mira",  u"Juno", u"Tavi", u"Elske", u"Ravi",
                                    u"Noor", u"Sil",   u"Odile", u"Bram", u"Yuki", u"Ines",  u"Caspar"};
constexpr std::array kLastNames = {u"Varo",   u"Hale",           u"Sønder", u"Quist",    u"Lindqvist", u"Mareš",
                                   u"Okafor", u"Delacroix-Ruiz", u"Ferro",  u"Nakamura", u"Brönn",     u"Vasquez",
                                   u"Eld"};
constexpr std::array kAdjectives = {u"Pumice", u"Velvet",   u"Hollow",   u"Saffron", u"Static", u"Paper",    u"Amber",
                                    u"Quiet",  u"Electric", u"Northern", u"Slow",    u"Glass",  u"Midnight", u"Copper"};
constexpr std::array kNouns = {u"Lantern", u"Harbour", u"Signal",  u"Orchard", u"Comet",    u"Tide",  u"Engine",
                               u"Garden",  u"Mirror",  u"Station", u"Ferry",   u"Meridian", u"Atlas", u"Furnace"};
constexpr std::array kGenres = {u"Techno",  u"House", u"Deep House", u"Drum & Bass",
                                u"Ambient", u"Disco", u"Electro",    u"Jazz"};
constexpr std::array kMixes = {u" (Original Mix)", u" (Extended Mix)", u" (Dub)", u" (Club Mix)"};
constexpr std::array kGroups = {u"FWYH", u"DJMIX", u"WEB", u"QMI", u"OMA"};

template <std::size_t N>
QString pick(QRandomGenerator &random, const std::array<const char16_t *, N> &words)
{
    return QString::fromUtf16(words.at(random.bounded(static_cast<quint32>(N))));
}

QString plural(const QString &noun)
{
    if (noun.endsWith(u's')) {
        return noun + u"es"_s;
    }
    if (noun.endsWith(u'y') && noun.size() > 1 && !QStringView(u"aeiou").contains(noun.at(noun.size() - 2))) {
        return noun.chopped(1) + u"ies"_s;
    }
    return noun + u's';
}

QString inventArtist(QRandomGenerator &random)
{
    switch (random.bounded(4)) {
    case 0:
        return u"The "_s + pick(random, kAdjectives) + u' ' + plural(pick(random, kNouns));
    case 1:
        return u"DJ "_s + pick(random, kNouns);
    default:
        return pick(random, kFirstNames) + u' ' + pick(random, kLastNames);
    }
}

QString inventTitle(QRandomGenerator &random)
{
    QString title = pick(random, kAdjectives) + u' ' + pick(random, kNouns);
    if (random.bounded(5) == 0) {
        title += u" feat. "_s + pick(random, kFirstNames);
    }
    return title;
}

struct Release
{
    QString artist;
    QString album;
    QString genre;
    int year = 2000;
    QStringList titles;
};

QList<Release> inventReleases(QRandomGenerator &random, int count)
{
    QList<Release> releases;
    for (int i = 0; i < count; ++i) {
        Release release;
        release.artist = inventArtist(random);
        if (random.bounded(3) == 0) {
            release.album = u"Live at the "_s + pick(random, kNouns);
        } else {
            release.album = pick(random, kNouns) + u' ' + pick(random, kNouns);
        }
        release.genre = pick(random, kGenres);
        release.year = 1994 + random.bounded(31);
        const int tracks = 3 + random.bounded(3);
        for (int t = 0; t < tracks; ++t) {
            release.titles.append(inventTitle(random));
        }
        releases.append(release);
    }
    return releases;
}

// --- naming habits ---------------------------------------------------------------------------------------------------

enum class Style {
    Tidy,     // Artist/Artist - Album (Year) [FLAC]/01 - Title.flac, with cue sheet, rip log and cover
    Scene,    // Artist_Name-Album_Name-WEB-2019-GRP/01-artist_name-title_name.mp3, with nfo, m3u and sfv
    Discs,    // Music/Artist/Album/CD1/1-01 Title.mp3
    Unsorted, // Unsorted/ARTIST - TITLE.mp3, track07.mp3, "… (Radio Edit) [192].mp3"
    DjTracks, // DJ/Genre/Artist - Title (Extended Mix).aiff
    Simple,   // Artist - Album (Year)/01. Title.flac
};

struct Habit
{
    const char16_t *peer = nullptr;
    AudioFormat format = AudioFormat::Mp3Cbr320;
    Style style = Style::Tidy;
    int releases = 0;   // from the releases several peers share
    int exclusive = -1; // and one only this peer has (index among the exclusive ones), or none
};

constexpr std::array kHabits = {
    Habit{u"lossless_lena", AudioFormat::Flac16, Style::Tidy, 3},
    Habit{u"cbr_carl", AudioFormat::Mp3Cbr320, Style::Scene, 3},
    Habit{u"vbr_vera", AudioFormat::Mp3Vbr0, Style::Discs, 2},
    Habit{u"lofi_leo", AudioFormat::Mp3Cbr192, Style::Unsorted, 3},
    Habit{u"aiff_ada", AudioFormat::Aiff16, Style::DjTracks, 2},
    // These two misbehave (devnet.conf): what only they share shows the queue and the failure paths.
    Habit{u"queue_quinn", AudioFormat::Flac16, Style::Simple, 1, 0},
    Habit{u"refusing_rita", AudioFormat::Mp3Cbr320, Style::Simple, 1, 1},
};

constexpr int kSharedReleases = 8;
constexpr int kExclusiveReleases = 2;
constexpr int kQueueSeconds = 45;

QString extensionOf(AudioFormat format)
{
    switch (format) {
    case AudioFormat::Mp3Cbr320:
    case AudioFormat::Mp3Vbr0:
    case AudioFormat::Mp3Cbr192:
        return u"mp3"_s;
    case AudioFormat::Flac16:
    case AudioFormat::Flac24:
        return u"flac"_s;
    case AudioFormat::Aiff16:
        return u"aiff"_s;
    case AudioFormat::Wav16:
        return u"wav"_s;
    }
    return u"bin"_s;
}

QString underscored(const QString &text)
{
    QString out = text;
    out.replace(u' ', u'_');
    return out;
}

QString number(int value, int width = 2)
{
    return u"%1"_s.arg(value, width, 10, QChar(u'0'));
}

class Planner
{
public:
    explicit Planner(quint32 seed)
        : m_random(seed)
    {}

    CorpusPlan plan()
    {
        QList<Release> releases = inventReleases(m_random, kSharedReleases + kExclusiveReleases);
        QList<QList<int>> picks; // per habit: the releases it shares
        for (const Habit &habit : kHabits) {
            QList<int> chosen;
            while (chosen.size() < habit.releases) {
                const int index = m_random.bounded(kSharedReleases);
                if (!chosen.contains(index)) {
                    chosen.append(index);
                }
            }
            if (habit.exclusive >= 0) {
                chosen.append(kSharedReleases + habit.exclusive);
            }
            picks.append(chosen);
        }
        // One title outside Latin script, for the encoding paths, in a release the first peer shares.
        releases[picks.at(0).at(0)].titles[0] = u"夜の信号"_s;
        for (std::size_t h = 0; h < kHabits.size(); ++h) {
            const Habit &habit = kHabits.at(h);
            const QString peer = QString::fromUtf16(habit.peer);
            m_plan.peers.append(peer);
            const QList<int> &chosen = picks.at(static_cast<qsizetype>(h));
            for (qsizetype i = 0; i < chosen.size(); ++i) {
                // The collector has one hi-res album.
                const bool hiRes = habit.style == Style::Tidy && i == 0;
                addRelease(peer, habit.style, hiRes ? AudioFormat::Flac24 : habit.format, releases.at(chosen.at(i)));
            }
        }
        addRootFiles(releases);
        return m_plan;
    }

private:
    PlannedTrack track(const Release &release, int index, AudioFormat format)
    {
        PlannedTrack t;
        t.format = format;
        t.artist = release.artist;
        t.title = release.titles.at(index);
        t.album = release.album;
        t.genre = release.genre;
        t.track = index + 1;
        t.trackCount = static_cast<int>(release.titles.size());
        t.year = release.year;
        t.frequency = 110.0 * std::pow(2.0, (12 + m_random.bounded(24)) / 12.0);
        t.seconds = 6 + m_random.bounded(7);
        return t;
    }

    void addRelease(const QString &peer, Style style, AudioFormat format, const Release &release)
    {
        const int count = static_cast<int>(release.titles.size());
        for (int i = 0; i < count; ++i) {
            PlannedTrack t = track(release, i, format);
            if (style == Style::DjTracks && m_random.bounded(3) == 0) {
                t.format = AudioFormat::Wav16; // the DJ keeps some tracks as WAV
            }
            t.path = unique(peer + u'/' + fileFor(style, t, i, count));
            m_plan.tracks.append(t);
        }
        const QString album = release.artist + u" - "_s + release.album;
        if (style == Style::Tidy) {
            const QString folder = peer + u'/' + tidyFolder(release.artist, release.album, release.year, format);
            m_plan.extras.append({folder + u"/cover.jpg"_s, {}, true});
            m_plan.extras.append({folder + u'/' + album + u".cue"_s, cueSheet(release).toUtf8(), false});
            m_plan.extras.append(
                {folder + u'/' + album + u".log"_s, "Exact Audio Copy V1.6 (synthetic log, selecta-devnet)\n", false});
        } else if (style == Style::Scene) {
            const QString name = sceneFolder(release.artist, release.album, release.year);
            m_plan.extras.append({peer + u'/' + name + u"/00-"_s + name.toLower() + u".nfo"_s,
                                  "Generated by selecta-devnet: synthetic tones, invented names.\n", false});
            m_plan.extras.append({peer + u'/' + name + u"/00-"_s + name.toLower() + u".m3u"_s, {}, false});
        }
    }

    static QString tidyFolder(const QString &artist, const QString &album, int year, AudioFormat format)
    {
        return artist + u'/' + artist + u" - "_s + album + u" ("_s + QString::number(year) +
               (format == AudioFormat::Flac24 ? u") [FLAC 24-96]"_s : u") [FLAC]"_s);
    }

    static QString sceneFolder(const QString &artist, const QString &album, int year)
    {
        const QString group = QString::fromUtf16(kGroups.at(static_cast<std::size_t>(year) % kGroups.size()));
        return underscored(artist) + u'-' + underscored(album) + u"-WEB-"_s + QString::number(year) + u'-' + group;
    }

    QString fileFor(Style style, const PlannedTrack &t, int index, int count)
    {
        const QString ext = extensionOf(t.format);
        switch (style) {
        case Style::Tidy:
            return tidyFolder(t.artist, t.album, t.year, t.format) + u'/' + number(t.track) + u" - "_s + t.title +
                   u'.' + ext;
        case Style::Scene:
            return sceneFolder(t.artist, t.album, t.year) + u'/' + number(t.track) + u'-' +
                   underscored(t.artist).toLower() + u'-' + underscored(t.title).toLower() + u'.' + ext;
        case Style::Discs: {
            const int disc = index < (count + 1) / 2 ? 1 : 2;
            return u"Music/"_s + t.artist + u'/' + t.album + u"/CD"_s + QString::number(disc) + u'/' +
                   QString::number(disc) + u'-' + number(t.track) + u' ' + t.title + u'.' + ext;
        }
        case Style::Unsorted:
            switch (m_random.bounded(3)) {
            case 0:
                return u"Unsorted/"_s + (t.artist + u" - "_s + t.title).toUpper() + u'.' + ext;
            case 1:
                return u"Unsorted/track"_s + number(++m_looseTracks) + u'.' + ext;
            default:
                return u"Unsorted/"_s + t.artist + u" - "_s + t.title + u" (Radio Edit) [192]."_s + ext;
            }
        case Style::DjTracks: {
            const QString mix = QString::fromUtf16(kMixes.at(m_random.bounded(static_cast<quint32>(kMixes.size()))));
            return u"DJ/"_s + t.genre + u'/' + t.artist + u" - "_s + t.title + mix + u'.' + ext;
        }
        case Style::Simple:
            return t.artist + u" - "_s + t.album + u" ("_s + QString::number(t.year) + u")/"_s + number(t.track) +
                   u". "_s + t.title + u'.' + ext;
        }
        return t.title + u'.' + ext;
    }

    // Next to the peers' folders (scanLibrary() shares only subfolders): the behaviour file, the tracks to paste into
    // a Get tracks list, and a few tracks nobody shares.
    void addRootFiles(const QList<Release> &releases)
    {
        QString config = u"# selecta-devnet: how some peers treat downloads, read when this folder is shared with "
                         u"--library.\n# <peer> queue <seconds>: keep each queued download that long, then upload it\n"
                         u"# <peer> refuse <reason>: refuse every download with that reason\n"_s;
        config += u"queue_quinn queue %1\n"_s.arg(kQueueSeconds);
        config += u"refusing_rita refuse Too many files\n"_s;
        for (int i = 0; i < kExclusiveReleases; ++i) {
            const Release &only = releases.at(kSharedReleases + i);
            config += u"# only %1 shares %2 - %3\n"_s.arg(i == 0 ? u"queue_quinn"_s : u"refusing_rita"_s, only.artist,
                                                          only.album);
        }
        m_plan.extras.append({u"devnet.conf"_s, config.toUtf8(), false});

        QStringList wanted;
        for (const PlannedTrack &t : std::as_const(m_plan.tracks)) {
            const QString line = t.artist + u" - "_s + t.title;
            if (!wanted.contains(line)) {
                wanted.append(line);
            }
        }
        m_plan.extras.append({u"tracklist.txt"_s, (wanted.join(u'\n') + u'\n').toUtf8(), false});
        QStringList missing;
        while (missing.size() < 3) {
            const QString line = inventArtist(m_random) + u" - "_s + inventTitle(m_random);
            if (!wanted.contains(line) && !missing.contains(line)) {
                missing.append(line);
            }
        }
        m_plan.extras.append({u"tracklist-missing.txt"_s, (missing.join(u'\n') + u'\n').toUtf8(), false});
    }

    // Two invented releases can share an artist and a title: the second file gets " (2)".
    QString unique(const QString &path)
    {
        QString candidate = path;
        for (int n = 2; m_paths.contains(candidate); ++n) {
            const QFileInfo info(path);
            candidate = info.path() + u'/' + info.completeBaseName() + u" (%1)."_s.arg(n) + info.suffix();
        }
        m_paths.insert(candidate);
        return candidate;
    }

    static QString cueSheet(const Release &release)
    {
        QString cue =
            u"PERFORMER \"%1\"\nTITLE \"%2\"\nREM DATE %3\n"_s.arg(release.artist, release.album).arg(release.year);
        for (qsizetype i = 0; i < release.titles.size(); ++i) {
            cue += u"  TRACK %1 AUDIO\n    TITLE \"%2\"\n"_s.arg(number(static_cast<int>(i) + 1), release.titles.at(i));
        }
        return cue;
    }

    QRandomGenerator m_random;
    CorpusPlan m_plan;
    QSet<QString> m_paths;
    int m_looseTracks = 0;
};

// --- ffmpeg ----------------------------------------------------------------------------------------------------------

int sampleRateOf(AudioFormat format)
{
    return format == AudioFormat::Flac24 ? 96000 : 44100;
}

QStringList codecArguments(AudioFormat format)
{
    switch (format) {
    case AudioFormat::Mp3Cbr320:
        return {u"-c:a"_s, u"libmp3lame"_s, u"-b:a"_s, u"320k"_s, u"-id3v2_version"_s, u"3"_s};
    case AudioFormat::Mp3Vbr0:
        return {u"-c:a"_s, u"libmp3lame"_s, u"-q:a"_s, u"0"_s, u"-id3v2_version"_s, u"3"_s};
    case AudioFormat::Mp3Cbr192:
        return {u"-c:a"_s, u"libmp3lame"_s, u"-b:a"_s, u"192k"_s, u"-id3v2_version"_s, u"3"_s};
    case AudioFormat::Flac16:
        return {u"-c:a"_s, u"flac"_s, u"-sample_fmt"_s, u"s16"_s};
    case AudioFormat::Flac24:
        return {u"-c:a"_s, u"flac"_s, u"-sample_fmt"_s, u"s32"_s, u"-bits_per_raw_sample"_s, u"24"_s};
    case AudioFormat::Aiff16:
        return {u"-c:a"_s, u"pcm_s16be"_s, u"-write_id3v2"_s, u"1"_s};
    case AudioFormat::Wav16:
        return {u"-c:a"_s, u"pcm_s16le"_s};
    }
    return {};
}

// Two tones a fifth apart, one per channel, pulsing twice a second: easy to tell apart, and a waveform with shape. A
// little noise makes encoders behave as with music (V0 lands near 210 kbps and FLAC near 650 kbps, where a pure tone
// would shrink to almost nothing).
QString toneSource(const PlannedTrack &track)
{
    const QString left = QString::number(track.frequency, 'f', 2);
    const QString right = QString::number(track.frequency * 1.5, 'f', 2);
    return u"aevalsrc=exprs=0.35*sin(2*PI*%1*t)*(0.65+0.35*sin(2*PI*2*t))+0.02*(2*random(0)-1)|"
           u"0.35*sin(2*PI*%2*t)*(0.65+0.35*sin(2*PI*2*t+1.5))+0.02*(2*random(1)-1)"
           u":s=%3:d=%4"_s.arg(left, right)
               .arg(sampleRateOf(track.format))
               .arg(track.seconds);
}

bool runFfmpeg(const QString &ffmpeg, const QStringList &arguments, QString *error)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(ffmpeg,
                  QStringList{u"-nostdin"_s, u"-hide_banner"_s, u"-loglevel"_s, u"error"_s, u"-y"_s} + arguments);
    if (!process.waitForStarted(10'000)) {
        *error = u"couldn't start %1: %2"_s.arg(ffmpeg, process.errorString());
        return false;
    }
    if (!process.waitForFinished(120'000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = u"ffmpeg failed: %1"_s.arg(QString::fromLocal8Bit(process.readAll()).trimmed());
        return false;
    }
    return true;
}

bool writeTrack(const PlannedTrack &track, const QString &path, const QString &ffmpeg, QString *error)
{
    QStringList arguments{u"-f"_s, u"lavfi"_s, u"-i"_s, toneSource(track)};
    arguments += codecArguments(track.format);
    arguments += {u"-metadata"_s,
                  u"title="_s + track.title,
                  u"-metadata"_s,
                  u"artist="_s + track.artist,
                  u"-metadata"_s,
                  u"album="_s + track.album,
                  u"-metadata"_s,
                  u"genre="_s + track.genre,
                  u"-metadata"_s,
                  u"date="_s + QString::number(track.year),
                  u"-metadata"_s,
                  u"track=%1/%2"_s.arg(track.track).arg(track.trackCount),
                  u"-metadata"_s,
                  u"comment=selecta-devnet synthetic tone"_s,
                  path};
    return runFfmpeg(ffmpeg, arguments, error);
}

bool writeExtra(const PlannedExtra &extra, const QString &path, const QString &ffmpeg, QString *error)
{
    if (extra.cover) {
        return runFfmpeg(ffmpeg,
                         {u"-f"_s, u"lavfi"_s, u"-i"_s, u"color=c=0x2b4c6f:s=300x300:d=1"_s, u"-frames:v"_s, u"1"_s,
                          u"-update"_s, u"1"_s, path},
                         error);
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(extra.text) != extra.text.size()) {
        *error = u"couldn't write %1: %2"_s.arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace

QString formatName(AudioFormat format)
{
    switch (format) {
    case AudioFormat::Mp3Cbr320:
        return u"MP3 320 CBR"_s;
    case AudioFormat::Mp3Vbr0:
        return u"MP3 V0"_s;
    case AudioFormat::Mp3Cbr192:
        return u"MP3 192 CBR"_s;
    case AudioFormat::Flac16:
        return u"FLAC 16/44.1"_s;
    case AudioFormat::Flac24:
        return u"FLAC 24/96"_s;
    case AudioFormat::Aiff16:
        return u"AIFF 16/44.1"_s;
    case AudioFormat::Wav16:
        return u"WAV 16/44.1"_s;
    }
    return u"?"_s;
}

CorpusPlan planCorpus(quint32 seed)
{
    return Planner(seed).plan();
}

bool writeCorpus(const CorpusPlan &plan, const QString &directory, const QString &ffmpeg, QString *error,
                 const std::function<void(const QString &)> &progress)
{
    const QDir root(directory);
    if (root.exists() && !root.isEmpty()) {
        *error = u"%1 exists and isn't empty; pick a new folder"_s.arg(directory);
        return false;
    }
    const auto prepare = [&](const QString &relative) {
        QString path = root.filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).path())) {
            *error = u"couldn't create the folder for %1"_s.arg(path);
            return QString();
        }
        return path;
    };
    const auto track = [&](const PlannedTrack &planned) {
        const QString path = prepare(planned.path);
        const bool written = !path.isEmpty() && writeTrack(planned, path, ffmpeg, error);
        if (written && progress) {
            progress(planned.path);
        }
        return written;
    };
    const auto extra = [&](const PlannedExtra &planned) {
        const QString path = prepare(planned.path);
        const bool written = !path.isEmpty() && writeExtra(planned, path, ffmpeg, error);
        if (written && progress) {
            progress(planned.path);
        }
        return written;
    };
    // Stops at the first failure.
    return std::ranges::all_of(plan.tracks, track) && std::ranges::all_of(plan.extras, extra);
}

QString findFfmpeg()
{
    return QStandardPaths::findExecutable(u"ffmpeg"_s);
}

} // namespace selecta::devnet
