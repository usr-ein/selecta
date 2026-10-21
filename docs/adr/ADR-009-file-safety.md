# ADR-009: File safety: Inbox, conversion on arrival, filing

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** PRD D2, D3, D11, D29, principle 2; `references/conversion-and-metadata.md` §9.4; `references/library-matching-and-organization.md` §12, §15

## Decision

- **`FileOps` is the only code that writes to disk outside the caches.**
  - In the collection it can only *create* new files, never overwriting.
  - In the Inbox it can modify files and trash them.
  - There is no API to modify, move or delete a file inside a collection root, and a test asserts it.
- **On arrival (D29),** each download is processed in a temporary file:
  1. Converted to the Inbox format.
  2. Tagged from its matched release.
  3. Verified: it decodes fully, its duration is within tolerance, and its tags and artwork are present.
  4. Published into the Inbox.

  A lossless original goes to the Trash after verification (D11). The audio-preserving path writes tags to a copy,
  never to the original before verification.
- **Filing** moves an Inbox item into the collection: copy → verify → remove from the Inbox, never overwriting.
- **The scanner opens files read-only:** TagLib gets an explicit read-only stream. Downloads carry the quarantine
  attribute.

## Amendment, 2026-10-21 (P-010, PRD D41)

One way, the user's, to change a file in the collection: an **upgrade** saved to the collection takes the place of the
file it upgrades (`FileOps::replaceInCollection()`). Only an Inbox file of the same extension replaces a plain file at
an exact path inside a collection folder: a copy is made and verified (SHA-256) under a hidden name next to the old
file, the old file goes to the macOS Trash, and the copy is renamed into its place (an exclusive rename); should that
fail, the old file comes back from the Trash. The path only ever holds the old file or the new one; a crash between
the two steps leaves the old file in the Trash and the new one still in the Inbox. Nothing else in a collection folder
can be modified, moved or deleted.

## Consequences

- A crash at any step loses nothing. Filed files are never touched again, so rekordbox's links stay valid.
