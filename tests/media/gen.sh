#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Synthetic test-media generator: builds the format/codec/transport decode matrix.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

DUR="${UAV_GEN_DUR:-5}"
SIZE="${UAV_GEN_SIZE:-320x240}"
RATE="${UAV_GEN_RATE:-25}"
VAAPI_DEV="${UAV_VAAPI_DEV:-/dev/dri/renderD128}"

FF=(ffmpeg -hide_banner -loglevel error -y)
SRC=(-f lavfi -i "testsrc2=size=${SIZE}:rate=${RATE}" -f lavfi -i "sine=frequency=440:sample_rate=48000")
COMMON=(-t "$DUR")

# Tag BT.601/limited so the oracle's contract mode passes (untagged decodes as
# UNSPECIFIED). Placed BEFORE codec args so per-call overrides win.
CTAG=(-color_primaries smpte170m -color_trc smpte170m -colorspace smpte170m -color_range tv)

HAVE_VAAPI=0
if [ -e "$VAAPI_DEV" ] && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_vaapi; then
  if ffmpeg -hide_banner -loglevel error -vaapi_device "$VAAPI_DEV" \
      -f lavfi -i "testsrc2=size=64x64:rate=5" -t 0.4 \
      -vf 'format=nv12,hwupload' -c:v h264_vaapi -f null - >/dev/null 2>&1; then
    HAVE_VAAPI=1
  fi
fi
HAVE_VT=0
if [ "$HAVE_VAAPI" = 0 ] \
   && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_videotoolbox \
   && ffmpeg -hide_banner -encoders 2>/dev/null | grep -q hevc_videotoolbox; then
  if ffmpeg -hide_banner -loglevel error \
      -f lavfi -i "testsrc2=size=64x64:rate=5" -t 0.4 \
      -c:v h264_videotoolbox -f null - >/dev/null 2>&1; then
    HAVE_VT=1
  fi
fi
echo "gen: dur=${DUR}s size=${SIZE} rate=${RATE} vaapi_hw_encode=$([ $HAVE_VAAPI = 1 ] && echo yes || echo no) videotoolbox_hw_encode=$([ $HAVE_VT = 1 ] && echo yes || echo no)"

PASS=0; SKIP=0; FAIL=0

verify_colorimetry() {
  local file="$1"
  command -v ffprobe >/dev/null 2>&1 || return 0
  local cs rng
  cs="$(ffprobe -hide_banner -loglevel error -select_streams v:0 -read_intervals '%+#1' \
        -show_entries frame=color_space -of default=nokey=1:noprint_wrappers=1 "$file" 2>/dev/null | head -1)"
  [ -z "$cs" ] && return 0
  rng="$(ffprobe -hide_banner -loglevel error -select_streams v:0 -read_intervals '%+#1' \
         -show_entries frame=color_range -of default=nokey=1:noprint_wrappers=1 "$file" 2>/dev/null | head -1)"
  case "$cs" in
    smpte170m|bt470bg) ;;
    *) printf '    colorimetry WARN: colorspace=%s (want smpte170m/bt470bg = BT.601)\n' "$cs"; return 1 ;;
  esac
  case "$rng" in
    tv|mpeg|limited) ;;
    *) printf '    colorimetry WARN: color_range=%s (want tv/limited)\n' "$rng"; return 1 ;;
  esac
  return 0
}

emit() {
  local out="$1"; shift
  if "${FF[@]}" "${SRC[@]}" "${COMMON[@]}" "${CTAG[@]}" "$@" "$OUT/$out" 2>/tmp/uav_gen_err; then
    if [ -s "$OUT/$out" ]; then
      if verify_colorimetry "$OUT/$out"; then
        printf '  OK    %s\n' "$out"
      else
        printf '  OK*   %s  (colorimetry assertion warned above)\n' "$out"
      fi
      PASS=$((PASS+1)); return 0
    fi
  fi
  printf '  FAIL  %s\n    %s\n' "$out" "$(tail -1 /tmp/uav_gen_err)"; FAIL=$((FAIL+1)); return 1
}

emit_audio() {
  local out="$1"; shift
  if "${FF[@]}" "${SRC[@]}" "${COMMON[@]}" "$@" "$OUT/$out" 2>/tmp/uav_gen_err; then
    if [ -s "$OUT/$out" ]; then
      printf '  OK    %s\n' "$out"; PASS=$((PASS+1)); return 0
    fi
  fi
  printf '  FAIL  %s\n    %s\n' "$out" "$(tail -1 /tmp/uav_gen_err)"; FAIL=$((FAIL+1)); return 1
}

skip() { printf '  SKIP  %s  (%s)\n' "$1" "$2"; SKIP=$((SKIP+1)); }

