# Remote Car Telepresence Platform

This repository fuses three pieces into a single remote-driving stack:

1. **FastAPI backend (`fastApi/`)** – relays motion commands to the rover, exposes health/state APIs, and serves the browser UIs.
2. **Browser UIs (`fastApi/public/`)** – WebRTC driven car HUD (`car.html`) and controller dashboard (`controller.html`) with an index launcher.
3. **ESP32 firmware (`src/main.cpp`)** – keeps a persistent WebSocket link to the backend and translates one-character commands into motor control.

Together they let a driver connect from any browser, see the rover video stream, and steer an ESP32-based car with sub-second latency.

---

## Repository Layout

| Path | Purpose |
| --- | --- |
| `fastApi/app.py` | FastAPI application, WebSocket bridges, REST control endpoints, WebRTC signaling, static file host. |
| `fastApi/public/` | Static HTML/CSS/JS assets for the landing page, car UI, and controller UI. |
| `fastApi/requirements.txt` | Minimal Python dependencies (FastAPI, Uvicorn, websockets). |
| `src/main.cpp` | Arduino/PlatformIO firmware for the ESP32 rover. |
| `platformio.ini` | PlatformIO environment targeting `esp32doit-devkit-v1` with WebSocketsClient dependency. |
| `include/`, `lib/`, `test/` | Standard PlatformIO scaffolding for headers, external libs, and tests. |

---

## Prerequisites

- Python 3.10+ with `pip` (for the FastAPI backend).
- PlatformIO Core (`pip install platformio`) or the VS Code PlatformIO extension for firmware builds.
- ESP32 DevKit (pins expected in `src/main.cpp`) wired to your motor driver.
- Cameras/microphones on both rover and controller machines if you intend to use WebRTC video/audio.

---

## 1. Backend Setup (FastAPI)

```bash
cd fastApi
python -m venv .venv
source .venv/bin/activate          # Windows: .\.venv\Scripts\activate
pip install -r requirements.txt

# Optional: provide STUN/TURN servers for WebRTC (comma-separated)
export STUN_SERVERS="stun:stun.l.google.com:19302,turn:USER:PASS@turn.example.com:3478?transport=udp"

uvicorn app:app --host 0.0.0.0 --port 5000 --reload
```

Key services exposed once Uvicorn is running:

| Endpoint | Type | Description |
| --- | --- | --- |
| `/controller` | HTML | Driver dashboard (WebRTC + control WebSocket). |
| `/car` | HTML | Rover HUD, local media capture, connects as WebRTC “car” peer. |
| `/ws?role=car|ctrl` | WebSocket | WebRTC signaling bridge between the two browsers. |
| `/ctrl` | WebSocket | High-rate command/telemetry channel between controller UI and server. |
| `/esp` | WebSocket | Persistent ESP32 socket (one-character commands and keepalives). |
| `/move`, `/rotate`, `/speed` | POST | Legacy REST fallbacks for motion / rotation / PWM control. |
| `/config`, `/ice-servers` | GET | Current STUN/TURN list for the browser clients. |
| `/health` | GET | Aggregate status (ESP, controller peer, car peer). |

Uvicorn **must** run with a single worker (`--workers 1`) so the ESP32 WebSocket lives in one event loop.

---

## 2. Browser Roles

1. **Car device (`/car`)**
   - Prompts for fullscreen and media permissions.
   - Publishes local camera/microphone, receives driver audio, mirrors status from FastAPI.
   - Toggle settings via the gear icon; HUD stays scrollable with controls for camera, mic, controller audio, etc.

2. **Controller (`/controller`)**
   - Joystick-style UI for forward/backward and steering, speed slider, rotation buttons, and mic/video toggles.
   - Communicates with the backend over `/ctrl` WebSocket for low-latency motion.
   - Negotiates WebRTC with the car peer for the live feed.

Launch sequence:

1. Start the FastAPI server.
2. On the rover host (or attached display), open `http://<server-ip>:5000/car`.
3. On the driver machine, open `http://<server-ip>:5000/controller`.
4. Once both peers connect, the signaling bridge notifies them to exchange WebRTC offers/answers automatically.

If you only need the static landing page, use `http://<server-ip>:5000/`.

---

