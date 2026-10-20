# `share/` — the share index and the upload queue

What Selecta shares (PRD D12: the whole collection, read-only) as peers see it, and the order of uploads. Contract:
`docs/contracts/share.md`. Spec: `references/sharing-and-uploads.md` §3–§9, ADR-012, ADR-009. Depends on `slsk/`
(wire types and the share-list encoder), `match/` (responder word splitting), `audio/` (header probe, cancellation)
and `collection/` (`ScannedFile`, the attributes core already has). No database, no settings, no network, no timers.

## Layers

```
ShareIndex.h  build(): walk (stat only) → attributes → virtual tree → word index → lookups → share list
              ShareIndex: immutable; sharedFileList, folderContents, search, lookup, counts
              ├─ detail/Walk         the folder walk: skip rules, (device, inode) visited set, virtual names
              ├─ detail/Attributes   knownAudio entries, else audio::probeStreamInfo() on an O_NOFOLLOW descriptor
              ├─ detail/Text         word splitting and folding, virtual names, aliases, file kinds
              ├─ detail/IndexData    the snapshot's arrays (records, string pool, dictionary, postings, lookups)
              ├─ detail/IndexBuilder walk result → IndexData
              └─ detail/Query        a peer's query → plain, partial and excluded words
Open.h        open(): O_NOFOLLOW | O_NONBLOCK, fstat identity check, QFile on the descriptor
UploadQueue.h UploadQueue: per-user FIFOs, turns between users, slots, limits, place in queue
```

## Decisions (within the contract; for review)

1. **One flat snapshot.** Files are records in one array, grouped by folder (a folder's files are a contiguous id
   range, sorted by virtual name bytes); folders in walk order. Strings (virtual paths and names, local paths) live
   in one UTF-8 pool; a file stores its local name only when it differs from its virtual name (NFD on disk, a `\`).
   No per-file hash table: `lookup()` splits the path at the last `\`, finds the folder in a hash of folder paths and
   the name by binary search in the folder's range. The case-insensitive fallback goes through a hash of case-folded
   folder paths and a scan of that folder's names.
2. **Inverted word index.** Words of a file = words of its alias, folders and name (with extension), split with
   `match::QueryPlanner::responderWords` and folded (NFKD, non-spacing marks dropped, a few letters that don't
   decompose transliterated: ø æ œ ß đ ð þ ł ı ħ ŧ, case-folded again, NFC). The dictionary is sorted (word ids are
   ranks), postings are one CSR array of file ids; a second array sorts the words by their reversed spelling, so
   `*suffix` is a binary search for a range. A query walks the shortest plain word's postings in order, checks the
   others with forward galloping cursors, partial words against a bitmap of their union, exclusions the same way,
   and stops at the cap: results come out in index order, which is path order within each folder.
3. **Excluded phrases twice.** The contract refuses a query that contains an excluded phrase; the sharing note (§4.3,
   the server's own wording) drops results whose path contains one. Both are done: the second costs nothing when the
   list is empty and only touches results.
4. **"At least one plain word of 3 or more characters"** is read literally (code points of the folded word): a
   query made only of short words ("u2", "ac dc") gets no answer, which also keeps the huge postings of short words
   out of every query.
5. **What isn't shared** (beyond the contract's list): empty files (a size of 0 means "over 2 GiB" to some
   downloaders), system junk folders that hold deleted or NAS-internal files (`$RECYCLE.BIN`, `System Volume
   Information`, `@eaDir`, `#recycle`, `#snapshot`, `Network Trash Folder`, `Temporary Items`), names with control
   characters or invalid UTF-8, and a second file or folder whose virtual name equals a sibling's.
6. **Empty folders are left out** of the share list, the counts and folder listings; the parents of folders with
   files stay, so the tree renders identically everywhere.
7. **Stale knownAudio entries are not used**: an entry whose size or modification time differs from the walked
   file's is treated as missing, so the file is probed. Keys are matched exactly, then by `collection::comparisonKey`
   (a table built on the first miss), so another normalisation of the same path still finds its entry.
8. **Probes never follow symlinks or start downloads.** The probe reads through a `ByteSource` over a descriptor
   opened with `O_NOFOLLOW | O_NONBLOCK` and checked by `fstat` against what the walk saw; the build thread's iCloud
   materialisation policy is off for the duration (restored after).
9. **Local paths keep the file system's bytes.** `SharedFile::localPath` is the walk's path decoded from UTF-8
   without normalising, and `open()` encodes it back to the same bytes, so a file on a volume that stores NFC names
   (exFAT, SMB) opens even though Qt would re-encode it to NFD.
10. **`open()` errors:** `File not shared.` when the file is gone, replaced or resized (index stale); `File read
    error.` when it is the indexed file but unreadable now (permissions, I/O, descriptors, data only in iCloud).
11. **Upload turns.** A user's stamp is the queue's clock at their last upload activity: arriving in the queue, or
    an upload of theirs starting or ending (Nicotine+'s update counter). The oldest stamp goes first, so a user who
    just had a turn waits behind everyone who waited meanwhile, newcomers included, and queueing more files never
    costs a user their turn. Privileged users go first only among users who can start now, as in Nicotine+, so a
    privileged user's active upload never leaves slots idle. Per-user limits count queued files only; the global
    bound counts everything held. `placeInQueue()` counts as if uploads ran one at a time and active ones had just
    ended.
12. **One access level, kept in the design** (D-S4): every folder record carries a level (always public in v1). P1's
    buddy-only folders add a level to `ShareRoot`, a second precompressed list (the private section) and a level
    argument to `search`, `folderContents` and `lookup`.

## Integration points

- **Building:** core calls `build(roots, knownAudio, {excludedFolders})` on a worker whenever the collection or the
  shared folders change, with `roots` = the collection roots and their base names, `knownAudio` = the collection's
  files keyed by `ScannedFile::file.path` (what `CollectionStore` already holds), `excludedFolders` = Selecta's Inbox,
  preview cache, incoming and staging folders. It swaps its `std::shared_ptr<const ShareIndex>` on its own thread
  and sends `SharedFoldersFiles` with `counts()` when they changed. A cancelled build is discarded.
- **Answering:** `slsk::ShareProvider::sharedFileList()` → `index->sharedFileList()` (banned users: an empty
  `ShareIndex`'s); `folderContents()` → `index->folderContents(folder)`; `Client::searchRequestReceived` →
  `index->search(query, excludedPhrases)` (core skips its own username) → `Client::answerSearch()` unless empty.
- **Uploads:** `TransferManager::uploadRequested(user, path)` → `index->lookup(path)`; unknown → `denyUpload(…, "File
  not shared.")`; else `queue.enqueue(user, path, file.size, privileged)` → deny with `Decision::reason` when refused.
  On enqueue, slot freed and every few seconds: `while (auto n = queue.next())` → `lookup` + `open()` →
  `startUpload(…, device)` + `queue.started()`, or `queue.remove()` + `denyUpload(…, opened.error)`. Terminal
  `uploadChanged` → `queue.finished()`. `PlaceInQueueRequest` → `queue.placeInQueue()`. Search answers and user info
  report `queue.slotFree()`, `queue.queued()` and `options().uploadSlots`.

## Benchmark

Not measured yet.

## Progress

- Done: contract and public headers.
