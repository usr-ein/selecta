# ADR-010: Licence and dependency policy

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** PRD D14, NFR-6; `references/soulseek-ecosystem.md` §1.1, §10; `references/qt-app-architecture.md` §11

## Decision

- **Selecta's code is GPL-3.0-or-later,** with SPDX headers in every source file.
- **Porting code:**
  - Allowed only from GPL-3.0-or-later or permissive sources: Nicotine+, aioslsk, soulseek-rs, seeleseek, Mixxx
    (GPL-2.0-or-later), rapidfuzz.
  - Every ported fragment is listed in `THIRD-PARTY-NOTICES.md` with its upstream path and commit.
- **Never copy or translate code** from Soulseek.NET, slskd, Seeker, sockseek or soulseekcpp. Reading them for
  behaviour is fine. `CONTRIBUTING.md` states this rule.
- **Dependencies** must be GPLv3-compatible:
  - FFmpeg is never built with `--enable-nonfree`.
  - GPL-3.0-only Qt add-ons are acceptable.
  - SF Symbols images and SF fonts are never bundled; icons come from `QIcon::fromTheme` at runtime.

## Consequences

- GPL-3.0-or-later stays available, and distributed binaries satisfy every dependency licence.
