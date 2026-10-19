# ADR-012: Sharing and uploads

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/sharing-and-uploads.md` §3–9; `references/network-etiquette-and-safe-development.md` §5, §7; PRD SHARE-1…5, D12

## Decision

- **`ShareIndex`:** one immutable snapshot, built off-thread and swapped atomically, built from the library scan for
  roots inside the collection. It provides:
  - virtual paths (`<alias>\sub\file.ext`);
  - pre-compressed browse blobs per access level;
  - folder contents;
  - an inverted word index for search answers;
  - upload lookups.
- **A peer's string reaches a file only through an exact index lookup,** followed by an `O_NOFOLLOW` open and a
  `(dev, ino)` check.
- **Search answers:**
  - Queries need ≥ 3 characters and at least one whole word; exclusions and `*suffix` follow Nicotine+.
  - Excluded phrases are filtered out.
  - ≤ 300 files per answer.
  - A work queue of 200 pending requests, then drop.
- **`UploadManager`:**
  - 3 slots; one active upload per user; privileged users first; round-robin between users.
  - Per-user limits of 100 files / 10 000 MB, answered with the exact strings `Too many files`, `Too many megabytes`
    and `File not shared.`.
  - A 45 s start timeout and a 60 s idle timeout.
  - Legacy `TransferRequest` downloads are answered with `Queued`.
- **Honest reporting:** `SharedFoldersFiles` after login and after index changes; `SendUploadSpeed` after each
  upload.
- **macOS:**
  - FSEvents with a persisted event ID, and a daily idle verification.
  - Dataless iCloud files are skipped, with materialisation off.
  - NFC on the wire, native bytes on disk; symlinks are not followed.
  - Offline roots are hidden, not deleted.

## Consequences

- Answering searches costs an outbound peer connection each time, so the governor accounts for it.
