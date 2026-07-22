#!/bin/sh
# Keep /mnt/data light: wipe firmware logs & app logs; never touch maps/miio/music/bins.
# Safe to run at boot and periodically from clean_music_loop or cron-like loops.

LOG=${AUTOWIPE_LOG:-/mnt/data/rockctl/autowipe.log}
DATA=/mnt/data
RRLOG=$DATA/rockrobo/rrlog
KEEP_MAPS="last_map user_map0 ChargerPos.data StartPos.data"

log() { echo "$(date 2>/dev/null || echo '?') $*" >> "$LOG" 2>/dev/null; }

# free MB on data (busybox df)
free_kb() {
  df -k "$DATA" 2>/dev/null | tail -1 | awk '{print $4}'
}

mkdir -p "$DATA/rockctl" 2>/dev/null
BEFORE=$(free_kb)

# --- firmware session logs (biggest) ---
# Remove old REL session dirs; keep directory itself
if [ -d "$RRLOG" ]; then
  # messages rotated copies
  rm -f "$RRLOG"/messages.*.xz 2>/dev/null
  # session folders (nav/core dumps)
  for d in "$RRLOG"/*; do
    [ -e "$d" ] || continue
    base=$(basename "$d")
    case "$base" in
      messages) continue ;;
      *REL|*rel)
        rm -rf "$d" 2>/dev/null
        log "rm rrlog session $base"
        ;;
    esac
  done
  # truncate live messages if huge (>256K)
  if [ -f "$RRLOG/messages" ]; then
    sz=$(wc -c < "$RRLOG/messages" 2>/dev/null || echo 0)
    if [ "$sz" -gt 262144 ] 2>/dev/null; then
      : > "$RRLOG/messages"
      log "truncate rrlog/messages was=$sz"
    fi
  fi
fi

# --- app / lab logs (truncate, keep files) ---
for f in \
  "$DATA/clean_music.log" \
  "$DATA/rockctl/rockctl.log" \
  "$DATA/rockctl/autowipe.log" \
  "$DATA/nanobot/nanobot.log" \
  "$DATA/nanobot/nanobot.out" \
  "$DATA/aplay_full.log" \
  "$DATA/aplay_test.log" \
  "$DATA/aplay_test2.log" \
  "$DATA/aplay_l.log" \
  "$DATA/1.log"
do
  [ -f "$f" ] || continue
  sz=$(wc -c < "$f" 2>/dev/null || echo 0)
  # keep last 8K of autowipe/music logs; zero junk logs
  case "$f" in
    *autowipe.log|*clean_music.log|*nanobot.log|*rockctl.log)
      if [ "$sz" -gt 16384 ] 2>/dev/null; then
        tail -c 8192 "$f" > "$f.tmp" 2>/dev/null && mv -f "$f.tmp" "$f" || : > "$f"
        log "trim $f was=$sz"
      fi
      ;;
    *)
      : > "$f" 2>/dev/null
      ;;
  esac
done

# --- one-shot junk (install leftovers) ---
rm -f "$DATA/audio_portable.tgz" "$DATA/aplay_trusty.tgz" 2>/dev/null
rm -f "$DATA/clean_loop.ogg" "$DATA/clean_loop_16k.ogg" "$DATA/clean_loop_20s.ogg" 2>/dev/null
# root duplicates of library tracks (library lives in rockctl/music)
if [ -d "$DATA/rockctl/music" ]; then
  for f in clean_loop_full.wav clean_loop_30s.wav; do
    if [ -f "$DATA/rockctl/music/$f" ] && [ -f "$DATA/$f" ]; then
      rm -f "$DATA/$f"
      log "rm duplicate $DATA/$f"
    fi
  done
fi
# old dash dumps
rm -rf "$DATA/clanker-dash" "$DATA/clankerdash" 2>/dev/null
rm -rf "$DATA/unsquashfs" 2>/dev/null

# NEVER delete maps
# $DATA/rockrobo/last_map user_map0 stay

AFTER=$(free_kb)
log "autowipe done free_kb before=$BEFORE after=$AFTER"
echo "autowipe: free_kb $BEFORE -> $AFTER"

# --- lhlam learn-lab byproducts (keep latest only) ---
if [ -d "$DATA/lhlam/lab/logs" ]; then
  for f in "$DATA/lhlam/lab/logs"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    case "$base" in
      on_robot_teach_latest.log|self_learning_drive_nohup.log) ;;
      loop.jsonl)
        sz=$(wc -c < "$f" 2>/dev/null || echo 0)
        if [ "$sz" -gt 8192 ] 2>/dev/null; then
          tail -c 4096 "$f" > "$f.tmp" 2>/dev/null && mv -f "$f.tmp" "$f" || : > "$f"
          log "trim lhlam loop.jsonl was=$sz"
        fi
        ;;
      *) rm -f "$f" 2>/dev/null; log "rm lhlam log $base" ;;
    esac
  done
fi
if [ -d "$DATA/lhlam/lab/paths" ]; then
  rm -f "$DATA/lhlam/lab/paths"/drive_*.json 2>/dev/null
fi
if [ -d "$DATA/rockctl/drive" ]; then
  rm -f "$DATA/rockctl/drive"/drive_*.json 2>/dev/null
fi
