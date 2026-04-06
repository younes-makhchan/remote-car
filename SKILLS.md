# Remote Car Skills

Quick reference for day-to-day workflows after the control-channel overhaul.

## Backend Ops
- `cd fastApi && uvicorn app:app --host 0.0.0.0 --port 5000 --reload` — launch the FastAPI server.
- `curl http://localhost:5000/health` — check ESP32, controller, and car session status.
- `wscat -c ws://localhost:5000/ctrl` — inspect the controller WebSocket (send JSON `{"type":"status"}` or `{"type":"drive","dir":"F"}`).

## Controller Channel
- Primary control path is `ws://<host>/ctrl`. Send `drive`, `stop`, `speed`, and `heartbeat` messages; expect `ack`, `status`, or `error` responses.
- Legacy REST fallbacks remain (`POST /move`, `/speed`, `/rotate`) for scripts or older builds, but new UI uses the WebSocket exclusively.

## WebRTC Signaling
- Car phone: `ws://<host>/ws?role=car` — automatically publishes offer/ICE. Use `car.html`.
- Controller phone: `ws://<host>/ws?role=ctrl` — receives offer, answers, and exchanges ICE. Requires microphone permission for two-way audio.
- STUN/TURN servers come from `GET /config`; override with `STUN_SERVERS="stun:foo,turn:bar?transport=udp"` when starting FastAPI.

## PlatformIO Firmware
- `pio run` — build ESP32 firmware.
- `pio run -t upload` — flash connected board.
- `pio device monitor` — open serial console to confirm heartbeat and command logs.