VP8=(-c:v libvpx -b:v 600k -deadline realtime -cpu-used 8)
VP9=(-c:v libvpx-vp9 -b:v 600k -deadline realtime -cpu-used 8 -row-mt 1)
AV1=(-c:v libsvtav1 -preset 12 -crf 50 -g 30)
AV1_10=(-pix_fmt yuv420p10le -c:v libsvtav1 -preset 12 -crf 50 -g 30)

h264_vaapi_args=(-vaapi_device "$VAAPI_DEV" -vf 'format=nv12,hwupload' -c:v h264_vaapi)
hevc_vaapi_args=(-vaapi_device "$VAAPI_DEV" -vf 'format=nv12,hwupload' -c:v hevc_vaapi)

h264_vt_args=(-c:v h264_videotoolbox -b:v 2M)
hevc_vt_args=(-c:v hevc_videotoolbox -b:v 2M)

OPUS=(-c:a libopus -b:a 96k)
VORBIS=(-c:a libvorbis -q:a 4)
AAC=(-c:a aac -b:a 128k)
MP3=(-c:a libmp3lame -b:a 128k)
FLAC=(-c:a flac)

echo "== matrix: container x video x audio =="

emit "webm__vp9__opus.webm"   "${VP9[@]}"   "${OPUS[@]}"
emit "webm__vp8__vorbis.webm" "${VP8[@]}"   "${VORBIS[@]}"
emit "webm__av1__opus.webm"   "${AV1[@]}"   "${OPUS[@]}"

emit "mkv__vp9__flac.mkv"     "${VP9[@]}"   "${FLAC[@]}"
emit "mkv__av1__opus.mkv"     "${AV1[@]}"   "${OPUS[@]}"
emit "mkv__av1_10__opus.mkv"  "${AV1_10[@]}" "${OPUS[@]}"

emit "mp4__av1__aac.mp4"      "${AV1[@]}"   "${AAC[@]}"  -movflags +faststart+write_colr
if [ "$HAVE_VAAPI" = 1 ]; then
  emit "mp4__h264__aac.mp4"   "${h264_vaapi_args[@]}" "${AAC[@]}" -movflags +faststart+write_colr
  emit "mp4__hevc__aac.mp4"   "${hevc_vaapi_args[@]}" -tag:v hvc1 "${AAC[@]}" -movflags +faststart+write_colr
elif [ "$HAVE_VT" = 1 ]; then
  emit "mp4__h264__aac.mp4"   "${h264_vt_args[@]}" "${AAC[@]}" -movflags +faststart+write_colr
  emit "mp4__hevc__aac.mp4"   "${hevc_vt_args[@]}" -tag:v hvc1 "${AAC[@]}" -movflags +faststart+write_colr
else
  skip "mp4__h264__aac.mp4" "no VAAPI/VideoToolbox HW encoder"
  skip "mp4__hevc__aac.mp4" "no VAAPI/VideoToolbox HW encoder"
fi

if [ "$HAVE_VAAPI" = 1 ]; then
  emit "mov__h264__aac.mov"   "${h264_vaapi_args[@]}" -movflags +write_colr "${AAC[@]}"
  emit "mov__hevc__aac.mov"   "${hevc_vaapi_args[@]}" -tag:v hvc1 -movflags +write_colr "${AAC[@]}"
elif [ "$HAVE_VT" = 1 ]; then
  emit "mov__h264__aac.mov"   "${h264_vt_args[@]}" -movflags +write_colr "${AAC[@]}"
  emit "mov__hevc__aac.mov"   "${hevc_vt_args[@]}" -tag:v hvc1 -movflags +write_colr "${AAC[@]}"
else
  emit "mov__prores__aac.mov" -c:v prores_ks -profile:v 0 -movflags +write_colr "${AAC[@]}"
fi

if [ "$HAVE_VAAPI" = 1 ]; then
  emit "mpegts__h264__mp3.ts"  "${h264_vaapi_args[@]}" "${MP3[@]}"
  emit "mpegts__hevc__aac.ts"  "${hevc_vaapi_args[@]}" "${AAC[@]}"
elif [ "$HAVE_VT" = 1 ]; then
  emit "mpegts__h264__mp3.ts"  "${h264_vt_args[@]}" "${MP3[@]}"
  emit "mpegts__hevc__aac.ts"  "${hevc_vt_args[@]}" "${AAC[@]}"
else
  skip "mpegts__h264__mp3.ts" "no VAAPI/VideoToolbox HW encoder"
  skip "mpegts__hevc__aac.ts" "no VAAPI/VideoToolbox HW encoder"
fi

emit_audio "ogg__novideo__vorbis.ogg" -map 1:a -vn "${VORBIS[@]}"
emit_audio "ogg__novideo__opus.ogg"   -map 1:a -vn "${OPUS[@]}"

echo "== done: PASS=$PASS SKIP=$SKIP FAIL=$FAIL =="
echo "files in: $OUT"
ls -1 "$OUT" 2>/dev/null
[ "$FAIL" -eq 0 ]
