# Security

## Do not commit

| Material | Where it lives (on device) |
|----------|----------------------------|
| miio device token | `/mnt/data/miio/device.token` (or path you pass to CLI) |
| labauth master | `/mnt/data/labauth/master.key` (optional) |
| nanobot peer token | `$NANOBOT_HOME/peer_token` |
| Wi‑Fi / site env | host-side `device.env` — not this tree |

Paths above are **defaults**, not secrets. Real token bytes stay on the robot.

## Threat notes

- HTTP `:8080` is a local LAN control plane. Firewall to trusted nets.
- `POST /api/v1/assist/peer_token` never returns the token on GET; rotate with current token or admin master.
- Web UI gate password is hashed client/server-side; do not log form fields.
- Thread-pool HTTP: each request uses its own miio UDP client; do not log token material.

## Reporting

Report issues privately to the repository owner. Do not open public issues with live tokens or robot serials.
