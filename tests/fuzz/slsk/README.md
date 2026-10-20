# slsk fuzz targets

One libFuzzer target per decoder family of the Soulseek codec (`src/slsk/`):

| Target | What it feeds |
|---|---|
| `slsk_fuzz_server` | server-channel frames, decoded in both directions (`decodeFromServer`, `decodeToServer`) |
| `slsk_fuzz_peer_init` | peer-init frames (`PeerInit`, `PierceFireWall`) and the peer-username check |
| `slsk_fuzz_peer` | peer frames, including the zlib share lists, search results (and their cheap header peek) and folder contents, plus the raw `F` handshake |
| `slsk_fuzz_distrib` | distributed frames, including searches embedded in `EmbeddedMessage` |
| `slsk_fuzz_zlib` | capped inflate and prefix inflate |

Every input is read both as a framed stream (through `FrameReader`, with the channel's size cap, split into two
chunks) and as one unframed payload. Besides AddressSanitizer and UBSan findings, a target fails when a message that
decodes doesn't come back identical after re-encoding.

## Running

Apple clang has no libFuzzer, so these targets build only in their own configuration with Homebrew LLVM
(`brew install llvm`). One command configures `build/fuzz-slsk`, builds the targets, writes a seed corpus from the
unit-test samples and runs each target:

```sh
export VCPKG_ROOT="$HOME/vcpkg"
tests/fuzz/slsk/fuzz.sh            # 60 s per target (the acceptance smoke run)
tests/fuzz/slsk/fuzz.sh 3600 peer  # one target for an hour
```

- The corpus grows in `build/fuzz-slsk/fuzz-corpus/<target>/` across runs; delete it to start over.
- Crashing inputs are saved in `build/fuzz-slsk/fuzz-artifacts/<target>/`. Reproduce one with
  `build/fuzz-slsk/tests/fuzz/slsk/slsk_fuzz_<target> <file>`, then add it as a unit test.
- The configuration is the dev one (same vcpkg packages) with `CMAKE_CXX_COMPILER` set to Homebrew clang,
  `SELECTA_SANITIZE="address;undefined"` and `SELECTA_SLSK_FUZZ=ON`, which adds `-fsanitize=fuzzer-no-link` to the
  library and `-fsanitize=fuzzer` to the targets. Warnings aren't errors in this build only.
- Zlib-compressed peer messages are fuzzed with an 8 MiB inflate cap instead of the production 128 MiB / 1 GiB, so a
  zip bomb can't exhaust libFuzzer's 2 GiB memory limit. The code path is the same.
