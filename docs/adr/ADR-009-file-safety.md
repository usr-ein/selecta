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

## Consequences

- A crash at any step loses nothing. Filed files are never touched again, so rekordbox's links stay valid.
