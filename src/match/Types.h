// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFlags>
#include <QString>
#include <QStringList>
#include <QtTypes>

#include <optional>

// Shared vocabulary of the matching library (references/library-matching-and-organization.md, ADR-008).
//
// Every type in this header is a plain value. Copy it freely; const instances can be shared across threads.
namespace selecta::match {

// Where a library record comes from (PRD LIB-3).
enum class Source : quint8 {
    Collection,
    Inbox,
    History,
};

// Confidence of an ownership decision (PRD LIB-3, SRCH-11).
enum class Tier : quint8 {
    None,
    Likely,
    Exact,
};

// How a library record relates to a remote file.
enum class Relation : quint8 {
    None,         // unrelated
    Same,         // the same track
    Upgrade,      // the same track, and the remote file's quality is established and better (PRD LIB-4)
    OtherVersion, // same artist and title, another version or edit ("Other version in collection", note §5)
};

// Version classes (note §5). For a DJ, different classes are different tracks.
enum class VersionClass : quint8 {
    Unspecified,  // no version text
    Original,     // Original Mix, Original Version, Main Mix, Album Version
    Extended,     // Extended Mix, Club Mix, 12" Version, Long Version, Full Length
    Radio,        // Radio Edit, Edit (unnamed), Short Edit, Single Version, 7" Version
    Remix,        // <X> Remix, <X> Mix, <X> Rework, <X> Edit, <X> Dub, <X> Version, unnamed "Remix"
    Vip,          // VIP, VIP Mix, <X> VIP
    Dub,          // Dub, Dub Mix
    Instrumental, // Instrumental, Instrumental Mix
    Vocal,        // Vocal Mix, Vocal Version
    Acapella,     // Acapella, A Cappella
    Live,         // Live, Live at <venue>, Live Version
    Acoustic,     // Acoustic, Unplugged
    Demo,         // Demo
};

// Further class words found next to the main class: they tell versions apart within a class ("Bicep Dub" is not
// "Bicep Remix", "Extended Vocal Mix" is not "Extended Mix").
enum class VersionModifier : quint16 {
    None = 0,
    Extended = 0x0001,
    Radio = 0x0002,
    Instrumental = 0x0004,
    Vocal = 0x0008,
    Dub = 0x0010,
    Edit = 0x0020, // a named edit or re-edit ("Bicep Edit"), as opposed to a remix
    Acapella = 0x0040,
    Live = 0x0080,
    Acoustic = 0x0100,
    Demo = 0x0200,
    Vip = 0x0400,
};
Q_DECLARE_FLAGS(VersionModifiers, VersionModifier)
Q_DECLARE_OPERATORS_FOR_FLAGS(VersionModifiers)

// A classified version (VersionClassifier). Two versions are the same exactly when their key() is equal.
struct VersionInfo
{
    VersionClass versionClass = VersionClass::Unspecified;
    VersionModifiers modifiers;
    QStringList by;        // normalised remixer keys, sorted (Remix, Vip), or the normalised venue of a Live version
    QString byDisplay;     // the remixer or venue as written, for display
    bool remaster = false; // "2011 Remaster": the same recording, never blocks a match (note §5)
    bool mixed = false;    // "(Mixed)": a DJ-mixed compilation track, so its length is unreliable (note §2.2)
    QString raw;           // the version text as found, e.g. "Extended Mix"

    bool isExplicit() const { return versionClass != VersionClass::Unspecified; }

    // Canonical key: "" for Unspecified, otherwise e.g. "extended", "remix:bicep", "remix:bicep+dub". Remaster and
    // mixed flags are not part of it. Stored in the version_key column (note §13).
    QString key() const;
};

// Whether two versions can be the same track (note §5, compatibility table).
enum class VersionCompatibility : quint8 {
    Same,         // same class, remixers and modifiers (or both unspecified); remaster ignored: tier up to exact
    ProbablySame, // Unspecified against Original: exact only when the lengths agree within the exact window
    Unsure,       // Unspecified against another explicit version, or an unnamed remix against a named one: likely
                  // at best, and only when the lengths agree
    OtherVersion, // explicit versions differ, or remix by X against remix by Y: never the same track
};

// Audio codecs Selecta tells apart for quality ranking.
enum class Codec : quint8 {
    Unknown,
    Mp3,
    Aac,
    Vorbis,
    Opus,
    Wma,
    Flac,
    Alac,
    Wav,
    Aiff,
    Ape,
    WavPack,
};

// Audio properties of a local file (from the scanner) or a remote file (from its attributes).
struct AudioQuality
{
    Codec codec = Codec::Unknown;
    int bitrateKbps = 0;     // 0 = unknown; average for VBR
    std::optional<bool> vbr; // unknown when absent
    int sampleRateHz = 0;    // 0 = unknown
    int bitDepth = 0;        // 0 = unknown (lossless only)
};

// The optional attributes of a Soulseek search result (note §1). 0 means absent: peers send 0 for unknown lengths.
struct RemoteAttributes
{
    int bitrateKbps = 0;
    int durationSec = 0; // whole seconds, truncated by most clients
    std::optional<bool> vbr;
    int sampleRateHz = 0;
    int bitDepth = 0;
};

// Stable lower-case names for storage, logs and explanations ("exact", "other version", "extended"…).
QString tierName(Tier tier);
QString relationName(Relation relation);
QString sourceName(Source source);
QString versionClassName(VersionClass versionClass);
std::optional<VersionClass> versionClassFromName(QStringView name);
QString compatibilityName(VersionCompatibility compatibility);

} // namespace selecta::match
