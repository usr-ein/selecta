// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/Error.h"
#include "collection/Scanner.h"
#include "slsk/Types.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <memory>
#include <optional>

// The share index (docs/contracts/share.md, sharing note §3–§4, §6–§7): what Selecta shares, as peers see it. build()
// walks the shared folders once into an immutable ShareIndex that answers browses, folder listings, searches and upload
// lookups from memory. No database, no settings, no network and no timers of its own.
namespace selecta::share {

namespace detail {
struct IndexData;
class IndexBuilder;
} // namespace detail

// At most this many files per search answer by default (Nicotine+'s 300).
inline constexpr int kDefaultSearchCap = 300;
// Longer queries get no answer (contract: security).
inline constexpr qsizetype kMaxQueryChars = 256;
// Longer folder and file paths from peers find nothing (contract: security).
inline constexpr qsizetype kMaxRequestPathBytes = 4096;
// Files beyond this many are left out of the index and reported (memory stays bounded on any disk).
inline constexpr int kMaxFiles = 1'000'000;
// Folders nested deeper than this under a root are left out and reported.
inline constexpr int kMaxDepth = 64;

// A shared folder.
struct ShareRoot
{
    QString path;  // absolute; may itself be a symlink to a folder, but nothing below it is ever followed
    QString alias; // the virtual root name peers see; core passes the folder's base name, build() makes it unique

    friend bool operator==(const ShareRoot &, const ShareRoot &) = default;
};

struct BuildOptions
{
    // Absolute folders under the roots that are never shared: Selecta's Inbox, preview cache, incoming and staging
    // folders when they live under a root. Matched by (device, inode) and by path, so no other path reaches them.
    QStringList excludedFolders;
};

class ShareIndex;

struct BuildResult
{
    // Never null. When the build was cancelled it is empty (nothing shared): never publish a cancelled build.
    std::shared_ptr<const ShareIndex> index;
    // (local path, reason), for the user and the local log, never for peers: roots that aren't there (an unplugged
    // drive), folders that can't be listed or are nested too deep, iCloud placeholders whose data isn't on this Mac,
    // names that can't be shared (not UTF-8, control characters, the same virtual name as a sibling), files beyond
    // kMaxFiles. Sorted by nothing: in the order met.
    QList<QPair<QString, QString>> unreadable;
    bool cancelled = false; // see build()
};

// Builds the index of `roots`, in the order given.
//
// The walk: stat only (lstat on every entry, one listing per folder), names sorted. Symlinks below a root are never
// followed (a root itself may be one). Every (device, inode) is visited once, folders and files alike, so hard links,
// loops and a root inside another root share each thing once, under the first path met.
//
// Shared: audio files (the collection's audio extensions: mp3 flac wav aif aiff m4a alac aac ogg opus wma wv ape),
// cover images (jpg jpeg png) and text companions (cue log nfo txt pdf), sharing note decision D-S1. Skipped silently:
// other extensions, empty files, hidden names (dot files, AppleDouble "._x") and the macOS hidden flag, temporary and
// partial files (*.selecta-part, *.part, *.tmp, …), folders in options.excludedFolders, system junk folders
// ($RECYCLE.BIN, System Volume Information, @eaDir, #recycle, #snapshot, Network Trash Folder, Temporary Items).
// Skipped and reported in `unreadable`: iCloud placeholders whose data isn't on disk (never opened: opening one
// starts a download), names that aren't valid UTF-8 or contain control characters, a name whose virtual form equals a
// sibling's (NFC/NFD twins on a volume that keeps both).
//
// Attributes of audio files (bitrate, duration in whole seconds, VBR flag, or sample rate and bit depth for lossless
// files, as Nicotine+ packs them) come from `knownAudio`: the collection scan, keyed by absolute path as
// collection::scan() reports it (another normalisation of the same path is found too). An entry whose size or
// modification time differs from the file's is stale and not used. A file without a usable entry gets what
// audio::probeStreamInfo() reads from its header (the file opened with O_NOFOLLOW and checked to be the walked file),
// or no attributes when that fails. Other files have no attributes.
//
// Virtual paths: `<alias>\<folders…>\<name>`, NFC, never revealing anything above the root. A `\` inside a local name
// becomes `＼` (U+FF3C) in its virtual name. Aliases are cleaned (`\` and `/` become `_`, control characters go, empty,
// "." and ".." become "Shared") and made unique case-insensitively in root order: "Music", "Music (2)". A root that
// isn't there (an unplugged drive) or isn't a folder is reported, keeps its alias (so aliases don't shift while a
// drive is away) and contributes nothing.
//
// Cost: the walk is a few seconds for 100 000 files on an internal SSD (stat only); each probe reads tens of KB.
// Memory: see DESIGN.md (benchmark).
//
// Blocking and reentrant: call it from a worker, any number at once. `cancel` is polled between entries: a cancelled
// build returns soon with `cancelled` set and an empty index. On macOS the calling thread's iCloud materialisation
// policy is set to "off" for the duration (so nothing it reads can start a download) and restored afterwards.
// Failure modes: none thrown; whatever can't be shared is skipped or reported. Never writes anything anywhere.
BuildResult build(const QList<ShareRoot> &roots, const QHash<QString, collection::ScannedFile> &knownAudio,
                  const BuildOptions &options = {}, const audio::CancellationFlag *cancel = nullptr);

// A file that may be uploaded: what lookup() resolved a peer's path to.
struct SharedFile
{
    QString localPath;  // as the file system spells it (it may be NFD); for open() and the local log, never for peers
    quint64 size = 0;   // bytes, when it was indexed
    quint64 device = 0; // st_dev, when it was indexed
    quint64 inode = 0;  // st_ino, when it was indexed
    slsk::FileEntry entry; // as a search answer shows it: the full virtual path, size, extension and attributes

