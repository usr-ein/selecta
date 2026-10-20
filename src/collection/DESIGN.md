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
   which reads the whole file (hundreds of GB for a 20 k-file first scan). The media agent added
   `audio::probeStreamInfo()` for this: the container header, the Xing/LAME frame or STREAMINFO, chunk headers, an
   MP4's `moov`; tens of KB, never a decode. Lengths are exact when the header carries them, estimated from the size
   otherwise (exact for CBR). A VBR MP3's bitrate is the LAME tag's average; a CBR MP3's is snapped to the standard
   rate nearest the probe's average (which counts the Info frame too, so it runs a little high on short files).
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

## Benchmark

`selecta_collection_bench` (Release, Apple M-series, performance cores, other agents' builds running; the tree is
written once into the temporary folder, so the page cache is warm):

| Synthetic tree | 20 000 files in 2 000 album folders: 14 000 tagged MP3 320 with a 16 KB ID3 block, 4 000 tagged FLAC, 2 000 untagged MP3 named "NN - Artist - Title" (766 MB) |
|---|---|
| First scan, 4 readers | 1.6 s (12 400 files/s) |
| First scan, 8 readers | 1.2 s (16 300 files/s) |
| Rescan, nothing changed (stat only) | 0.37 s |
| `groupAlbums` | 49 ms |

The contract asks for under about 60 s and 2 s. A cold cache costs more: a scan reads each file's header and tags
(tens of KB, never the audio), about 0.8 GB here, which an internal SSD delivers in seconds; a spinning disk is where
`readers` 1–2 helps. `selecta-cli collection scan <root>… --rescan` gives the same numbers on real folders.

## Progress

- Done: contract and public headers; the walk, the reader and `scan()`; `groupAlbums()`; the Watcher (FSEvents and
  the portable QFileSystemWatcher, both tested on macOS through `Watcher::Mechanism`); 48 tests, clean under dev,
  ASan and TSan.
- Done since: the benchmark (above) and `selecta-cli collection scan <root>… [--readers n] [--rescan] [--list]`
  (counts, formats, untagged files, albums, unreadable files, timings; read-only).
- Stream facts now come from `audio::probeStreamInfo()` (main d39213b); the interim never-complete ByteSource is
  gone.
