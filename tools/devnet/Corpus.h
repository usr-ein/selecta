// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace selecta::devnet {

enum class AudioFormat {
    Mp3Cbr320,
    Mp3Vbr0,
    Mp3Cbr192,
    Flac16, // 16 bit, 44.1 kHz
    Flac24, // 24 bit, 96 kHz
    Aiff16, // 16 bit, 44.1 kHz, tags in an ID3 chunk
    Wav16,  // 16 bit, 44.1 kHz
};

QString formatName(AudioFormat format);

struct PlannedTrack
{
    QString path; // relative to the corpus folder: `<peer>/<folders…>/<file>`
    AudioFormat format = AudioFormat::Mp3Cbr320;
    QString artist;
    QString title;
    QString album;
    QString genre;
    int track = 1;
    int trackCount = 1;
    int year = 2000;
    double frequency = 440; // Hz: every track has its own tone
    int seconds = 10;
};

// A file that isn't audio: a cover image (made by ffmpeg) or text (cue sheet, rip log, nfo, playlist).
struct PlannedExtra
{
    QString path;
    QByteArray text; // written as is, unless `cover`
    bool cover = false;
};

struct CorpusPlan
{
    QStringList peers;
    QList<PlannedTrack> tracks;
    QList<PlannedExtra> extras;
};

// A small synthetic library, the same for the same seed. The artists, albums and titles are invented and the audio
// is generated tones, never music. Peers with different habits share overlapping releases, so one search finds the
// same track in several qualities: a lossless collector (FLAC 16/44.1 and one 24/96 album, cue sheets and rip logs),
// a scene-style 320 CBR sharer, a V0 sharer with disc folders, a 192 kbps "Unsorted" folder of loose files with the
// worst names, and a DJ with AIFF and WAV extended mixes. Two more peers misbehave, as the plan's devnet.conf says:
// queue_quinn keeps downloads queued for 45 s, refusing_rita refuses every one, and each alone shares one release, so
// the queue and failure paths show up for those tracks. Names carry the mess real shares have: underscores, release
// groups, disc folders, "(Original Mix)", brackets, all caps, diacritics and a Japanese title.
//
// Next to the peers' folders the plan also holds devnet.conf (Behaviour.h), tracklist.txt (one "Artist - Title" per
// shared track, for a Get tracks list) and tracklist-missing.txt (tracks nobody shares).
CorpusPlan planCorpus(quint32 seed);

// Writes `plan` into `directory`, which must not exist or be empty, running `ffmpeg` once per file: generated tones,
// tagged with artist, title, album, track, year and genre. Stops at the first failure and says why in `error`.
// `progress` gets each file's relative path once it is written.
bool writeCorpus(const CorpusPlan &plan, const QString &directory, const QString &ffmpeg, QString *error,
                 const std::function<void(const QString &)> &progress = {});

// ffmpeg from PATH (it's in the Brewfile), or an empty string.
QString findFfmpeg();

} // namespace selecta::devnet
