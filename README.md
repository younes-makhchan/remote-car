# Remote Car Control Platform

FastAPI backend and lightweight web client for driving an ESP32-powered rover with sub-second latency from any browser.

- **Low-latency control path**: Browser → FastAPI → ESP32 (WebSocket relay).
- **Signaling path**: Browser ↔ Browser WebRTC room for live video/control sync.
- **Stateless browsers**: clients auto-discover each other via `/ws?role=car|ctrl`.

---

## 1. Project Layout

- `fastApi/app.py` – FastAPI application exposing control endpoints, WebSocket bridges, and static asset hosting.
- `fastApi/public/` – HTML/CSS/JS front-ends (`index.html` controller, `car.html` rover HUD, `controller.html` legacy view).
- `src/`, `include/`, `lib/` – PlatformIO firmware for the ESP32 (motor drivers, sensors).
- `platformio.ini` – PlatformIO environment configuration for compiling and flashing the ESP32 firmware.
- `test/` – Hardware/software integration tests (expand with simulation stubs as the project grows).

---

## 2. Quick Start (Local Dev)

### Backend
```bash
cd fastApi
python3 -m venv .venv
source .venv/bin/activate  # Windows: .venv\\Scripts\\activate
pip install -r requirements.txt  # create from manual list below if missing
# or: pip install fastapi uvicorn[standard] websockets

uvicorn app:app --host 0.0.0.0 --port 5000 --reload
```

### Controller UI
- Visit `http://localhost:5000/` and choose **Drive** (or go directly to `http://localhost:5000/controller`) for the controller dashboard.
- The rover client should open `http://localhost:5000/` → **Car Node** (direct: `http://localhost:5000/car`).
- ESP32 firmware connects via WebSocket to `ws://<server-ip>:5000/esp` and sends 5 s heartbeat pings.

---

## 3. Core Endpoints

| Endpoint | Method | Purpose | Notes |
| --- | --- | --- | --- |
| `/ctrl` | WebSocket | Bidirectional controller command channel | JSON `drive/stop/speed/heartbeat` messages with `ack` + `status` replies |
| `/move` | POST (JSON `{ "dir": "F" }`) | Relay directional commands to ESP32 | Legacy fallback; rejects if ESP offline |
| `/rotate` | POST (JSON `{ "dir": "L", "deg": 60 }`) | Timed rotation (start/stop with calibrated delay) | Applies a dedicated rotation speed, returns whether stop succeeded |
| `/speed` | POST (JSON `{ "val": 180 }`) | Adjust PWM speed by mapping to single-digit command | Clamps range, surfaces offline errors |
| `/esp` | WebSocket | Persistent ESP32 connection | Maintains connection status + last seen |
| `/ws?role=car|ctrl` | WebSocket | WebRTC signaling bridge between car and controller browsers | Rejects duplicate role connections; auto notifies when both peers connected |
| `/health` | GET | Report ESP32 status & connected signaling peers | Includes connected status, last-seen age |

### Controller WebSocket Messages

- **Outbound (controller → server)**  
  - `{"type":"hello","client":"controller","version":"2.0"}` for handshake metadata.  
  - `{"type":"drive","dir":"F"}` to begin continuous motion; `{"type":"stop"}` to halt.  
  - `{"type":"speed","value":180}` to adjust PWM.  
  - `{"type":"heartbeat"}` every ~2 s to keep the session alive.
- **Inbound (server → controller)**  
  - `{"type":"ack","cmd":"drive","status":"ok","dir":"F"}` confirms command delivery and provides latency targets.  
  - `{"type":"status","esp":{...},"car":{...},"controller":{...}}` broadcasts session health.  
  - `{"type":"error","reason":"esp_offline"}` communicates faults.

HTTP endpoints (`/move`, `/speed`, `/rotate`) remain as fallbacks for legacy clients and automation, but the UI now prefers `/ctrl`.

### WebRTC Signaling Configuration

- By default both `car.html` and `controller.html` use Google’s public STUN server (`stun:stun.l.google.com:19302`).
- Override the list with a comma-separated `STUN_SERVERS` environment variable before launching FastAPI, e.g.  
  `STUN_SERVERS="stun:global.stun.twilio.com:3478,turn:turn.example.com:3478?transport=udp" uvicorn app:app ...`
- The front-ends fetch `/config` on load to pull the current list; add TURN URLs (with credentials in the URL) when deploying across difficult NATs.

---

## 4. Deployment Notes

1. **Container / VM**: Run `uvicorn app:app --host 0.0.0.0 --port ${PORT:-5000} --workers 1`. Keep workers at `1` to avoid WebSocket session pinning issues with the ESP32.
2. **TLS / Reverse Proxy**: Terminate TLS in front of Uvicorn (nginx, Caddy, Traefik). Ensure WebSockets are forwarded.
3. **Static assets**: Served directly from `fastApi/public`. Amend `PUBLIC_DIR` in `app.py` only if you move assets.
4. **Environment**: Set `PORT` environment variable if 5000 is unavailable.
5. **Observability**: Logs report ESP32 connection status; consider adding Prometheus or structured logging before production.

---

## 5. Firmware Workflow (PlatformIO)

```bash
pio run          # compile firmware
pio run -t upload  # flash connected ESP32
pio device monitor  # open serial monitor for debugging
```

Tune motor timings in firmware, but keep `/rotate` duration map synchronized. Update both sides if you recalibrate.

---

## 6. Future Enhancements

- Add auth or pairing tokens before exposing to the public internet.
- Persist rotation calibration on the server using a config file or database.
- Integrate WebRTC video feed from the rover camera directly in `car.html`.
- Provide automated tests covering command routing and WebSocket failure recovery.

---

## 7. Troubleshooting

- **ESP32 not connected**: Check serial console; ensure it can reach the server IP/port; verify CORS/WS not blocked.
- **Commands lagging**: Confirm both network hops are <10 ms; inspect `uvicorn` logs for backpressure; ensure single worker.
- **WebRTC peers not pairing**: Inspect browser console for ICE server errors; ensure both clients reach `/ws` endpoint.
