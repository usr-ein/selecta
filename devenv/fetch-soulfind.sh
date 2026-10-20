#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetches the pinned Soulfind build (soulfind.pin) into devenv/.cache/soulfind/<sha256>/, checks its SHA-256, and
# prints the path of the soulfind binary. Does nothing when that build is already there. It downloads from GitHub
# only and never contacts a Soulseek server.
#
#   devenv/fetch-soulfind.sh
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
pin="$here/soulfind.pin"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) platform=macos-arm64 ;;
    Darwin-x86_64) platform=macos-x86_64 ;;
    Linux-x86_64) platform=linux-x86_64 ;;
    Linux-aarch64 | Linux-arm64) platform=linux-arm64 ;;
    *)
        echo "fetch-soulfind: no pinned Soulfind build for $(uname -s) $(uname -m)" >&2
        exit 1
        ;;
esac

entry="$(sed -n "s/^${platform}=//p" "$pin")"
asset="${entry%% *}"
sha256="${entry##* }"
if [[ -z "$entry" || -z "$asset" || -z "$sha256" || "$asset" == "$sha256" ]]; then
    echo "fetch-soulfind: no usable $platform entry in $pin" >&2
    exit 1
fi

cache="$here/.cache/soulfind"
target="$cache/$sha256"
if [[ -x "$target/soulfind" ]]; then
    echo "$target/soulfind"
    exit 0
fi

checksum() {
    if command -v shasum > /dev/null; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

scratch="${TMPDIR:-/tmp}"
download="$(mktemp -d "${scratch%/}/soulfind-download.XXXXXX")"
trap 'rm -rf "$download"' EXIT
zip="$download/soulfind.zip"
echo "fetch-soulfind: downloading Soulfind $platform (GitHub asset $asset)" >&2
if ! curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 --max-time 300 \
    --header 'Accept: application/octet-stream' --output "$zip" \
    "https://api.github.com/repos/soulfind-dev/soulfind/releases/assets/$asset"; then
    echo "fetch-soulfind: download failed. Upstream may have replaced the pinned build; see README.md" >&2
    exit 1
fi
actual="$(checksum "$zip")"
if [[ "$actual" != "$sha256" ]]; then
    echo "fetch-soulfind: checksum mismatch (expected $sha256, got $actual); nothing kept" >&2
    exit 1
fi

mkdir -p "$download/unpacked"
unzip -q -o "$zip" soulfind soulsetup -d "$download/unpacked"
chmod +x "$download/unpacked/soulfind" "$download/unpacked/soulsetup"
mkdir -p "$cache"
rm -rf "$target"
mv "$download/unpacked" "$target"
ln -sfn "$sha256" "$cache/current"
echo "$target/soulfind"
