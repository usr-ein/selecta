# Third-party notices

Selecta is free software under the GNU General Public License, version 3 or later (`LICENSE`). It links the
libraries below statically (ADR-004) and follows ADR-010 for anything it takes from other projects.

## Behaviour ported from other projects

Selecta contains no code copied or translated from another Soulseek client. Where it follows another client's
behaviour, that is listed here.

- **Nicotine+** (GPL-3.0-or-later), `pynicotine/slskproto.py`, `slskmessages.py` and `uploads.py` at commit
  `f3b9d4d`. Followed by `src/slsk/` (see `src/slsk/DESIGN.md`):
  - frame size caps;
  - distributed parent selection and the child limit;
  - connection replacement;
  - upload close handling and the 45 s upload start timeout;
  - the exact denial strings.

Nothing was read from or ported from Soulseek.NET, slskd, Seeker, sockseek or soulseekcpp.

Port mapping (`src/core/PortMapClient.cpp`) is written from the specifications: RFC 6886 (NAT-PMP) and the UPnP Device
Architecture and IGD WANIPConnection documents. No code is taken from miniupnpc; only libnatpmp's default-gateway lookup
is linked.

## Libraries linked into Selecta

Versions are pinned by `vcpkg.json` (its `builtin-baseline`) and `cmake/vcpkg-ports/`.

| Library | Licence | Used for |
|---|---|---|
| Qt 6 (qtbase) | LGPL-3.0-only / GPL-3.0 | the application framework |
| FFmpeg (libavcodec, libavformat, libswresample), built without `--enable-nonfree` | LGPL-2.1-or-later | decoding audio |
| LAME (libmp3lame) | LGPL-2.0-or-later | MP3 encoding |
| libsoxr | LGPL-2.1-or-later | resampling |
| TagLib | LGPL-2.1-only or MPL-1.1 | reading and writing tags |
| SQLite | public domain | the library database |
| OpenSSL | Apache-2.0 | TLS for the metadata services (through Qt) |
| RapidFuzz (rapidfuzz-cpp) | MIT | fuzzy string matching |
| zlib | Zlib | Soulseek message compression |
| libnatpmp | BSD-3-Clause | finding the default gateway, for opening the listening port on the router (NET-3) |
| libjpeg-turbo, libpng, FreeType, HarfBuzz, PCRE2, double-conversion, Brotli, bzip2, md4c, BLAKE2 (libb2) | BSD-style, FTL, MIT, Boost, IJG and similar permissive licences | dependencies of Qt |

Tests also use GoogleTest and Google Benchmark (BSD-3-Clause); neither ships in the app.

The full licence texts of these libraries come with their sources. A release build collects them into the app
bundle's `Contents/Resources/Licenses/` (PLAN, release checklist).
