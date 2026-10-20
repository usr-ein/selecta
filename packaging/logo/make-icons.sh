#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Draws Selecta's icons from the logo in this folder (D36): a vinyl record with the flag of Jamaica as its label. The
# icons are committed. After changing a drawing or the variant below, run this, look at the results and commit them.
#
#   packaging/logo/make-icons.sh
#
# Writes:
#   packaging/macos/Selecta.icns      the round record, the bundle's CFBundleIconFile (macOS 14 and 15, and builds
#                                     without Xcode)
#   packaging/macos/Selecta.icon/     the Icon Composer document (the record on a dark tile) that actool compiles into
#                                     Assets.car: macOS 26 and later draw every app icon in their own rounded square
#   packaging/icons/selecta-<n>.png   the window icon on Linux and Windows, n = 16 to 256 px
#
# Needs macOS (iconutil), rsvg-convert (brew install librsvg) and ImageMagick 7 (brew install imagemagick).
set -euo pipefail

# The variant in use: a, b or c. Each has a full drawing, selecta-logo-<variant>.svg, and a simpler one for 16 to
# 32 px, selecta-logo-<variant>-small.svg.
variant=b

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
art="$here/selecta-logo-$variant.svg"
small="$here/selecta-logo-$variant-small.svg"

for tool in rsvg-convert magick iconutil; do
    command -v "$tool" >/dev/null || { echo "make-icons: $tool is missing (see the header of $0)" >&2; exit 1; }
done
[[ -f "$art" && -f "$small" ]] || { echo "make-icons: no drawings for variant '$variant' in $here" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/selecta-icons.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# Each drawing is rendered large once and scaled down with Lanczos, which keeps the small sizes sharper than rendering
# them directly. Icon Composer casts its own shadow, so its layer is the record without the drawing's drop shadow.
rsvg-convert -w 2048 -h 2048 "$art" -o "$work/art.png"
rsvg-convert -w 512 -h 512 "$small" -o "$work/small.png"
sed '/id="shadow"/d' "$art" | rsvg-convert -w 2048 -h 2048 -o "$work/disc.png"

# scaled <source> <pixels> <output>: 8-bit RGBA without metadata, so the same drawing always gives the same file.
scaled() {
    magick "$1" -filter Lanczos -resize "$2x$2" -strip "PNG32:$3"
}
# The drawing for a size: the simple one up to 32 px, where the grooves would only blur the label.
source_for() {
    if (($1 <= 32)); then echo "$work/small.png"; else echo "$work/art.png"; fi
}

iconset="$work/Selecta.iconset"
mkdir "$iconset"
for points in 16 32 128 256 512; do
    scaled "$(source_for "$points")" "$points" "$iconset/icon_${points}x${points}.png"
    scaled "$(source_for $((points * 2)))" $((points * 2)) "$iconset/icon_${points}x${points}@2x.png"
done
iconutil -c icns "$iconset" -o "$root/packaging/macos/Selecta.icns"

# The record fills 92% of the tile: the layer is drawn at 86% of its canvas, scaled by 1.07.
doc="$root/packaging/macos/Selecta.icon"
rm -rf "$doc"
mkdir -p "$doc/Assets"
scaled "$work/disc.png" 1024 "$doc/Assets/disc.png"
cat >"$doc/icon.json" <<'JSON'
{
  "fill" : {
    "linear-gradient" : [
      "srgb:0.17000,0.17000,0.18000,1.00000",
      "srgb:0.03000,0.03000,0.03300,1.00000"
    ],
    "orientation" : {
      "start" : {
        "x" : 0.5,
        "y" : 0
      },
      "stop" : {
        "x" : 0.5,
        "y" : 1
      }
    }
  },
  "groups" : [
    {
      "layers" : [
        {
          "glass" : false,
          "image-name" : "disc.png",
          "name" : "disc",
          "position" : {
            "scale" : 1.07,
            "translation-in-points" : [
              0,
              0
            ]
          }
        }
      ],
      "shadow" : {
        "kind" : "neutral",
        "opacity" : 0.5
      },
      "translucency" : {
        "enabled" : false,
        "value" : 0.5
      }
    }
  ],
  "supported-platforms" : {
    "squares" : "shared"
  }
}
JSON

mkdir -p "$root/packaging/icons"
for pixels in 16 24 32 48 64 128 256; do
    scaled "$(source_for "$pixels")" "$pixels" "$root/packaging/icons/selecta-$pixels.png"
done

echo "make-icons: variant $variant written to packaging/macos and packaging/icons"
