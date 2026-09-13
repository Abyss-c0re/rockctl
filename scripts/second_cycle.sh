#!/bin/sh
# After the current clean returns/docks, start one more auto clean.
# Used when firmware has no set_clean_count (e.g. 4.3.5).
# Does not restart rockctl. Safe to run once per job.

URL="${ROCKCTL_STATUS_URL:-http://127.0.0.1:8080/api/v1/status}"
CTRL="${ROCKCTL_CONTROL_URL:-http://127.0.0.1:8080/api/v1/control}"
LOG="${SECOND_CYCLE_LOG:-/mnt/data/rockctl/second_cycle.log}"
PIDFILE=/tmp/clanker_second_cycle.pid

log() { echo "$(date 2>/dev/null || echo '?') $*" >>"$LOG"; }

if [ -f "$PIDFILE" ]; then
  old=$(cat "$PIDFILE" 2>/dev/null)
  if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
    log "already live pid=$old"
    exit 0
  fi
fi
echo $$ >"$PIDFILE"
trap 'rm -f "$PIDFILE"' EXIT INT TERM

get_st() {
  body=$(wget -q -O - -T 3 "$URL" 2>/dev/null) || body=""
  echo "$body" | sed -n 's/.*"state"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1
}

log "watch start"
saw_clean=0
i=0
while [ "$i" -lt 720 ]; do
  i=$((i + 1))
  st=$(get_st)
  [ -n "$st" ] || { sleep 5; continue; }
  case "$st" in
    5|11|17|18) saw_clean=1 ;;
  esac
  if [ "$saw_clean" = "1" ]; then
    case "$st" in
      10)
        log "paused — abort second cycle"
        exit 0
        ;;
      12)
        log "error — abort second cycle"
        exit 0
        ;;
      6|8|15)
        log "first cycle done state=$st — start second"
        wget -q -O - -T 8 --post-data='{"action":"start","sync":true}' \
          --header='Content-Type: application/json' "$CTRL" >>"$LOG" 2>&1 || \
        wget -q -O - -T 8 --post-data='{"action":"start"}' \
          --header='Content-Type: application/json' "$CTRL" >>"$LOG" 2>&1 || true
        log "second start sent"
        exit 0
        ;;
    esac
  fi
  sleep 5
done
log "timeout waiting for first cycle end"
exit 1
