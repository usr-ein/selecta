# `collection/` — the collection scanner

Reads the user's music folders, read-only, into what `match::MatchIndex::build` needs. Contract:
`docs/contracts/collection.md`. Spec: `references/library-matching-and-organization.md` §8 and §12. Depends on
`audio/` (Decoder, FileSource, CancellationFlag), `convert/` (readTags) and `match/` (AudioQuality, LibraryAlbum,
RemotePathParser, Normalizer).

## Layers

```
Scanner.h   scan(): walk (stat only) → compare signatures → read the files that changed (reader threads)
            ├─ detail/Walk      the folder walk: skip rules, (device, inode) visited sets, signatures, NFC keys
            └─ detail/FileReader one file: tags (convert::readTags), stream facts (audio::Decoder, header only),
                                names from the file and folders (match::RemotePathParser) when there's no title
Albums.h    groupAlbums(): album folders, disc folders merged, various artists, stable ids
Watcher.h   Watcher: FSEvents (macOS) or QFileSystemWatcher, coalesced, folders under the given roots
```

## Decisions (within the contract; for review)

1. **Stream facts from the header only.** `audio::Decoder::open()` on a complete MP3 or FLAC indexes every frame,
   which reads the whole file (a 300 GB first scan for 20 k files). The scanner opens files through a source that
   has every byte available but isn't declared complete, which is what the decoder does while a file downloads: it
   parses the container header, the Xing/LAME/VBRI frame or STREAMINFO, and stops. Lengths are exact with a LAME
   frame count or STREAMINFO, estimated from the size for an MP3 without either (exact for CBR). Formats only FFmpeg
   reads (MP4 without fast start, Ogg, WMA, …) answer NeedsCompleteFile and are reopened as complete files; FFmpeg's
   probing reads their header and a few packets. A VBR MP3's bitrate is the LAME tag's average.
2. **Offline volumes keep their files.** A root that isn't there, or a folder that can't be listed, is reported as
   unreadable, and the known files under it are not reported as removed (LIB-2). Truncated walks (more than
   `maxFiles` files) remove nothing.
3. **Signatures compare NFC keys.** A known path is matched exactly first, then by its NFC key; the result carries the
   real on-disk path. A file whose only change is its name's normalisation is unchanged.
4. **Visited once.** Folders and files are visited once by (device, inode): symlinks inside the roots are followed,
   loops and duplicate paths are not, and the first path in traversal order (names sorted) wins, so results are stable.
5. **Read failures.** A file whose stream the decoder can't open is unreadable. A file whose tags can't be read but
   whose stream opens is kept, with its names from the file and folders. A file that vanished between the walk and the
   read is dropped (and removed, when it was known).
6. **Reader threads of its own.** `scan()` starts `readers` threads (no global pool to compete with) and calls
   `progress` on the calling thread; each result slot is written by one reader.

## Integration points

- **Core:** keep each file's `FileSignature` (path, size, modifiedMs) in the `file` table; pass them back as `known`.
  Apply `added`, `changed`, `removed`; show `unreadable` in the library view. Build `match::LibraryTrack`s from
  `ScannedFile` (`albumId` from `groupAlbums`), and `match::LibraryAlbum`s from `groupAlbums`.
- **Watcher:** rescan each reported folder with `scan({folder}, knownUnder(folder))`.

## Progress

- Done: contract and public headers; the walk, the reader and `scan()`; `groupAlbums()`; the Watcher (FSEvents and
  the portable QFileSystemWatcher, both tested on macOS through `Watcher::Mechanism`); 48 tests, clean under dev,
  ASan and TSan.
- Next: the benchmark (20 k files) and `selecta-cli collection scan`.
- Header-only stream facts: `detail::probeStream()` in FileReader.cpp is the one place to switch when audio/ offers
  a header probe (the head implementer asked the media agent for one).
