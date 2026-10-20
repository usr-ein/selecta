#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the app icon of a built Selecta.app (D36, src/app/CMakeLists.txt): Info.plist names the round Selecta.icns,
# which holds every size macOS asks for, and, exactly when the build had actool from Xcode 26 or later, the Icon
# Composer icon in Assets.car.
#
#   tests/app/check-bundle-icon.sh <Selecta.app> <1 if built with actool, else 0>
set -euo pipefail
app="$1"
with_actool="$2"
plist="$app/Contents/Info.plist"
resources="$app/Contents/Resources"
fail() {
    echo "check-bundle-icon: $*" >&2
    exit 1
}
work="$(mktemp -d "${TMPDIR:-/tmp}/selecta-bundle-icon.XXXXXX")"
trap 'rm -rf "$work"' EXIT

plutil -lint -s "$plist" || fail "$plist isn't a valid property list"

icon_file="$(plutil -extract CFBundleIconFile raw "$plist" 2>/dev/null)" || fail "Info.plist has no CFBundleIconFile"
[[ "$icon_file" == *.icns ]] || icon_file+=.icns
[[ -f "$resources/$icon_file" ]] || fail "CFBundleIconFile names $icon_file, which isn't in Contents/Resources"
iconutil -c iconset "$resources/$icon_file" -o "$work/icon.iconset" || fail "iconutil can't read $icon_file"
for points in 16 32 128 256 512; do
    for name in "icon_${points}x${points}.png" "icon_${points}x${points}@2x.png"; do
        [[ -s "$work/icon.iconset/$name" ]] || fail "$icon_file has no $name"
    done
done

if icon_name="$(plutil -extract CFBundleIconName raw "$plist" 2>/dev/null)"; then
    [[ "$with_actool" == 1 ]] || fail "Info.plist has CFBundleIconName, but the build had no actool"
    [[ -f "$resources/Assets.car" ]] || fail "Info.plist has CFBundleIconName, but there's no Assets.car"
    # assetutil lists the catalogue as a JSON array, which plutil reads; entry 0 describes the file itself.
    assetutil --info "$resources/Assets.car" >"$work/car.json" || fail "assetutil can't read Assets.car"
    stacks=0
    for ((i = 1; ; i++)); do
        plutil -type "$i" "$work/car.json" >/dev/null 2>&1 || break
        if [[ "$(plutil -extract "$i.AssetType" raw "$work/car.json" 2>/dev/null)" == IconImageStack &&
              "$(plutil -extract "$i.Name" raw "$work/car.json" 2>/dev/null)" == "$icon_name" ]]; then
            stacks=$((stacks + 1))
        fi
    done
    ((stacks > 0)) || fail "Assets.car has no Icon Composer icon named $icon_name"
else
    [[ "$with_actool" == 0 ]] || fail "the build had actool, but Info.plist has no CFBundleIconName"
    [[ ! -e "$resources/Assets.car" ]] || fail "Assets.car is in the bundle, but Info.plist has no CFBundleIconName"
fi
echo "check-bundle-icon: $app is fine"