    friend bool operator==(const SharedFile &, const SharedFile &) = default;
};

// An immutable snapshot of what is shared. build() makes one; core swaps its std::shared_ptr<const ShareIndex> on its
// own thread when a new one is built.
//
// Threading: every member is const and safe to call from any number of threads at once (no caches, no locks).
// Ownership: shared through std::shared_ptr; results are values that own their data. Failure modes: none; requests
// that match nothing, or are too long, get empty answers.
class ShareIndex
{
public:
    struct Counts
    {
        int folders = 0; // the folders of sharedFileList(): those holding a shared file, and their parents
        int files = 0;

        friend bool operator==(const Counts &, const Counts &) = default;
    };

    // An empty index: nothing shared (sharedFileList() is the empty list's payload).
    ShareIndex();
    ~ShareIndex();
    ShareIndex(const ShareIndex &) = delete;
    ShareIndex &operator=(const ShareIndex &) = delete;
    ShareIndex(ShareIndex &&) = delete;
    ShareIndex &operator=(ShareIndex &&) = delete;

    // The SharedFileListResponse payload (slsk::peer::SharedFileListResponse::encode(): zlib-compressed), built once by
    // build(): every folder that holds a shared file, and the folders above it so the tree keeps its parents; each with
    // its files (bare names, sizes, lower-case extensions, attributes). Folders and files in walk order (names sorted
    // within a folder). No private (buddy-only) section: one public level in v1 (decision D-S4).
    QByteArray sharedFileList() const;

    // The folder at `folder` (a raw path as the peer sent it) and its files, without subfolders: one entry, named with
    // the index's own spelling of the path, which may hold no files when only subfolders do. Empty for an unknown
    // folder or a path over kMaxRequestPathBytes. Matched as lookup() matches files: exactly, then ignoring case.
    QList<slsk::SharedDirectory> folderContents(const slsk::RemotePath &folder) const;

    // Files whose virtual path matches `query`, Nicotine+'s way (sharing note §4.2), at most `cap`, as full virtual
    // paths in index order:
    // - words are split as responders split paths (match::QueryPlanner::responderWords), case-folded and with
    //   diacritics folded ("Röyksopp" and "royksopp" find each other, decision D-S7), on both sides;
    // - every word must be a word of the virtual path (folders, the alias and the file name with its extension);
    // - `-word` excludes files with that word; `*word` matches words ending with it;
    // - at least one plain word of 3 or more characters is needed, otherwise there is no answer;
    // - a query containing one of `excludedPhrases` (the server's list; case-insensitive) gets no answer, and a file
    //   whose virtual path contains one is never in an answer;
    // - a query over kMaxQueryChars characters gets no answer; `cap` <= 0 gives none.
    // Cost: an inverted word index; well under a millisecond for a typical query at 100 000 files (DESIGN.md).
    QList<slsk::FileEntry> search(QStringView query, const QStringList &excludedPhrases,
                                  int cap = kDefaultSearchCap) const;

    // The only way from a peer's string to a local file. `path` is matched exactly against the virtual paths (after
    // decoding it as RemotePath::display() does, UTF-8 or else Latin-1 from old clients, and normalising it to NFC),
    // then ignoring case (Soulseek NS lower-cases paths), the first match in index order. nullopt for anything else,
    // including a path over kMaxRequestPathBytes or a folder's path. The file may have changed since: open() checks.
    std::optional<SharedFile> lookup(const slsk::RemotePath &path) const;

    // For SharedFoldersFiles and the UI.
    Counts counts() const;

    // The roots as built, in order: cleaned absolute paths and the aliases peers see (missing roots included).
    QList<ShareRoot> roots() const;

    // Approximate heap bytes held by this index (records, strings, word index, lookup tables, the share list).
    qsizetype memoryBytes() const;

private:
    friend class detail::IndexBuilder;
    explicit ShareIndex(std::unique_ptr<const detail::IndexData> data);

    std::unique_ptr<const detail::IndexData> d;
};

} // namespace selecta::share