## 3. ESP32 Firmware (PlatformIO)

Edit the Wi-Fi credentials and backend address at the top of `src/main.cpp`:

```cpp
const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "super-secret";
const char* FLASK_HOST    = "192.168.1.42"; // IP of the FastAPI server
const uint16_t FLASK_PORT = 5000;
const char* FLASK_PATH    = "/esp";
```

> The comments still reference “Flask”; the actual backend is FastAPI but the WebSocket path stays `/esp`.

Build and upload:

```bash
pio run                # compile
pio run -t upload      # flash (board must be connected over USB)
pio device monitor     # open serial monitor @115200 baud
```

Firmware behavior:

- Connects to Wi-Fi, then to `ws://FLASK_HOST:FLASK_PORT/esp`.
- Receives single-character commands: `F`, `B`, `L`, `R`, `S`, and digits `0`–`9` for PWM speed presets.
- Sends `"ping"` every 5 seconds (handled in `app.py` to keep last-seen timestamps).
- Stops motors automatically on Wi-Fi/WebSocket disconnect.

Motor pins and PWM channels are defined near the top of the file; adjust them to match your motor driver wiring.

---

## 4. WebRTC / ICE Configuration

- Default STUN/TURN list ships inside `app.py`.  
- Override it by exporting `STUN_SERVERS` before starting Uvicorn. Use comma-separated URLs; include credentials inline for TURN (e.g. `turn:username:password@turn.yourdomain.com:3478?transport=udp`).
- The browser UIs call `/config` on load, so updates take effect without rebuilding the front-end.
- For tougher NAT scenarios you can extend `app.py` to load from a JSON file (see `_load_json_ice_servers` helper).

---

## 5. Command & Telemetry Flow

```
Controller UI  →  /ctrl WebSocket  →  FastAPI  →  ESP32 /esp WebSocket  →  Motors
                                   ↘                        ↗
                          status/ack JSON         one-char drive commands
Car UI ↔ Controller UI (browser↔browser media via WebRTC, signaled through /ws)
```

- The controller WebSocket sends `drive`, `stop`, `speed`, and `heartbeat` messages.
- FastAPI keeps the ESP32 speed digit in sync (`DEFAULT_SPEED_DIGIT`, `ROTATION_SPEED_DIGIT`) so rotations temporarily switch to a calibrated PWM and then restore the previous speed.
- `/rotate` exposes pre-calibrated angles (15°, 45°, 60°, 90°) by sleeping the event loop for predefined durations before issuing a stop command.
- `/health` aggregates ESP connection state, controller heartbeat age, car peer presence, and signaling peers for quick diagnostics.

---

## 6. Running the Full Stack Locally

1. Flash the ESP32 firmware and ensure it can reach your development machine’s IP.
2. Start FastAPI on that machine (`uvicorn app:app --host 0.0.0.0 --port 5000 --reload`).
3. On the rover, open `/car`; on the driver device, open `/controller`.
4. Confirm the ESP32 logs show “Connected to ws://<host>:5000/esp” and the FastAPI console prints connection information.
5. Use the controller UI to drive; the car HUD should display live status and video preview.

---

## 7. Troubleshooting

| Symptom | Check |
| --- | --- |
| ESP32 never connects | Verify Wi-Fi credentials/IP in `main.cpp` and ensure port 5000 is reachable. |
| Commands lag | Confirm only one Uvicorn worker, watch for warnings about ESP offline, inspect network latency. |
| WebRTC handshake fails | Open browser dev tools, inspect ICE candidates, make sure both clients can reach `/ws`. Provide TURN URLs if devices are on different NATs. |
| Car HUD buttons unresponsive | Make sure `/ctrl` WebSocket is connected (indicator in controller UI) and ESP32 is online. |

---

## 8. Next Steps

- Harden the signaling channel with authentication/pairing tokens before deploying publicly.
- Store rotation calibration and drivetrain parameters in a config file or database instead of in-code dictionaries.
- Replace hard-coded STUN/TURN defaults with environment-provided JSON in production.
- Extend the PlatformIO project with sensor feedback (IMU, encoders) and surface telemetry in the controller UI.

---

Made with FastAPI, WebRTC, and a lot of glow effects. Happy driving! 🛻💡
