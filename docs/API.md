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
| PUT | `/api/v1/water` | `{"level":"off\|low\|medium\|high"}` | Mop moisture (water box) |
| GET | `/api/v1/schedule` | — | Cleaning jobs (`/mnt/data/rockctl/schedule.json`) |
| PUT | `/api/v1/schedule` | `{"ok":true,"jobs":[…]}` or one job | Persist schedule (survives reboot) |
| DELETE | `/api/v1/schedule` | — | Clear all jobs |
| POST | `/api/v1/raw` | `{"method":"…","params":[]}` | Raw miio method |
| GET | `/api/v1/drive/last` | — | Last drive path JSON (ClankerDash coverage + lab export) |
| PUT | `/api/v1/drive/last` | session JSON | Save last drive path (`/mnt/data/rockctl/drive/last.json`) |
| GET | `/api/v1/drive/coverage` | — | RRSLAM path-layer cells (last clean coverage sample) |

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
