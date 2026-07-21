#!/bin/sh
# ouch_watcher — OUCH on real bumper / dock light-touch hits.
#
# Docked: MCU enables "LightTouch"; physical press logs Sent Bumper Val:N (N!=0)
#          or ProcessBumper L/R=1 — NOT Enable/Disable alone, NOT Val:0.
# Cleaning: same bumper lines when it hits walls.
# Ignores trap/carpet/FanInfo/WheelWarn noise that previously false-fired.
#
# Overlay music via ALSA pcm "clanker" (dmix). Never kills music aplay.

ROOT="${ROCKCTL_HOME:-/mnt/data/rockctl}"
LOG="${OUCH_LOG:-$ROOT/ouch_watcher.log}"
PIDFILE="${OUCH_PIDFILE:-$ROOT/ouch_watcher.pid}"
STATUS_URL="${ROCKCTL_STATUS_URL:-http://127.0.0.1:8080/api/v1/status}"
WAV="${OUCH_WAV:-$ROOT/sounds/ouch.wav}"
APLAY="${OUCH_APLAY:-/mnt/data/audio-bin/bin/aplay}"
export LD_LIBRARY_PATH="/mnt/data/audio-bin/lib:/usr/lib/arm-linux-gnueabihf:${LD_LIBRARY_PATH:-}"
ENABLED_FILE="${OUCH_ENABLED_FILE:-$ROOT/ouch_enabled}"
POLL_MS="${OUCH_POLL_MS:-200}"
COOLDOWN_MS="${OUCH_COOLDOWN_MS:-900}"
MCU_LOG="${OUCH_MCU_LOG:-/run/shm/MCU_normal.log}"
EVT_LOG="${OUCH_EVT_LOG:-/run/shm/EVENTTASK_normal.log}"

log() {
  echo "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?') $*" >> "$LOG" 2>/dev/null
  sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
  if [ "$sz" -gt 49152 ] 2>/dev/null; then
    tail -c 12288 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
  fi
}

is_enabled() {
  if [ "${OUCH_ENABLED:-}" = "0" ] || [ "${OUCH_ENABLED:-}" = "off" ]; then return 1; fi
  if [ -f "$ENABLED_FILE" ]; then
    v=$(cat "$ENABLED_FILE" 2>/dev/null | tr -d ' \t\r\n' | tr 'A-Z' 'a-z')
    case "$v" in 0|off|false|no) return 1 ;; esac
  fi
  return 0
}

now_ms() {
  # integer seconds only (fractional /proc/uptime padding caused bad deltas)
  if [ -r /proc/uptime ]; then
    u=$(cut -d. -f1 /proc/uptime)
    case "$u" in ''|*[!0-9]*) u=$(date +%s 2>/dev/null || echo 0) ;; esac
    echo $((u * 1000))
    return
  fi
  echo $(($(date +%s) * 1000))
}

sleep_ms() {
  ms="$1"
  if sleep "$(awk "BEGIN{printf \"%.3f\", $ms/1000}")" 2>/dev/null; then return; fi
  s=$((ms / 1000)); [ "$s" -lt 1 ] && s=1; sleep "$s"
}

fetch_status() {
  if command -v wget >/dev/null 2>&1; then
    wget -q -T 2 -O- "$STATUS_URL" 2>/dev/null; return
  fi
  if command -v curl >/dev/null 2>&1; then
    curl -sS -m 2 "$STATUS_URL" 2>/dev/null; return
  fi
  echo ""
}

parse_bumpers() {
  body="$1"
  case "$body" in
    *adbumper_status*)
      rest=${body#*adbumper_status}
      rest=${rest#*[}
      rest=${rest%%]*}
      echo "$rest" | tr ',' ' ' | tr -cd '0-9 -'
      ;;
    *) echo "" ;;
  esac
}

bump_active() {
  for t in $1; do
    [ -n "$t" ] || continue
    [ "$t" != "0" ] && return 0
  done
  return 1
}

# Strict hit detection — only real contact signals
# Returns 0 if hit found; prints reason token to stdout via global HIT_REASON
HIT_REASON=""
chunk_is_hit() {
  ch="$1"
  HIT_REASON=""
  [ -n "$ch" ] || return 1

  # Non-zero bumper value (dock light-touch + driving bumps)
  # e.g. "Sent Bumper Val:1" or "Sent Bumper Val:3"
  if echo "$ch" | grep -qE 'Sent Bumper Val:[1-9][0-9]*'; then
    HIT_REASON="Sent Bumper Val nonzero"
    return 0
  fi

  # ProcessBumper with a side asserted
  if echo "$ch" | grep -qE 'ProcessBumper[^[:alnum:]]*(L=1|R=1|L=[1-9]|R=[1-9])'; then
    HIT_REASON="ProcessBumper side=1"
    return 0
  fi
  if echo "$ch" | grep -qE 'ProcessBumper.*(true|pressed|hit)'; then
    HIT_REASON="ProcessBumper pressed"
    return 0
  fi

  # Light-touch press events (not Enable/Disable mode toggles)
  if echo "$ch" | grep -qiE 'LightTouch[[:space:]]+(Hit|Press|Pressed|Trigger|Detect|Active|On[^a-z]|Event)'; then
    HIT_REASON="LightTouch press"
    return 0
  fi
  # Some firmwares log "LightTouch!" or "light_touch=1"
  if echo "$ch" | grep -qiE 'light[_ ]?touch[=:][1-9]|LightTouch!'; then
    HIT_REASON="LightTouch flag"
    return 0
  fi

  # Explicit wall hit language
  if echo "$ch" | grep -qiE 'HitWall|hit_wall|collision_detect|BumpPressed'; then
    HIT_REASON="wall/collision keyword"
    return 0
  fi

  # BUMPER ACKED only counts if same chunk also has nonzero Val or ProcessBumper L/R
  # (Val:0 + ACKED is dock chatter — ignore)

  return 1
}

