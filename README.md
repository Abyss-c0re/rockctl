# rockctl

Local **miio** control for Roborock-class robots: CLI + HTTP API, optional embedded dash UI.

**Not affiliated with Roborock, Xiaomi, or Valetudo.**

```
src/          C sources (http, miio, places, main)
third_party/  AES + MD5 for miio framing
scripts/      install, watchdog, music/ouch helpers
www/          dash SPA (embed into binary via scripts/embed_dash.sh)
docs/         API + firewall notes
```

## Build

```bash
# optional: rebuild embedded dash from a SPA path
./scripts/embed_dash.sh ./www/index.html   # or path to clanker-dash source

make host          # native
make arm           # static armv7 (set ARM toolchain; see Makefile)
# → build/host/rockctl  or  build/armv7/rockctl
```

## Run (on robot)

```bash
# token: 16-byte file (never commit it)
export ROCKCTL_TOKEN=/mnt/data/miio/device.token   # or --token PATH

rockctl status|start|stop|pause|home|spot|locate
rockctl fan quiet|balanced|turbo|max
rockctl serve --port 8080
```

Defaults: miio host `127.0.0.1`, token path `/mnt/data/miio/device.token`.

## HTTP (summary)

| Method | Path | Notes |
|--------|------|--------|
| GET | `/api/v1/health` | Liveness (always cheap) |
| GET | `/api/v1/status` | miio `get_status` |
| POST | `/api/v1/control` | start/stop/pause/home/… |
| POST | `/api/v1/manual` | remote control (no clean) |
| PUT | `/api/v1/fan` | suction preset |
| GET | `/` | embedded dash |

Concurrency: **thread pool** (main thread accepts + schedule; workers handle requests). See `docs/API.md`.

## Secrets

See [SECURITY.md](SECURITY.md). Never commit `device.token`, peer tokens, or master keys.

## License

**Cubechain License** — [LICENSE](LICENSE).
