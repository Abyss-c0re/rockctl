#!/bin/sh
# sam_record.sh — render SAM TTS to a persistent WAV (optional play).
# Used by rockctl /api/v1/sounds record action.
# Args: --out path.wav [--play] [--volume N] [--pitch N] [--speed N] [--throat N] [--mouth N] text…
set -eu

SELF=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SPEAK_HOME="${SPEAK_HOME:-/mnt/data/nanobot-wrapper/speak}"
SPEAK_ENV="${SPEAK_ENV:-/mnt/data/nanobot/speak.env}"
OUT=""
PLAY=0
VOLUME=""
PITCH=""
SPEED=""
THROAT=""
MOUTH=""

if [ -f "$SPEAK_ENV" ]; then
  set -a
  # shellcheck disable=SC1090
  . "$SPEAK_ENV"
  set +a
fi

PITCH="${SAM_PITCH:-${SPEAK_PITCH:-64}}"
SPEED="${SAM_SPEED:-${SPEAK_SPEED:-72}}"
THROAT="${SAM_THROAT:-${SPEAK_THROAT:-128}}"
MOUTH="${SAM_MOUTH:-${SPEAK_MOUTH:-128}}"
VOLUME="${SPEAK_VOLUME:-${SAM_VOLUME:-35}}"

while [ $# -gt 0 ]; do
  case "$1" in
    --out|-o) OUT=$2; shift 2 ;;
    --play|-p) PLAY=1; shift ;;
    --volume|-v) VOLUME=$2; shift 2 ;;
    --pitch) PITCH=$2; shift 2 ;;
    --speed) SPEED=$2; shift 2 ;;
    --throat) THROAT=$2; shift 2 ;;
    --mouth) MOUTH=$2; shift 2 ;;
    -h|--help) echo "usage: sam_record.sh --out file.wav [--play] [--volume 0-100] text…"; exit 0 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done

TEXT="$*"
if [ -z "$TEXT" ] && [ ! -t 0 ]; then
  TEXT=$(cat)
fi
[ -n "$OUT" ] || { echo "sam_record: need --out path.wav" >&2; exit 2; }
[ -n "$TEXT" ] || { echo "sam_record: empty text" >&2; exit 2; }

# sanitize like sam_speak
TEXT=$(printf '%s' "$TEXT" | tr '\n\t' '  ' | sed 's/  */ /g; s/^ //; s/ $//')
TEXT=$(printf '%s' "$TEXT" | sed "s/[^A-Za-z0-9 .,!?'-]//g")
[ -n "$TEXT" ] || { echo "sam_record: text empty after sanitize" >&2; exit 2; }
TEXT=$(printf '%s' "$TEXT" | cut -c1-200)

case $VOLUME in ''|*[!0-9]*) VOLUME=35 ;; esac
[ "$VOLUME" -gt 100 ] 2>/dev/null && VOLUME=100
[ "$VOLUME" -lt 0 ] 2>/dev/null && VOLUME=0

SAM_BIN="${SAM_BIN:-}"
if [ -z "$SAM_BIN" ]; then
  for b in "$SPEAK_HOME/bin/sam" /mnt/data/nanobot-wrapper/speak/bin/sam; do
    [ -x "$b" ] && SAM_BIN=$b && break
  done
fi
[ -n "${SAM_BIN:-}" ] && [ -x "$SAM_BIN" ] || { echo "sam_record: sam not found" >&2; exit 1; }

WAV_VOL="${WAV_VOL_BIN:-$SPEAK_HOME/bin/wav_vol}"
APLAY="${APLAY_BIN:-/mnt/data/audio-bin/bin/aplay}"
TMPDIR_PLAY="${SPEAK_TMPDIR:-/dev/shm}"
[ -d "$TMPDIR_PLAY" ] || TMPDIR_PLAY=/tmp

OUTDIR=$(dirname "$OUT")
mkdir -p "$OUTDIR"

TMP=$(mktemp "$TMPDIR_PLAY/samrec.XXXXXX" 2>/dev/null || mktemp /tmp/samrec.XXXXXX)
# shellcheck disable=SC2086
set -- $TEXT
if ! "$SAM_BIN" -pitch "$PITCH" -speed "$SPEED" -throat "$THROAT" -mouth "$MOUTH" \
    -wav "$TMP" "$@" >/dev/null 2>&1; then
  rm -f "$TMP"
  echo "sam_record: sam failed" >&2
  exit 1
fi
[ -s "$TMP" ] || { rm -f "$TMP"; echo "sam_record: empty wav" >&2; exit 1; }

if [ -x "$WAV_VOL" ] && [ "$VOLUME" -lt 100 ]; then
  "$WAV_VOL" "$TMP" "$VOLUME" 2>/dev/null || true
fi

cp -f "$TMP" "$OUT"
rm -f "$TMP"
chmod 644 "$OUT" 2>/dev/null || true

if [ "$PLAY" = 1 ] && [ -x "$APLAY" ]; then
  export HOME="${HOME:-/mnt/data/rockctl}"
  if [ -d /mnt/data/audio-bin/lib ]; then
    export LD_LIBRARY_PATH="/mnt/data/audio-bin/lib:${LD_LIBRARY_PATH:-}"
  fi
  SPEAK_ALSA_DEVICE="${SPEAK_ALSA_DEVICE:-clanker}"
  "$APLAY" -D "$SPEAK_ALSA_DEVICE" "$OUT" >/dev/null 2>&1 \
    || "$APLAY" "$OUT" >/dev/null 2>&1 \
    || true
fi

echo "ok out=$OUT bytes=$(wc -c < "$OUT" | tr -cd '0-9')"
exit 0
