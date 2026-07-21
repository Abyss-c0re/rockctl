#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Optional portable profile (CLANKER_HOST / ROCKCTL_URL / paths)
if [[ -f "$ROOT/../scripts/load_device_env.sh" ]]; then
  # shellcheck disable=SC1091
  . "$ROOT/../scripts/load_device_env.sh" 2>/dev/null || true
elif [[ -f "$ROOT/../../clanker/scripts/load_device_env.sh" ]]; then
  . "$ROOT/../../clanker/scripts/load_device_env.sh" 2>/dev/null || true
fi
BIN="$ROOT/build/armv7/rockctl"
HOST="${ROCKCTL_HOST:-${CLANKER_HOST:-}}"
if [[ -z "$HOST" || "$HOST" == "127.0.0.1" ]]; then
  echo "Set CLANKER_HOST or ROCKCTL_HOST to the robot address (see clanker/config/device.example.env)" >&2
  exit 2
fi
KEY="${CLANKER_SSH_KEY:-${ROCKCTL_SSH_KEY:-$HOME/.ssh/id_rsa}}"
DEST="${ROCKCTL_HOME:-/mnt/data/rockctl}"

[[ -x "$BIN" ]] || { echo "make arm first"; exit 1; }

ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=10 "root@$HOST" "mkdir -p $DEST/bin $DEST/docs"
ssh -i "$KEY" -o BatchMode=yes "root@$HOST" "cat > $DEST/bin/rockctl.new && chmod 755 $DEST/bin/rockctl.new && mv -f $DEST/bin/rockctl.new $DEST/bin/rockctl" < "$BIN"
ssh -i "$KEY" -o BatchMode=yes "root@$HOST" "cat > $DEST/docs/API.md" < "$ROOT/docs/API.md"

# install _root.sh without valetudo; start rockctl serve
ssh -i "$KEY" -o BatchMode=yes "root@$HOST" 'sh -s' << 'REMOTE'
set -e
# stop valetudo
killall valetudo 2>/dev/null || true
# backup and remove valetudo binary (free ~13MB)
if [ -f /mnt/data/valetudo ]; then
  mv -f /mnt/data/valetudo /mnt/data/valetudo.removed.$(date +%Y%m%d%H%M%S) 2>/dev/null || rm -f /mnt/data/valetudo
fi
rm -f /mnt/data/valetudo_config.json

# rewrite boot hook
cat > /mnt/reserve/_root.sh << 'ROOT'
#!/bin/bash
# rockctl boot — no Valetudo
if [[ -f /mnt/data/miio/wifi.conf ]]; then
  if grep -q -e "cfg_by=tuya" -e "cfg_by=rriot" /mnt/data/miio/wifi.conf 2>/dev/null; then
    sed -i "s/cfg_by=tuya/cfg_by=miot/g" /mnt/data/miio/wifi.conf
    sed -i "s/cfg_by=rriot/cfg_by=miot/g" /mnt/data/miio/wifi.conf
  fi
fi
# Prefer pinned lab SSID (set via ADB/ClankerCommander wifi_pin) so mult does not roam back
if [[ -x /mnt/data/rockctl/bin/wifi_pin.sh && -f /mnt/data/rockctl/wifi_preferred.env ]]; then
  /mnt/data/rockctl/bin/wifi_pin.sh --apply-preferred --no-restart \
    >> /mnt/data/rockctl/wifi_pin.log 2>&1 || true
fi
rm -rf /mnt/data/rockrobo/rrlog/*REL 2>/dev/null || true

# rockctl: watchdog owns serve (restarts if dead or hung)
if [[ -x /mnt/data/rockctl/bin/rockctl_watchdog.sh ]]; then
  kill $(cat /mnt/data/rockctl/rockctl_watchdog.pid 2>/dev/null) 2>/dev/null || true
  killall rockctl_watchdog.sh 2>/dev/null || true
  /mnt/data/rockctl/bin/rockctl_watchdog.sh >> /mnt/data/rockctl/watchdog.log 2>&1 &
elif [[ -x /mnt/data/rockctl/bin/rockctl ]]; then
  killall rockctl 2>/dev/null || true
  /mnt/data/rockctl/bin/rockctl serve --port 8080 >> /mnt/data/rockctl/rockctl.log 2>&1 &
fi

# optional nanobot (settings-aware)
if [[ -x /mnt/data/nanobot/run.sh ]]; then
  killall nanobot 2>/dev/null || true
  export NANOBOT_HOME=/mnt/data/nanobot
  /mnt/data/nanobot/run.sh >> /mnt/data/nanobot/nanobot.log 2>&1 &
fi

# optional clean music left intact if present
if [[ -x /mnt/data/clean_music_loop.sh && -f /mnt/data/clean_loop_full.wav ]]; then
  kill $(cat /mnt/data/clean_music.pid 2>/dev/null) 2>/dev/null || true
  /mnt/data/clean_music_loop.sh >> /mnt/data/clean_music.log 2>&1 &
elif [[ -x /mnt/data/rockctl/bin/clean_music_loop.sh ]]; then
  kill $(cat /mnt/data/clean_music.pid 2>/dev/null) 2>/dev/null || true
  /mnt/data/rockctl/bin/clean_music_loop.sh >> /mnt/data/clean_music.log 2>&1 &
fi
ROOT
chmod 755 /mnt/reserve/_root.sh

# watchdog companion (push separately if missing on robot)
REMOTE

echo "Installed. API: http://$HOST:8080/  CLI: rockctl status"
