#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

URL="${UAV_CC_URL:-https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_320x180.mp4}"
DEST="$OUT/cc__bigbuckbunny.mp4"
PINNED_SHA="${UAV_CC_SHA256:-}"

echo "fetch-cc: trying $URL (best-effort)"
if ! command -v curl >/dev/null 2>&1; then
  echo "fetch-cc: no curl -> skipping (synthetic media is sufficient)"; exit 0
fi

if ! curl -fsSL --connect-timeout 8 --max-time 120 -o "$DEST.part" "$URL"; then
  echo "fetch-cc: download failed/offline -> skipping (synthetic media is sufficient)"
  rm -f "$DEST.part"; exit 0
fi

GOT_SHA="$(sha256sum "$DEST.part" | awk '{print $1}')"
if [ -n "$PINNED_SHA" ] && [ "$GOT_SHA" != "$PINNED_SHA" ]; then
  echo "fetch-cc: checksum MISMATCH (got $GOT_SHA, want $PINNED_SHA) -> discarding"
  rm -f "$DEST.part"; exit 0
fi

mv "$DEST.part" "$DEST"
echo "fetch-cc: saved $DEST"
echo "fetch-cc: sha256=$GOT_SHA"
[ -z "$PINNED_SHA" ] && echo "fetch-cc: (no pin set; record this sha in ATTRIBUTION.md to pin it)"
echo "fetch-cc: CC-BY 3.0 (c) Blender Foundation - see ATTRIBUTION.md"
