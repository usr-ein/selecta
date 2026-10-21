# Contract: `collection/`, the collection scanner

| | |
|---|---|
| **Owner** | collection library agent, branch `lib/collection` |
| **Integrator** | head implementer (the `file` table, scan scheduling on the engine's pool, Watcher events, MatchIndex snapshots, the UI) |
| **Spec** | `references/library-matching-and-organization.md` §8 (album folders), §12 (library scanning: read-only TagLib streams, exclusions, FSEvents); `references/sharing-and-uploads.md` (dataless placeholders) |
| **Decisions** | PRD LIB-1, LIB-2, DL-11 and the Owned state of Get tracks lines; ADR-009 (file safety: never write under the user's folders) |

## Purpose and boundary

Core needs to know what the user already owns. The scanner reads the user's music folders, **read-only**, and
produces what `match::MatchIndex::build` needs: one `ScannedFile` per audio file (tags, stream facts, identity) and
the albums among them. The collection is two folders with roles (PRD LIB-1, D39): a tracks folder of loose files and
an albums folder with one album per subfolder.

Core does the rest: the database table, scheduling scans on the engine's pool, applying Watcher events, building
MatchIndex snapshots, the UI. The scanner has no database, no settings and no network access.

## API

1. `struct FileSignature { QString path; qint64 size; qint64 modifiedMs; }`. Paths are absolute.
2. `struct ScannedFile { FileSignature file; QString artist, title, album, albumArtist; int trackNumber = 0,
   discNumber = 0, year = 0; qint64 durationMs = 0; match::AudioQuality quality; QString isrc;
   bool namesFromFileName = false; }`
   - The title keeps its version text, as tagged.
   - Quality and duration come from the stream headers through `audio::probeStreamInfo()`: header only (tens of KB),
     never a decode and never a whole-file read. Lengths are exact when the header carries them (STREAMINFO, a LAME
     frame count, chunk sizes, an MP4 index), estimated otherwise.
   - When there are no tags (no title), the file and folder names are parsed with match's `RemotePathParser`, with
     the folder's other audio files as siblings; `namesFromFileName` says so.
3. `struct ScanResult { QList<ScannedFile> added, changed; QStringList removed;
   QList<QPair<QString, QString>> unreadable /* path, reason */; bool cancelled = false; }`
4. `ScanResult scan(const QStringList &roots, const QHash<QString, FileSignature> &known,
   const ScanOptions &options = {}, const audio::CancellationFlag *cancel = nullptr,
   std::function<void(int done, int total)> progress = {})`
   - Blocking; it uses its own reader threads (`ScanOptions::readers`). `progress` is called on the calling thread.
   - Only files whose signature differs from `known` are read.
   - Paths in `known` that are under the roots but no longer exist are reported as removed. A root that isn't there
     (an unmounted volume) or a folder that can't be listed is reported as unreadable, and the known files under it are
     **not** reported as removed (LIB-2: offline volumes keep their contents).
   - Cancelled: `added` and `changed` hold the files read so far, `removed` is empty unless the walk finished.
5. Skipped: hidden files and folders (dot names and the macOS hidden flag), AppleDouble `._*` files, `*.selecta-part`
   and other temporary files, non-audio extensions, folders listed in `ScanOptions::excludedFolders` (Selecta's own
   Inbox and caches when they live under a root), what the folder roles leave out (`ScanOptions::tracksFolders`: no
   subfolders; `ScanOptions::albumsFolders`: no files lying in the folder itself), and symlinks that point outside the
   roots. Loops are never followed:
   folders and files are visited once by (device, inode). iCloud placeholders whose data isn't on disk are reported as
   unreadable and never opened (opening one starts a download).
6. Read-only, always: tags through `convert::readTags` (TagLib on a read-only stream) with `TagReadOptions` bounds
   (no pictures), stream facts through `audio::FileSource` (read-only). Corrupt files are reported as unreadable, never
   crash the scan. Nothing is ever written under the roots.
7. `class FolderRoles { FolderRoles(tracks, albums); roots(); applyTo(ScanOptions &); rescanFor(folder);
   roleOf(path, &albumFolder); }`: the two folders (either may be unset) and what each holds. A file lying in the
   tracks folder is a track; a file under a subfolder of the albums folder is a track of that album; anything else
   isn't part of the collection.
   - `QList<match::LibraryAlbum> albumsOf(const QList<ScannedFile> &files, const FolderRoles &folders,
     QHash<QString, quint64> *albumIdOfPath)`: one album per subfolder of the albums folder, with every file under it
     whatever its tags say; an empty albums folder has none, loose tracks are never one. Title, artist and year from
     the tags, else the folder's name. Various-artists albums are marked. Ids are stable across scans (derived from
     the album folder's path).
   - `groupAlbums(files, albumIdOfPath)` guesses album folders from tags and names (at least 3 tracks sharing an album
     tag, or folder naming that says so; disc folders merged), for folders without a role (selecta-cli).
8. `class Watcher : public QObject` covering the roots: FSEvents on macOS (CoreServices), `QFileSystemWatcher`
   elsewhere. Changes are coalesced with about 2 s latency and reported as `changed(QStringList folders)`: rescan the
   folders `FolderRoles::rescanFor(folder)` gives. Dropped events report the roots.
9. Performance: a first scan of 20 k files under about 60 s on an M-series Mac (I/O bound); a rescan with nothing
   changed under about 2 s for 20 k files (stat only). Google Benchmark in `tests/collection/bench/`.
10. Unicode: macOS paths may come back in NFD. Signatures are compared with paths normalised to NFC; results report the
    real on-disk path.
11. `selecta-cli collection scan <root>…`: counts, timings, unreadable files. Read-only, for debugging against real
    folders.

## Threading

`scan()`, `albumsOf()` and `groupAlbums()` are reentrant, and `FolderRoles` is a value type: call them from a worker,
any number at once. `Watcher` lives on the thread
that created it, which must run an event loop; `changed` is emitted there.

## Tests and acceptance

- Synthetic test audio only (`tests/audio/support`), in temporary folders; tests never touch anything else.
- Every skip rule, the folder roles, NFC/NFD signatures, removed and offline roots, symlinks inside and outside the
  roots and loops, corrupt and empty files, files without tags, every format's stream facts, albums by folder role
  and album grouping (tags, folder names, disc folders, various artists), cancellation and progress, the Watcher on
  create, modify and delete.
- Lint clean (`tools/lint.sh src/collection tests/collection`), tests green under `dev` and `asan`.
