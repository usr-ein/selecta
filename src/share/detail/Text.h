// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QStringView>

// Internal: names and words of the share index. Pure functions; any thread.
namespace selecta::share::detail {

// What build() shares, by extension (decision D-S1).
enum class FileKind : quint8 {
    Audio,     // the collection's audio extensions (match::RemotePathParser::fileKind)
    Image,     // jpg, jpeg, png
    Companion, // cue, log, nfo, txt, pdf
    Other,     // not shared
};

// By the name's extension, case-insensitively.
FileKind fileKind(QStringView name);
// The extension in lower case without the dot; empty when there is none ("x", "x.", ".x").
QString lowerExtension(QStringView name);

// Skip rules by name.
bool isHiddenName(QStringView name);     // ".x": dot files and folders, AppleDouble "._x", .DS_Store
bool isTemporaryName(QStringView name);  // *.selecta-part, *.part, *.partial, *.tmp, *.temp, *.crdownload, *.download,
                                         // *.!qB, ~$x, x~
bool isJunkFolderName(QStringView name); // $RECYCLE.BIN, System Volume Information, @eaDir, #recycle, #snapshot,
                                         // Network Trash Folder, Temporary Items

// A name read from a folder listing, as the index uses it.
struct LocalName
{
    QString key;         // as Qt decodes it (QFile::decodeName: NFC on macOS), so paths match collection::scan()'s
    QString virtualName; // what peers see: NFC, with `\` (the wire separator) replaced by `＼` (U+FF3C)
    QString problem;     // why it can't be shared (then key and virtualName are empty): invalid UTF-8, a control
                         // character
};
LocalName localName(QByteArrayView nativeName);

// The replacement for `\` inside a virtual name.
inline constexpr char16_t kBackslashReplacement = u'＼';

// An alias as peers may see it: NFC, `\` and `/` → `_`, control characters removed, trimmed, at most 255 characters;
// "Shared" for an empty result, ".", "..".
QString cleanAlias(QStringView alias);
// cleanAlias() of each, made unique case-insensitively in order: "Music", "Music (2)", "Music (3)".
QStringList uniqueAliases(const QStringList &aliases);

// The folded form of one word of match::QueryPlanner::responderWords (already case-folded): NFKD, non-spacing marks
// dropped, letters that don't decompose transliterated (ø æ œ ß đ ð þ ł ı ħ ŧ), case-folded again (compatibility
// forms can decompose to capitals), NFC. ASCII words come back as they are.
QString foldWord(QStringView word);
// The folded words of `text`, in order, repeats included: what a file's path and a query are matched on.
QStringList searchWords(QStringView text);
// Unicode code points in `text` (a surrogate pair counts once).
qsizetype codePointCount(QStringView text);

} // namespace selecta::share::detail
