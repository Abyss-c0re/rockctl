#!/bin/sh
# rockctl_watchdog — keep rockctl HTTP serve alive (and restart if hung).
# Lean: busybox ash, no bashisms. Poll ~15s. Log rotated ~24KB.
#
# Boot: started from /mnt/reserve/_root.sh
# Manual: /mnt/data/rockctl/bin/rockctl_watchdog.sh &
#
# Env:
#   ROCKCTL_PORT=8080
#   ROCKCTL_BIN=/mnt/data/rockctl/bin/rockctl
#   ROCKCTL_WD_INTERVAL=15   # seconds between checks
#   ROCKCTL_WD_HANG=1        # 1=kill if process up but health fails

ROOT=/mnt/data/rockctl
BIN="${ROCKCTL_BIN:-$ROOT/bin/rockctl}"
PORT="${ROCKCTL_PORT:-8080}"
INTERVAL="${ROCKCTL_WD_INTERVAL:-15}"
CHECK_HANG="${ROCKCTL_WD_HANG:-1}"
PIDFILE="$ROOT/rockctl_watchdog.pid"
SERVE_PIDFILE="$ROOT/rockctl.pid"
LOG="$ROOT/watchdog.log"
SERVE_LOG="$ROOT/rockctl.log"

log() {
  # keep log tiny
  echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null
  sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
  if [ "$sz" -gt 24576 ] 2>/dev/null; then
    tail -c 8192 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
  fi
}

health_ok() {
  # Prefer wget (busybox); curl optional
  if command -v wget >/dev/null 2>&1; then
    out=$(wget -q -T 3 -O- "http://127.0.0.1:${PORT}/api/v1/health" 2>/dev/null) || return 1
  elif command -v curl >/dev/null 2>&1; then
    out=$(curl -sS -m 3 "http://127.0.0.1:${PORT}/api/v1/health" 2>/dev/null) || return 1
  else
    # no client: only process check
    return 0
  fi
  echo "$out" | grep -q '"ok"' || echo "$out" | grep -q rockctl
}

rockctl_running() {
  # any rockctl serve process
  ps 2>/dev/null | grep -v grep | grep -q '[r]ockctl serve' && return 0
  if [ -f "$SERVE_PIDFILE" ]; then
    sp=$(cat "$SERVE_PIDFILE" 2>/dev/null)
    [ -n "$sp" ] && kill -0 "$sp" 2>/dev/null && return 0
  fi
  return 1
}

start_rockctl() {
  if [ ! -x "$BIN" ]; then
    log "missing binary $BIN"
    return 1
  fi
  # free stale listeners
  killall rockctl 2>/dev/null || true
  sleep 1
  killall -9 rockctl 2>/dev/null || true
  sleep 1
  mkdir -p "$ROOT"
  # trim serve log if huge
  if [ -f "$SERVE_LOG" ]; then
    sz=$(wc -c < "$SERVE_LOG" 2>/dev/null || echo 0)
    if [ "$sz" -gt 65536 ] 2>/dev/null; then
      tail -c 16384 "$SERVE_LOG" > "$SERVE_LOG.tmp" 2>/dev/null && mv "$SERVE_LOG.tmp" "$SERVE_LOG"
    fi
  fi
  nohup "$BIN" serve --port "$PORT" >> "$SERVE_LOG" 2>&1 &
  echo $! > "$SERVE_PIDFILE"
  log "started rockctl pid=$! port=$PORT"
  sleep 2
}

# single instance
if [ -f "$PIDFILE" ]; then
  old=$(cat "$PIDFILE" 2>/dev/null)
  if [ -n "$old" ] && [ "$old" != "$$" ] && kill -0 "$old" 2>/dev/null; then
    cmd=$(tr '\0' ' ' < /proc/"$old"/cmdline 2>/dev/null || true)
    case "$cmd" in
      *rockctl_watchdog*) log "already live pid=$old"; exit 0 ;;
    esac
  fi
fi
echo $$ > "$PIDFILE"
trap 'rm -f "$PIDFILE"; log "watchdog exit"; exit 0' INT TERM

mkdir -p "$ROOT"
log "watchdog start interval=${INTERVAL}s hang_check=$CHECK_HANG bin=$BIN"

# ensure one serve on boot
if ! rockctl_running || ! health_ok; then
  start_rockctl
fi

fail=0
while true; do
  sleep "$INTERVAL" || sleep 15

  if ! rockctl_running; then
    log "rockctl dead — restart"
    start_rockctl
    fail=0
    continue
  fi

  if [ "$CHECK_HANG" = "1" ]; then
    if health_ok; then
      fail=0
    else
      fail=$((fail + 1))
      log "health fail streak=$fail"
      # 2 consecutive fails → hung (accept can stick on miio)
      if [ "$fail" -ge 2 ]; then
        log "rockctl hung — kill -9 and restart"
        killall -9 rockctl 2>/dev/null || true
        sleep 1
        start_rockctl
        fail=0
      fi
    fi
  fi
done
