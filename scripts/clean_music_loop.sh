#!/bin/sh
# Clean-music watchdog — multi-track, once/loop/shuffle (lean, offline).
#
# When-to-play (music_mode):
#   off | clean | on
# How-to-play (play_mode):
#   once    — play current track once, then stop until next "want music" edge / play action
#   loop    — cycle playlist in order (single track loops)
#   shuffle — random next (avoids same if ≥2 tracks)
#
# Tracks: /mnt/data/rockctl/music/*.wav
# Fallback: CLEAN_MUSIC_FILE or /mnt/data/clean_loop_full.wav
# State:  music_mode, play_mode, current (basename)

ROOT="${ROCKCTL_HOME:-/mnt/data/rockctl}"
MUSIC_DIR="${ROOT}/music"
MODE_FILE="${ROOT}/music_mode"
PLAY_MODE_FILE="${ROOT}/play_mode"
CURRENT_FILE="${ROOT}/current"
FALLBACK="${CLEAN_MUSIC_FILE:-/mnt/data/clean_loop_full.wav}"
APLAY="${CLEAN_MUSIC_APLAY:-/mnt/data/audio-bin/bin/aplay}"
export LD_LIBRARY_PATH=/mnt/data/audio-bin/lib:/usr/lib/arm-linux-gnueabihf:${LD_LIBRARY_PATH:-}
LOG=/mnt/data/clean_music.log
PIDFILE=/mnt/data/clean_music.pid
APLAY_PIDFILE=/mnt/data/clean_music_aplay.pid
STATUS_URL="${ROCKCTL_STATUS_URL:-http://127.0.0.1:8080/api/v1/status}"
POLL_PLAY=1
POLL_IDLE=1
BUSY_BACKOFF=2
EMPTY_TOLERANCE=8
ONCE_DONE_FLAG=/tmp/clanker_music_once_done

log() { echo "$(date 2>/dev/null || echo '?') $*" >> "$LOG"; }

read_mode() {
  m=$(cat "$MODE_FILE" 2>/dev/null | tr -d ' \t\r\n' | tr 'A-Z' 'a-z')
  case "$m" in
    off|0|false|no) echo off ;;
    on|always|1|true|yes|force) echo on ;;
    clean|auto|while|cleaning|"") echo clean ;;
    *) echo clean ;;
  esac
}

read_play_mode() {
  m=$(cat "$PLAY_MODE_FILE" 2>/dev/null | tr -d ' \t\r\n' | tr 'A-Z' 'a-z')
  case "$m" in
    once|one|single) echo once ;;
    shuffle|random) echo shuffle ;;
    loop|repeat|"" ) echo loop ;;
    *) echo loop ;;
  esac
}

