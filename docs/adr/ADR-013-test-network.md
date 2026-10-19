# ADR-013: Test network and development environment

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/soulseek-ecosystem.md` §5, §9; `references/network-etiquette-and-safe-development.md` §8; `references/soulseek-protocol.md` §12.5, §13; PLAN §5

## Decision

- **`devenv/` runs a local Soulseek network.** The live server is never used from tests, CI or scripts (CLAUDE.md).
  - Soulfind is the server: a pinned release binary for native runs, and a container pinned by digest in CI.
  - Nicotine+ (headless) and slskd are reference peers, with pre-written configs.
- **The distributed network,** which Soulfind lacks, is tested with in-repo `FakeServer` / `FakePeer` fixtures,
  following soulseek-rs's mock-parent technique and aioslsk's mock server.
- **A counting proxy acts as referee:** CI integration scenarios must stay within the governor's budgets.
- **CTest starts a fresh Soulfind per test** on a random port with a temporary database, as soulseek-rs does.
- **`FakeNetwork`** is an in-process fake for UI work without any server.
- **Debug builds refuse a public server address** unless it is explicitly enabled at build time and at run time
  (SAFE-3).
- **Test audio is synthetic.** There is no copyrighted music in the repository.

## Consequences

- Docker-based integration tests run on Linux CI only, since macOS runners have no nested virtualisation. macOS
  runs native Soulfind.
