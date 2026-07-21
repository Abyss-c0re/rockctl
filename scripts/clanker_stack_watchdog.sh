#!/bin/sh
# clanker_stack_watchdog — keep rockctl + nanobot alive (lean, ash).
# Complements rockctl_watchdog (HTTP hang) with nanobot process care.
# Interval default 30s. Log: /mnt/data/rockctl/stack_watchdog.log

ROOT=/mnt/data/rockctl
NANO_HOME=/mnt/data/nanobot
INTERVAL="${CLANKER_STACK_WD_INTERVAL:-30}"
PIDFILE="$ROOT/stack_watchdog.pid"
LOG="$ROOT/stack_watchdog.log"
ROCK_PORT="${ROCKCTL_PORT:-8080}"
NANO_PORT="${NANOBOT_PORT:-8787}"

log() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null
  sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
  if [ "$sz" -gt 24576 ] 2>/dev/null; then
    tail -c 8192 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
  fi
}

http_ok() {
  url="$1"
  if command -v wget >/dev/null 2>&1; then
    wget -q -T 3 -O- "$url" 2>/dev/null | grep -q ok && return 0
    return 1
  fi
  if command -v curl >/dev/null 2>&1; then
    curl -sS -m 3 "$url" 2>/dev/null | grep -q ok && return 0
    return 1
  fi
  return 0
}

ensure_rockctl() {
  # Prefer dedicated rockctl watchdog if present
  if [ -x "$ROOT/bin/rockctl_watchdog.sh" ]; then
    if [ -f "$ROOT/rockctl_watchdog.pid" ]; then
      rp=$(cat "$ROOT/rockctl_watchdog.pid" 2>/dev/null)
      if [ -n "$rp" ] && kill -0 "$rp" 2>/dev/null; then
        :
      else
        log "rockctl_watchdog dead — restart"
        "$ROOT/bin/rockctl_watchdog.sh" >> "$ROOT/watchdog.log" 2>&1 &
      fi
    else
      # no pid file but maybe running
      if ! ps 2>/dev/null | grep -v grep | grep -q '[r]ockctl_watchdog'; then
        log "start rockctl_watchdog"
        "$ROOT/bin/rockctl_watchdog.sh" >> "$ROOT/watchdog.log" 2>&1 &
      fi
    fi
  fi
  # Fallback: direct rockctl if no rockctl_watchdog
  if ! http_ok "http://127.0.0.1:${ROCK_PORT}/api/v1/health"; then
    if [ ! -x "$ROOT/bin/rockctl_watchdog.sh" ] && [ -x "$ROOT/bin/rockctl" ]; then
      log "rockctl health fail — direct restart"
      killall rockctl 2>/dev/null || true
      sleep 1
      nohup "$ROOT/bin/rockctl" serve --port "$ROCK_PORT" >> "$ROOT/rockctl.log" 2>&1 &
      echo $! > "$ROOT/rockctl.pid"
    fi
  fi
}


ensure_ouch() {
  if [ ! -x "$ROOT/bin/ouch_watcher.sh" ]; then
    return 0
  fi
  if ps 2>/dev/null | grep -v grep | grep -q '[o]uch_watcher'; then
    return 0
  fi
  log "start ouch_watcher"
  "$ROOT/bin/ouch_watcher.sh" >> "$ROOT/ouch_watcher.log" 2>&1 &
}


ensure_nanobot() {
  if http_ok "http://127.0.0.1:${NANO_PORT}/peer/v1/health"; then
    return 0
  fi
  log "nanobot health fail — restart"
  kill $(cat "$NANO_HOME/nanobot.pid" 2>/dev/null) 2>/dev/null || true
  killall nanobot 2>/dev/null || true
  sleep 1
  export NANOBOT_HOME="$NANO_HOME"
  if [ -x "$NANO_HOME/run.sh" ]; then
    nohup "$NANO_HOME/run.sh" >> "$NANO_HOME/nanobot.out" 2>&1 &
    echo $! > "$NANO_HOME/nanobot.pid"
  elif [ -x "$NANO_HOME/bin/nanobot" ]; then
    WWW=""
    if [ -f "$NANO_HOME/settings" ]; then
      # shellcheck disable=SC1090
      . "$NANO_HOME/settings" 2>/dev/null || true
    fi
    if [ "${UI:-off}" = "on" ] && [ -n "${WWW:-}" ] && [ -d "$WWW" ]; then
      nohup "$NANO_HOME/bin/nanobot" --home "$NANO_HOME" --port "$NANO_PORT" --www "$WWW" \
        >> "$NANO_HOME/nanobot.out" 2>&1 &
    else
      nohup "$NANO_HOME/bin/nanobot" --home "$NANO_HOME" --port "$NANO_PORT" \
        >> "$NANO_HOME/nanobot.out" 2>&1 &
    fi
    echo $! > "$NANO_HOME/nanobot.pid"
  else
    log "nanobot binary missing"
    return 1
  fi
  sleep 2
  if http_ok "http://127.0.0.1:${NANO_PORT}/peer/v1/health"; then
    log "nanobot recovered"
  else
    log "nanobot still down after restart"
  fi
}

# single instance
if [ -f "$PIDFILE" ]; then
  old=$(cat "$PIDFILE" 2>/dev/null)
  if [ -n "$old" ] && [ "$old" != "$$" ] && kill -0 "$old" 2>/dev/null; then
    cmd=$(tr '\0' ' ' < /proc/"$old"/cmdline 2>/dev/null || true)
    case "$cmd" in
      *clanker_stack_watchdog*|*stack_watchdog*) log "already live pid=$old"; exit 0 ;;
    esac
  fi
fi
echo $$ > "$PIDFILE"
trap 'rm -f "$PIDFILE"; log "stack_watchdog exit"; exit 0' INT TERM

mkdir -p "$ROOT"
log "stack_watchdog start interval=${INTERVAL}s"

while true; do
  ensure_rockctl
  ensure_nanobot
  ensure_ouch
  sleep "$INTERVAL" || sleep 30
done