read_new() {
  path="$1"
  off_file="$2"
  [ -f "$path" ] || return 1
  sz=$(wc -c < "$path" 2>/dev/null | tr -cd '0-9')
  [ -n "$sz" ] || sz=0
  off=$(cat "$off_file" 2>/dev/null | tr -cd '0-9')
  [ -n "$off" ] || off=0
  if [ "$sz" -lt "$off" ]; then off=0; fi
  if [ "$sz" -eq "$off" ]; then return 1; fi
  if [ ! -f "$off_file" ]; then
    echo "$sz" > "$off_file"
    return 1
  fi
  sz=${sz:-0}; off=${off:-0}
  len=$((sz - off))
  if [ "$len" -lt 0 ] 2>/dev/null; then len=0; fi
  if [ "$len" -gt 16384 ] 2>/dev/null; then
    off=$((sz - 16384))
    len=16384
  fi
  [ "$len" -gt 0 ] 2>/dev/null || return 1
  dd if="$path" bs=1 skip="$off" count="$len" 2>/dev/null
  echo "$sz" > "$off_file"
  return 0
}

play_ouch() {
  reason="$1"
  [ -x "$APLAY" ] || { log "no aplay"; return 1; }
  [ -s "$WAV" ] || { log "no wav $WAV"; return 1; }
  log "HIT reason=$reason → OUCH"
  HOME="$ROOT" "$APLAY" -D clanker "$WAV" >/dev/null 2>&1 &
  ap=$!
  i=0
  while [ "$i" -lt 4 ]; do
    kill -0 "$ap" 2>/dev/null || break
    i=$((i + 1))
    sleep 0.1 2>/dev/null || true
  done
  if kill -0 "$ap" 2>/dev/null; then
    echo $ap > /tmp/ouch_aplay.pid
    return 0
  fi
  mp=$(cat /mnt/data/clean_music_aplay.pid 2>/dev/null)
  if [ -n "$mp" ] && kill -0 "$mp" 2>/dev/null; then
    kill -STOP "$mp" 2>/dev/null || true
    HOME="$ROOT" "$APLAY" -D default "$WAV" >/dev/null 2>&1
    kill -CONT "$mp" 2>/dev/null || true
    return 0
  fi
  HOME="$ROOT" "$APLAY" -D default "$WAV" >/dev/null 2>&1 &
  echo $! > /tmp/ouch_aplay.pid
  return 0
}

maybe_ouch() {
  reason="$1"
  t=$(now_ms | tr -cd '0-9')
  [ -n "$t" ] || t=0
  last="$last_ouch_ms"
  case "$last" in ''|*[!0-9]*) last=0 ;; esac
  if [ "$last" != "0" ] && [ -n "$t" ]; then
    delta=$(( ${t:-0} - ${last:-0} ))
    cool=${COOLDOWN_MS:-900}
    case "$cool" in ''|*[!0-9]*) cool=900 ;; esac
    if [ "$delta" -ge 0 ] 2>/dev/null; then
      if [ "$delta" -lt "$cool" ] 2>/dev/null; then
        log "debounce reason=$reason delta=${delta}ms"
        return 0
      fi
    fi
  fi
  play_ouch "$reason"
  last_ouch_ms=$t
}

# single instance
for p in $(ps w 2>/dev/null | grep '[o]uch_watcher' | awk '{print $1}'); do
  if [ "$p" != "$$" ]; then
    if [ -f "$PIDFILE" ]; then
      op=$(cat "$PIDFILE" 2>/dev/null)
      if [ -n "$op" ] && [ "$op" != "$$" ] && kill -0 "$op" 2>/dev/null; then
        log "already live pid=$op"; exit 0
      fi
    fi
    kill "$p" 2>/dev/null || true
  fi
done
echo $$ > "$PIDFILE"
trap 'rm -f "$PIDFILE"; log "ouch_watcher exit"; exit 0' INT TERM

mkdir -p "$ROOT" "$ROOT/sounds" /tmp/ouch_off
[ -f "$ENABLED_FILE" ] || echo 1 > "$ENABLED_FILE"
for pair in "MCU:$MCU_LOG" "EVT:$EVT_LOG"; do
  name=${pair%%:*}
  path=${pair#*:}
  of=/tmp/ouch_off/$name
  if [ -f "$path" ]; then
    wc -c < "$path" 2>/dev/null | tr -cd '0-9' > "$of"
    [ -s "$of" ] || echo 0 > "$of"
  else
    echo 0 > "$of"
  fi
done

log "ouch_watcher start poll=${POLL_MS}ms cooldown=${COOLDOWN_MS}ms strict=bumper/LightTouch wav=$WAV"
last_ouch_ms=0
prev_bumper=0

while true; do
  if ! is_enabled; then
    sleep_ms 1000
    continue
  fi

  body=$(fetch_status)
  if [ -n "$body" ]; then
    bumps=$(parse_bumpers "$body")
    if bump_active "$bumps"; then
      if [ "$prev_bumper" = "0" ]; then
        maybe_ouch "adbumper=[$bumps]"
      fi
      prev_bumper=1
    else
      prev_bumper=0
    fi
  fi

  for pair in "MCU:$MCU_LOG" "EVT:$EVT_LOG"; do
    name=${pair%%:*}
    path=${pair#*:}
    of=/tmp/ouch_off/$name
    chunk=$(read_new "$path" "$of" 2>/dev/null) || chunk=""
    if [ -n "$chunk" ] && chunk_is_hit "$chunk"; then
      maybe_ouch "log:$name:${HIT_REASON}"
    fi
  done

  sleep_ms "$POLL_MS"
done