# Build list of tracks into global TRACKS (newline separated) and NTRACKS
list_tracks() {
  TRACKS=""
  NTRACKS=0
  if [ -d "$MUSIC_DIR" ]; then
    for f in "$MUSIC_DIR"/*.wav "$MUSIC_DIR"/*.WAV; do
      [ -f "$f" ] || continue
      TRACKS="${TRACKS}${f}
"
      NTRACKS=$((NTRACKS + 1))
    done
  fi
  if [ "$NTRACKS" -eq 0 ] && [ -f "$FALLBACK" ]; then
    TRACKS="$FALLBACK
"
    NTRACKS=1
  fi
}

track_at() {
  # 1-based index
  i="$1"
  n=0
  echo "$TRACKS" | while read -r t; do
    [ -n "$t" ] || continue
    n=$((n + 1))
    if [ "$n" -eq "$i" ]; then echo "$t"; break; fi
  done
}

index_of() {
  want="$1"
  n=0
  echo "$TRACKS" | while read -r t; do
    [ -n "$t" ] || continue
    n=$((n + 1))
    bn=$(basename "$t")
    if [ "$t" = "$want" ] || [ "$bn" = "$want" ]; then echo "$n"; break; fi
  done
}

pick_next() {
  pm=$(read_play_mode)
  list_tracks
  [ "$NTRACKS" -gt 0 ] || { echo ""; return 1; }
  cur=$(cat "$CURRENT_FILE" 2>/dev/null)
  idx=$(index_of "$cur")
  [ -n "$idx" ] || idx=0
  case "$pm" in
    once)
      # same track again only on explicit restart; picker returns current if set else first
      if [ -n "$cur" ] && [ -f "$cur" ]; then echo "$cur"; return 0; fi
      track_at 1
      ;;
    shuffle)
      if [ "$NTRACKS" -eq 1 ]; then track_at 1; return 0; fi
      # random 1..NTRACKS avoid same index if possible
      r=$(awk 'BEGIN{srand(); print int(rand()*'"$NTRACKS"')+1}')
      if [ -n "$idx" ] && [ "$r" -eq "$idx" ]; then
        r=$(( (r % NTRACKS) + 1 ))
      fi
      track_at "$r"
      ;;
    loop|*)
      if [ -z "$idx" ] || [ "$idx" -ge "$NTRACKS" ]; then
        track_at 1
      else
        track_at $((idx + 1))
      fi
      ;;
  esac
}

resolve_current() {
  list_tracks
  [ "$NTRACKS" -gt 0 ] || { echo ""; return 1; }
  cur=$(cat "$CURRENT_FILE" 2>/dev/null)
  if [ -n "$cur" ] && [ -f "$cur" ]; then echo "$cur"; return 0; fi
  # basename only
  if [ -n "$cur" ] && [ -f "$MUSIC_DIR/$cur" ]; then echo "$MUSIC_DIR/$cur"; return 0; fi
  t=$(track_at 1)
  echo "$t"
  [ -n "$t" ]
}

get_state() {
  body=$(wget -q -O - -T 3 "$STATUS_URL" 2>/dev/null) || body=""
  [ -n "$body" ] || { echo ""; return 1; }
  inc=$(echo "$body" | sed -n 's/.*"in_cleaning"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1)
  st=$(echo "$body" | sed -n 's/.*"state"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1)
  if [ -n "$st" ]; then
    echo "$st:$inc"
    return 0
  fi
  echo ""
  return 1
}

# Actively cleaning only (music mode=clean).
# Pause (10) keeps in_cleaning=1 on many firmwares — do NOT treat as active.
# Returning/docking/charging/idle must stop music.
is_cleaning_state() {
  st="$1"; inc="$2"
  case "$st" in
    10|6|8|3|15|2|9|12|22|1|7|16) return 1 ;;  # pause/return/charge/idle/dock/manual/goto…
  esac
  case "$st" in
    5|11|17|18) return 0 ;;  # clean / spot / zoned / segment
  esac
  # Do not trust in_cleaning alone (pause / edge states).
  return 1
}

# Speak/TTS busy flag — never touch aplay while SAM is talking
SPEAK_BUSY_FLAG="${SPEAK_BUSY_FLAG:-/dev/shm/sam_speak.busy}"

speak_busy() {
  [ -f "$SPEAK_BUSY_FLAG" ] || [ -d /dev/shm/sam_speak.lock ]
}

stop_aplay() {
  # Only stop *our* music aplay. Never mass-kill all aplay — that cut off SAM TTS.
  if speak_busy; then
    return 0
  fi
  if [ -f "$APLAY_PIDFILE" ]; then
    ap=$(cat "$APLAY_PIDFILE" 2>/dev/null)
    [ -n "$ap" ] && kill "$ap" 2>/dev/null
    [ -n "$ap" ] && kill -9 "$ap" 2>/dev/null
    rm -f "$APLAY_PIDFILE"
  fi
}

aplay_alive() {
  [ -f "$APLAY_PIDFILE" ] || return 1
  ap=$(cat "$APLAY_PIDFILE" 2>/dev/null)
  [ -n "$ap" ] || return 1
  kill -0 "$ap" 2>/dev/null
}

# ALSA device: prefer software mix so OUCH/TTS can overlay.
# Uses /mnt/data/rockctl/asound.conf pcm "clanker" (plug→dmix) when present.
music_aplay_dev() {
  # $HOME/.asoundrc (HOME=$ROOT) defines pcm.clanker for dmix overlay
  if [ -f "$ROOT/.asoundrc" ] || [ -f "$ROOT/asound.rc" ]; then
    echo clanker
    return
  fi
  echo hw:0,0
}

start_aplay_file() {
  f="$1"
  speak_busy && return 0
  [ -x "$APLAY" ] || { log "no aplay at $APLAY"; return 1; }
  [ -f "$f" ] || { log "no file $f"; return 1; }
  aplay_alive && return 0
  stop_aplay
  rm -f "$APLAY_PIDFILE"
  basename "$f" > "$CURRENT_FILE" 2>/dev/null || echo "$f" > "$CURRENT_FILE"
  # also store full path for reliability
  echo "$f" > "${CURRENT_FILE}.path"
  DEV=$(music_aplay_dev)
  HOME="$ROOT" "$APLAY" -D "$DEV" "$f" >>"$LOG" 2>&1 &
  ap=$!
  echo "$ap" > "$APLAY_PIDFILE"
  sleep 1
  if kill -0 "$ap" 2>/dev/null; then
    log "aplay start pid=$ap dev=$DEV file=$(basename "$f")"
    return 0
  fi
  # Do not fall back to exclusive hw:0,0 — that blocks SAM TTS / OUCH dmix.
  # Retry same soft device once after a short backoff.
  log "aplay died on $DEV — retry $DEV (no exclusive hw)"
  rm -f "$APLAY_PIDFILE"
  sleep "$BUSY_BACKOFF"
  stop_aplay
  HOME="$ROOT" "$APLAY" -D "$DEV" "$f" >>"$LOG" 2>&1 &
  ap=$!
  echo "$ap" > "$APLAY_PIDFILE"
  sleep 1
  kill -0 "$ap" 2>/dev/null && { log "aplay retry ok pid=$ap"; return 0; }
  log "aplay failed on $DEV"
  rm -f "$APLAY_PIDFILE"
  return 1
}

ensure_playing() {
  speak_busy && return 0
  pm=$(read_play_mode)
  # once mode: if we already finished a full play for this "session", stay silent
  if [ "$pm" = "once" ] && [ -f "$ONCE_DONE_FLAG" ]; then
    return 0
  fi
  if aplay_alive; then return 0; fi
  # track ended
  if [ "$pm" = "once" ]; then
    # was playing and died → mark done
    if [ "$was_playing" = "1" ]; then
      touch "$ONCE_DONE_FLAG"
      log "once: track finished — stop until next session"
      was_playing=0
      return 0
    fi
    f=$(resolve_current) || return 1
    start_aplay_file "$f" && was_playing=1
    return $?
  fi
  # loop / shuffle: pick next and play
  f=$(pick_next) || f=$(resolve_current) || return 1
  start_aplay_file "$f" && was_playing=1
}

mkdir -p "$ROOT" "$MUSIC_DIR"

# disk hygiene (maps/miio/music kept)
if [ -x /mnt/data/rockctl/bin/autowipe.sh ]; then
  /mnt/data/rockctl/bin/autowipe.sh >/dev/null 2>&1 || true
fi
# re-run autowipe about every ~1h of watchdog time
AW_TICKS=0
[ -f "$MODE_FILE" ] || echo clean > "$MODE_FILE"
[ -f "$PLAY_MODE_FILE" ] || echo loop > "$PLAY_MODE_FILE"

# seed library from legacy files once
if [ ! "$(ls -A "$MUSIC_DIR" 2>/dev/null)" ]; then
  [ -f /mnt/data/clean_loop_full.wav ] && cp -n /mnt/data/clean_loop_full.wav "$MUSIC_DIR/clean_loop_full.wav" 2>/dev/null
  [ -f /mnt/data/clean_loop_30s.wav ] && cp -n /mnt/data/clean_loop_30s.wav "$MUSIC_DIR/clean_loop_30s.wav" 2>/dev/null
fi

if [ -f "$PIDFILE" ]; then
  old=$(cat "$PIDFILE" 2>/dev/null)
  if [ -n "$old" ] && [ "$old" != "$$" ] && kill -0 "$old" 2>/dev/null; then
    cmd=$(tr '\0' ' ' < /proc/"$old"/cmdline 2>/dev/null || true)
    case "$cmd" in
      *clean_music*) log "watchdog already live pid=$old"; exit 0 ;;
    esac
  fi
fi
echo $$ > "$PIDFILE"
trap 'stop_aplay; rm -f "$PIDFILE"; log "watchdog exit"; exit 0' INT TERM
log "watchdog start dir=$MUSIC_DIR mode_file=$MODE_FILE play_mode_file=$PLAY_MODE_FILE"

last_mode=""
last_st=""
empty_streak=0
want_music=0
was_playing=0
prev_want=0

while true; do
  AW_TICKS=$((AW_TICKS + 1))
  # ~ every 3600 poll seconds (~1h at 1s poll)
  if [ "$AW_TICKS" -ge 3600 ] 2>/dev/null; then
    AW_TICKS=0
    [ -x /mnt/data/rockctl/bin/autowipe.sh ] && /mnt/data/rockctl/bin/autowipe.sh >/dev/null 2>&1 || true
  fi
  mode=$(read_mode)
  if [ "$mode" != "$last_mode" ]; then
    log "mode=$mode play=$(read_play_mode)"
    last_mode=$mode
    rm -f "$ONCE_DONE_FLAG"
    was_playing=0
  fi

  # Yield entirely while SAM / voice is speaking
  if speak_busy; then
    sleep "$POLL_IDLE"
    continue
  fi

  if [ "$mode" = "off" ]; then
    stop_aplay
    want_music=0
    was_playing=0
    sleep "$POLL_IDLE"
    continue
  fi

  if [ "$mode" = "on" ]; then
    want_music=1
    if [ "$prev_want" = "0" ]; then rm -f "$ONCE_DONE_FLAG"; was_playing=0; fi
    ensure_playing || true
    prev_want=1
    sleep "$POLL_PLAY"
    continue
  fi

  # clean mode
  raw=$(get_state) || raw=""
  if [ -z "$raw" ]; then
    empty_streak=$((empty_streak + 1))
    if [ "$empty_streak" -ge "$EMPTY_TOLERANCE" ] && [ "$want_music" = "1" ]; then
      log "status empty — stop"
      stop_aplay
      want_music=0
      was_playing=0
    fi
    sleep "$POLL_IDLE"
    continue
  fi
  empty_streak=0
  st=$(echo "$raw" | cut -d: -f1)
  inc=$(echo "$raw" | cut -d: -f2)
  if [ "$st" != "$last_st" ]; then log "state=$st in_cleaning=${inc:-?}"; last_st=$st; fi

  if is_cleaning_state "$st" "$inc"; then
    want_music=1
    if [ "$prev_want" = "0" ]; then rm -f "$ONCE_DONE_FLAG"; was_playing=0; fi
    ensure_playing || true
    prev_want=1
    sleep "$POLL_PLAY"
  else
    if [ "$want_music" = "1" ] || aplay_alive; then
      case "$st" in
        10) log "paused — stop music" ;;
        6)  log "returning — stop music" ;;
        15) log "docking — stop music" ;;
        *)  log "not cleaning (state=$st) — stop music" ;;
      esac
      stop_aplay
    fi
    want_music=0
    was_playing=0
    prev_want=0
    rm -f "$ONCE_DONE_FLAG"
    sleep "$POLL_IDLE"
  fi
done
