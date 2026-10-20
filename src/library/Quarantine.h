// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QLatin1StringView>
#include <QString>

// The macOS quarantine attribute (`com.apple.quarantine`) on what Selecta downloads, as browsers put it on theirs (PRD
// NFR-4, ADR-009): Gatekeeper checks anything executable that carries it before it first runs, so nothing that came
// from a peer runs unchecked. Audio files and pictures open as usual.
//
// Selecta writes the attribute itself, "0081;<seconds since 1970, hex>;Selecta;": the download flag and the flag
// LaunchServices puts on what it quarantines, then the time and the agent. macOS reads it back as "quarantined by
// Selecta". LaunchServices' own setter (NSURLQuarantinePropertiesKey) would also add an event to the user's
// quarantine database for every file, tests included; the attribute alone is what Gatekeeper checks.
//
// An attribute a file already has is never replaced: it may record that the user allowed the file to open.
//
// Other platforms have no such attribute: there every function does nothing and succeeds, and isSupported() is false.
// Symbolic links are never followed: a link gets the attribute itself, never the file it points to.
//
// Thread-safe (no state). Blocking file-system calls: use from workers, like FileOps.
namespace selecta::library::quarantine {

inline constexpr const char *kAttribute = "com.apple.quarantine"; // the extended attribute's name
inline constexpr QLatin1StringView kAgent{"Selecta"};

// Whether this platform has the attribute (macOS).
bool isSupported();

// The attribute's value on `path`, empty when it has none (or `path` doesn't exist).
QByteArray value(const QString &path);

// The agent the attribute names ("Selecta", "Safari"…); empty without the attribute.
QString agentOf(const QString &path);

// Marks `path` (a file or a folder) as downloaded by Selecta, unless it already has the attribute. Returns false, with
// the reason in `error`, when the attribute can't be written (a volume without extended attributes, a file Selecta
// may not change).
bool mark(const QString &path, QString *error = nullptr);

// Marks `folder` and everything inside it, at any depth. Marks as much as it can; returns false, with the first
// reason in `error`, if anything couldn't be marked.
bool markTree(const QString &folder, QString *error = nullptr);

// Gives `to` the attribute `from` has (a copy made by reading and writing gets none of its own); nothing when `from`
// has none. Returns false, with the reason in `error`, when `to` can't take it.
bool copy(const QString &from, const QString &to, QString *error = nullptr);

} // namespace selecta::library::quarantine
