# rockctl API

Local-first Roborock control over **miio** (same wire family Valetudo used, without Valetudo).

## CLI

```bash
rockctl status
rockctl start | stop | pause | home | spot | locate
rockctl fan quiet|balanced|turbo|max
rockctl consumable
rockctl raw get_status '[]'
rockctl serve --port 8080
```

Defaults: `--host 127.0.0.1 --token /mnt/data/miio/device.token`.

## HTTP

Base URL: `http://<robot>:8080`

| Method | Path | Body | Description |
|--------|------|------|-------------|
| GET | `/api/v1/health` | — | Liveness |
| GET | `/api/v1/status` | — | `get_status` miio result |
| GET | `/api/v1/consumable` | — | Consumables |
| POST | `/api/v1/control` | `{"action":"start\|stop\|pause\|home\|spot\|locate\|rc_start\|rc_end"}` | Basic control (+ manual aliases) |
| POST | `/api/v1/manual` | `{"action":"start\|stop\|forward\|back\|left\|right\|halt\|move",…}` | Remote control (no clean); add `"async":true` for non-blocking |
| POST | `/api/v1/manual/async` | same body | Always **202** + detached miio (client never waits on UDP) |
| PUT | `/api/v1/fan` | `{"level":"quiet\|balanced\|turbo\|max"}` | Fan / suction preset |
| PUT | `/api/v1/music` | `{"mode":"clean\|on\|off","volume":0-100}` | Clean-music; volume 66 default. Dock/return always stops playback |
| PUT | `/api/v1/water` | `{"level":"off\|low\|medium\|high"}` | Mop moisture (water box) |
| GET | `/api/v1/schedule` | — | Cleaning jobs (`/mnt/data/rockctl/schedule.json`) |
| PUT | `/api/v1/schedule` | `{"ok":true,"jobs":[…]}` or one job | Persist schedule (survives reboot) |
| DELETE | `/api/v1/schedule` | — | Clear all jobs |
| POST | `/api/v1/raw` | `{"method":"…","params":[]}` | Raw miio method |
| GET | `/api/v1/drive/last` | — | Last drive path JSON (ClankerDash coverage + lab export) |
| PUT | `/api/v1/drive/last` | session JSON | Save last drive path (`/mnt/data/rockctl/drive/last.json`) |
| GET | `/api/v1/drive/coverage` | — | RRSLAM path-layer cells (last clean coverage sample) |
| GET | `/api/v1/map/restrictions` | — | Virtual walls + no-go (`restrictions.json` + lab flag) |
| PUT | `/api/v1/map/restrictions` | `{"walls":[…],"zones":[…]}` | Draw/save via miio `save_map` (lab maps must be on) |
| DELETE | `/api/v1/map/restrictions` | — | Clear all walls and no-go zones |

### Last drive path (Dash + LHLAM)

```bash
# after on-robot teach publishes:
curl -s http://$CLANKER_HOST:8080/api/v1/drive/last | jq .
# map path layer (coverage dots on Dash):
curl -s http://$CLANKER_HOST:8080/api/v1/drive/coverage | jq .n
```

ClankerDash Map tab: **Path** reloads overlays; **Drive JSON** downloads export.  
Lime polyline = last session; magenta = map coverage; red = issues.

Schedule job fields: `id`, `enabled`, `hh`, `mm`, `dow` (e.g. `1-5`), `type` (`auto`|`spot`), `cycles` (1–3), `fan`, `water`.

`POST /api/v1/clean` with `cycles` ≥ 2 uses firmware repeat: `app_segment_clean` `{repeat}` when rooms exist, else `app_zoned_clean` `[[x1,y1,x2,y2,repeat]]` over the map (no-go / walls still apply). This AppProxy has no `set_clean_count`.

### Virtual walls & no-go

Coords are **miio / `app_goto_target` frame** (SLAM + 25500). Same as Summon / places.

```bash
# list
curl -s http://$CLANKER_HOST:8080/api/v1/map/restrictions | jq .

# one virtual wall (line) + one no-go box, then push to firmware
curl -s -X PUT http://$CLANKER_HOST:8080/api/v1/map/restrictions \
  -H 'Content-Type: application/json' \
  -d '{"walls":[{"id":"w1","x1":26000,"y1":27000,"x2":28000,"y2":27000}],
       "zones":[{"id":"z1","type":"nogo","x1":27000,"y1":30000,"x2":29000,"y2":32000}]}'

# clear
curl -s -X DELETE http://$CLANKER_HOST:8080/api/v1/map/restrictions
```

- Wall: two endpoints. Zone: axis-aligned box (`x1,y1`–`x2,y2`). Optional `type` `nogo` (default) or `nomop`.
- Limits: 10 walls, 10 zones, 68 vertices (firmware). Requires `lab_status=1` (map saving).
- ClankerDash Map tab: **Virtual wall** / **No-go zone** → tap two points → **Save to robot**.
| GET | `/openapi.yaml` | — | OpenAPI 3 document |
| GET | `/` | — | Short text index |

### Examples

```bash
curl -s http://$CLANKER_HOST:8080/api/v1/status | jq .
curl -s -X POST http://$CLANKER_HOST:8080/api/v1/control \
  -H 'Content-Type: application/json' -d '{"action":"home"}'
curl -s -X PUT http://$CLANKER_HOST:8080/api/v1/fan \
  -H 'Content-Type: application/json' -d '{"level":"balanced"}'
```

## Protocol notes

- Transport: miio UDP `:54321` with 16-byte device token (AES-128-CBC + MD5).
- Does **not** use Xiaomi/Roborock cloud.
- Does **not** include Valetudo UI/maps/MQTT — control plane only (maps can be added later).

## Safety

`start` will run the robot. Prefer `status` / `locate` while validating.
## Demo: work room (drive only)

```bash
rockctl demo work-room
curl -X POST http://ROBOT:8080/api/v1/demo/work-room
```

Uses `app_stop`/`app_pause` then `app_goto_target` — **no cleaning**.
Place: `/mnt/data/rockctl/places.json` key `work_room`.
