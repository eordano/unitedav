#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Fuzz seed-corpus generator.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
MEDIA_OUT="${UAV_MEDIA_DIR:-$REPO_DIR/tests/media/out}"
CORPUS_ROOT="$SCRIPT_DIR/corpus"

# Corpus dir names must equal the NAME_WE of the matching fuzz target sources.
DECODE_TARGETS=(fuzz_open fuzz_decode_url)

mkdir -p "$CORPUS_ROOT"

if [ ! -d "$MEDIA_OUT" ] || [ -z "$(ls -A "$MEDIA_OUT" 2>/dev/null)" ]; then
  echo "gen-corpus: media dir empty -> running tests/media/gen.sh"
  bash "$REPO_DIR/tests/media/gen.sh" || {
    echo "gen-corpus: WARN media generation failed; will still emit committed seeds" >&2
  }
fi

# Count visible entries in a dir (matches `ls -1 | grep -c .`: excludes dotfiles).
count_entries() {
  local dir="$1" n=0 e
  shopt -s nullglob
  for e in "$dir"/*; do
    [ -e "$e" ] && n=$((n + 1))
  done
  shopt -u nullglob
  echo "$n"
}

byte_poke() {
  local file="$1" off="$2" val="$3"
  printf "$(printf '\\x%02x' "$val")" \
    | dd of="$file" bs=1 seek="$off" count=1 conv=notrunc status=none 2>/dev/null
}

emit_decode_seeds() {
  local clip="$1" target_dir="$2"
  local base; base="$(basename "$clip")"
  local stem="${base%.*}"

  cp -f "$clip" "$target_dir/clip-$stem.bin"

  local n
  for n in 1024 4096; do
    dd if="$clip" of="$target_dir/seed-trunc${n}-$stem.bin" \
       bs="$n" count=1 status=none 2>/dev/null
  done

  local pfx="$target_dir/seed-flip-$stem.bin"
  dd if="$clip" of="$pfx" bs=4096 count=1 status=none 2>/dev/null
  if [ -s "$pfx" ]; then
    local cur
    cur="$(dd if="$pfx" bs=1 skip=32 count=1 status=none 2>/dev/null | od -An -tu1 | tr -d ' \n')"
    [ -n "$cur" ] && byte_poke "$pfx" 32 $(( (cur ^ 0x01) & 0xff ))
  fi
}

for t in "${DECODE_TARGETS[@]}"; do
  td="$CORPUS_ROOT/$t"
  mkdir -p "$td"
  : > "$td/.gitkeep" 2>/dev/null || true
  if [ -d "$MEDIA_OUT" ]; then
    shopt -s nullglob
    for clip in "$MEDIA_OUT"/*; do
      [ -f "$clip" ] || continue
      emit_decode_seeds "$clip" "$td"
    done
    shopt -u nullglob
  fi
  echo "gen-corpus: $t -> $(count_entries "$td") entries"
done

URL_DIR="$CORPUS_ROOT/fuzz_url"
mkdir -p "$URL_DIR"
: > "$URL_DIR/.gitkeep" 2>/dev/null || true

write_url_seed() {
  local out="$1" sel="$2" portb="$3" leaf="$4"
  printf "$(printf '\\x%02x' "$sel")$(printf '\\x%02x' "$portb")" > "$out"
  printf '%s' "$leaf" >> "$out"
}
write_url_seed "$URL_DIR/seed-url-path.bin"  0 7  "media.mp4"
write_url_seed "$URL_DIR/seed-url-file.bin"  1 7  "clip.webm"
write_url_seed "$URL_DIR/seed-url-sdp.bin"   2 7  "stream"
write_url_seed "$URL_DIR/seed-url-rtp.bin"   3 23 "ssrc=1"
write_url_seed "$URL_DIR/seed-url-rtsp.bin"  4 23 "live"
echo "gen-corpus: fuzz_url -> $(count_entries "$URL_DIR") entries"

SEND_DIR="$CORPUS_ROOT/fuzz_send_config"
mkdir -p "$SEND_DIR"
: > "$SEND_DIR/.gitkeep" 2>/dev/null || true

write_cfg() {
  local out="$1"; shift
  : > "$out"
  local v
  for v in "$@"; do
    printf "$(printf '\\x%02x' $(( v        & 0xff )))$(printf '\\x%02x' $(( (v>>8)  & 0xff )))$(printf '\\x%02x' $(( (v>>16) & 0xff )))$(printf '\\x%02x' $(( (v>>24) & 0xff )))" >> "$out"
  done
}

# LE u32 fields, in fuzz_send_config.cpp take_u32 offset order:
#   video_codec audio_codec width height fps vbitrate srate channels abitrate strpad strflag
write_cfg "$SEND_DIR/seed-cfg-vp8-opus.bin"   1 1 64 64 30 300000 0 2 96000 0 0
write_cfg "$SEND_DIR/seed-cfg-vp9-opus.bin"   2 1 32 32 24 200000 1 1 64000 16 1
write_cfg "$SEND_DIR/seed-cfg-audio-only.bin" 0 1 0  0  0  0      0 2 96000 0 0
write_cfg "$SEND_DIR/seed-cfg-badvcodec.bin"  4 1 16 16 60 0      0 2 0     0 0
echo "gen-corpus: fuzz_send_config -> $(count_entries "$SEND_DIR") entries"

echo "gen-corpus: done. Corpus root: $CORPUS_ROOT"
