# sounds

Short event clips on the robot under `/mnt/data/rockctl/sounds/`.

| Event | Role |
|-------|------|
| `ouch` | Bumper / light-touch hit (ouch_watcher) |
| `dock` | Config slot (future wiring) |
| `start_clean` / `done_clean` / `low_battery` | Config slots |

**ClankerDash → Sounds:** pick a library WAV or record SAM TTS (pitch/speed/volume/throat/mouth per recording) into `<event>_tts.wav`.

Config on robot: `sounds/events/<id>/{enabled,file,tts_text,tts_volume,…}`.  
API: `GET/POST /api/v1/sounds`, upload `POST /api/v1/sounds/upload`.

WAV binaries are **gitignored** (ship on robot). Keep this README only in git.
