# Contract: `share/`, the share index and the upload queue

| | |
|---|---|
| **Owner** | share library agent, branch `lib/share` |
| **Integrator** | head implementer (`core/ShareService`: settings, the database, slsk wiring, the governor, the UI) |
| **Spec** | `references/sharing-and-uploads.md` §3 (wire format), §4 (answering searches), §5 (uploads), §6 (macOS indexing), §7 (security), §8 (design sketch), §9.2 (defaults) |
| **Decisions** | PRD SHARE-1…3, SHARE-5, D12 (the whole collection is shared, read-only); ADR-012; ADR-009 (never write under the user's folders) |

## Purpose and boundary

Selecta shares the user's collection (D12). Peers browse it, ask for folder contents, search it and download from
it. This library answers those questions from an immutable in-memory index, opens shared files safely, and decides
the order of uploads. It has no database, no settings, no network and no timers of its own.

Core does the rest: which folders are shared (the collection roots by default), building the index on a worker when
the collection changes, handing slsk the answers (`slsk::ShareProvider`, `Client::answerSearch`), starting and
cancelling uploads through `slsk::TransferManager`, recording upload statistics, and the UI.

Decisions taken for the maintainer's open questions in the note (§9.3), use these:

- D-S1: share audio files, nothing else (PRD D40, P-016; it first also shared cover images and text companions);
- D-S2: a banned user is answered `Banned` (core decides who is banned; the queue only carries the string);
- D-S4: no buddy-only folders in v1 (one public access level, but keep the level in the design so P1 can add one);
- D-S7: incoming queries match with diacritics folded.

## API (namespace `selecta::share`)

1. `struct ShareRoot { QString path; QString alias; Layout layout; }`. `path` is absolute. `alias` is the virtual root
   name peers see; core passes the folder's base name and the library makes aliases unique (`Music`, `Music (2)`).
   `layout` is the folder's role in the collection (PRD LIB-1, SHARE-1, D40): `Tracks` shares the tracks lying in the
   folder, `Albums` the tracks lying in each of its subfolders (one album each), `TracksAndAlbums` both. Nothing else
   is ever shared: no deeper folder, no loose file in an albums folder, no file that isn't audio.
2. `struct BuildOptions { QStringList excludedFolders; }`: folders under a root that are never shared (Selecta's
   Inbox, preview cache, incoming and staging folders when they live under a root).
3. `struct BuildResult { std::shared_ptr<const ShareIndex> index; QList<QPair<QString, QString>> unreadable;
   bool cancelled = false; }`
4. `BuildResult build(const QList<ShareRoot> &roots, const QHash<QString, collection::ScannedFile> &knownAudio,
   const BuildOptions &options = {}, const audio::CancellationFlag *cancel = nullptr)`
   - Blocking, reentrant; call it from a worker. Walks the roots (stat only) as far as their layouts reach, never
     follows symlinks, visits each (device, inode) once, shares a root met inside another as itself only, skips hidden files and folders, AppleDouble files, temporary and partial files
     (`*.selecta-part`, `*.part`), excluded folders, and iCloud placeholders whose data isn't on disk (never opened).
   - Audio files take their attributes (bitrate, duration in seconds, VBR, sample rate, bit depth) from `knownAudio`
     (core passes the collection scan, keyed by absolute path). An audio file missing from it is shared with what
     `audio::probeStreamInfo()` reads from its header, or with no attributes when that fails.
   - A root that isn't there (an unplugged drive) is reported in `unreadable` and contributes nothing.
   - Memory: at 100 000 files the index stays under about 60 MB; a first build of 20 000 files with `knownAudio`
     complete takes under about 3 s on an M-series Mac (stat only). Google Benchmark in `tests/share/bench/`.
5. `class ShareIndex`, immutable after `build()`; every method is const and safe to call from any thread at once:
   - `QByteArray sharedFileList() const`: the precompressed `SharedFileListResponse` payload
     (`slsk::peer::SharedFileListResponse::encode()`), built once in `build()`. Folders that hold no shared file
     are left out; the virtual tree keeps its parents.
   - `QList<slsk::SharedDirectory> folderContents(const slsk::RemotePath &folder) const`: one folder and its files,
     no subfolders; empty for an unknown folder. Case-insensitive fallback, as clients send paths in odd cases.
   - `QList<slsk::FileEntry> search(QStringView query, const QStringList &excludedPhrases, int cap = 300) const`:
     Nicotine+ semantics (note §4.2): words split as responders split paths
     (`match::QueryPlanner::responderWords`), every word must appear in the virtual path, `-word` excludes, a
     leading `*` matches a word suffix, at least one whole word of 3 or more characters, diacritics folded. A query
     containing an excluded phrase (case-insensitive) gets no answer. At most `cap` files, full virtual paths.
     Target: under 1 ms for a typical query at 100 000 files (an inverted word index).
   - `std::optional<SharedFile> lookup(const slsk::RemotePath &path) const`: the only way from a peer's string to a
     local file. Exact match of the virtual path (NFC; Latin-1 bytes from old clients decoded the way
     `RemotePath::display()` does), then the case-insensitive fallback. `SharedFile { QString localPath;
     quint64 size; quint64 device; quint64 inode; slsk::FileEntry entry; }`.
   - `struct Counts { int folders; int files; }; Counts counts() const` for `SharedFoldersFiles` and the UI.
6. `Opened open(const SharedFile &file)`: opens the file for an upload. `O_NOFOLLOW`, then `fstat` must match the
   index's device, inode and size, and the file must be a regular file with its data on disk. `Opened` holds a
   `std::unique_ptr<QIODevice>` open read-only (seekable) or an error: `File not shared.` when the file is gone,
   replaced or changed. Never blocks for long: no reads before the transfer asks for them.
7. `class UploadQueue`, pure policy (note §5.2, §5.3, §9.2), single-threaded (core keeps it on the engine thread):
   - `Options { int slots = 3; int maxFilesPerUser = 100; qint64 maxBytesPerUser = 10'000'000'000; }`.
   - `Decision enqueue(const QString &user, const slsk::RemotePath &path, quint64 size, bool privileged)`: accepted,
     or denied with the exact standard string (`Too many files`, `Too many megabytes`). A repeat of a queued file
     is accepted without a second entry.
   - `std::optional<Next> next()`: the item to start now, if a slot is free: one active upload per user, privileged
     users first, then round-robin by the oldest "last served" time, each user's files in FIFO order. `Next` names
     the user, path and size.
   - `started(user, path)`, `finished(user, path)` (any outcome), `remove(user, path)`, `removeUser(user)` (went
     offline, banned), `setPrivileged(QSet<QString>)`.
   - `std::optional<quint32> placeInQueue(user, path) const`: the 1-based place in the order `next()` would serve.
   - `int queued() const`, `int active() const`, `bool slotFree() const` for search answers and user info.
   - Bounded: at most `maxFilesPerUser` per user and 10 000 entries in total (beyond that, `Too many files`).
8. `selecta-cli share index <root>…`: counts, timings, and `--search <query>` / `--browse` to print answers. Read-only.

## Security (note §7, NFR-4)

Every string from a peer is hostile. No path is ever built by joining a peer's string to a local folder: `lookup()`
is an exact map lookup and `open()` re-checks the identity of what it opens. Virtual paths never reveal local paths
(the alias replaces everything above the root). Inputs are bounded: queries over 256 characters are refused,
folder requests over 4 KB find nothing.

## Threading

`build()` is reentrant. `ShareIndex` is immutable and shared through `std::shared_ptr<const ShareIndex>`: core swaps
the pointer on its thread when a new index is built. `open()` may be called from any thread. `UploadQueue` is not
thread-safe: one owner thread.

## Tests and acceptance

- Synthetic files in temporary folders only (`tests/audio/support` makes real audio); nothing else is touched.
- Every skip rule; symlinks inside and outside the roots and loops; NFC/NFD names; duplicate aliases; a missing
  root; files that change, vanish or are replaced by a symlink between `build()` and `open()`.
- Search semantics against the note's table (§4.2): AND of words, exclusions, `*suffix`, diacritics, short words,
  excluded phrases, the cap, odd separators in paths.
- The share list and folder contents decode with slsk's own decoders to exactly what was indexed.
- Upload order: slots, one per user, privileged first, round-robin, FIFO per user, limits and their strings,
  repeats, removal, place in queue.
- Lint clean (`tools/lint.sh src/share tests/share`), tests green under `dev` and `asan`.
