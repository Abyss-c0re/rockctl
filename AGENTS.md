# rockctl agents

- Product: local miio CLI + HTTP for robot control; optional dash embed.
- **No secrets in git** — token paths only; real material on device (SECURITY.md).
- Never hardcode site IPs or lab serials; use env (`CLANKER_HOST` / `ROCKCTL_HOST`) in install scripts.
- Peer token API: never expose value on GET; generate needs current token or admin master.
- CORS OPTIONS must stay for dash on another port → rockctl `:8080`.
- Embed dash before arm ship: `./scripts/embed_dash.sh <spa-index.html>`.
- Do not import nanobot product code here.
