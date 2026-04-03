"""
app.py  —  Robot Car Server  (FastAPI + uvicorn)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Install:
    pip install fastapi uvicorn[standard] websockets

Run (local):
    uvicorn app:app --host 0.0.0.0 --port 5000

Run (EC2 later — production grade):
    uvicorn app:app --host 0.0.0.0 --port 5000 --workers 1
    (keep workers=1 — ESP32 connection lives in one process)

Control path:
    Browser  →  POST /move  →  FastAPI  →  ESP32 WebSocket
    Fully async end-to-end, no threads, no GIL blocking.
    Latency: unmeasurable on local network (~0.1ms relay)

Signaling path (WebRTC, browser↔browser):
    /ws?role=car|ctrl  —  instant relay, no polling
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
"""

import os, json, asyncio, logging, time
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Query
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

# ══════════════════════════════════════════════════
#  CONFIG
# ══════════════════════════════════════════════════
PORT = int(os.getenv("PORT", "5000"))
BASE = Path(__file__).parent
# ══════════════════════════════════════════════════

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s"
)
log = logging.getLogger("robot")

app = FastAPI(title="Robot Car", version="2.0")
PUBLIC_DIR = BASE / "public"
PUBLIC_DIR.mkdir(exist_ok=True)
# ──────────────────────────────────────────────────
#  ESP32 connection state
#  Single persistent WebSocket from the ESP32.
#  asyncio.Lock instead of threading.Lock — non-blocking.
# ──────────────────────────────────────────────────
class ESP32Connection:
    def __init__(self):
        self.ws: WebSocket | None = None
        self.lock = asyncio.Lock()
        self.last_seen: float = 0.0
        self.connected = False

    async def send(self, cmd: str) -> bool:
        """Push a command to ESP32. Returns False if not connected."""
        async with self.lock:
            if self.ws is None:
                return False
            try:
                await self.ws.send_text(cmd)
                return True
            except Exception as e:
                log.warning(f"ESP32 send error: {e}")
                self.ws = None
                self.connected = False
                return False

    async def register(self, ws: WebSocket):
        async with self.lock:
            self.ws        = ws
            self.connected = True
            self.last_seen = time.monotonic()

    async def unregister(self):
        async with self.lock:
            self.ws        = None
            self.connected = False

    def touch(self):
        self.last_seen = time.monotonic()

    def status(self) -> dict:
        age = round((time.monotonic() - self.last_seen) * 1000) if self.connected else None
        return {"connected": self.connected, "last_seen_ms": age}


esp32 = ESP32Connection()


# ──────────────────────────────────────────────────
#  WebRTC signaling room  (browser ↔ browser)
# ──────────────────────────────────────────────────
class SignalingRoom:
    def __init__(self):
        self.peers: dict[str, WebSocket] = {}
        self.lock = asyncio.Lock()

    async def join(self, role: str, ws: WebSocket):
        async with self.lock:
            self.peers[role] = ws

    async def leave(self, role: str, ws: WebSocket):
        async with self.lock:
            if self.peers.get(role) is ws:
                del self.peers[role]

    async def relay(self, sender: str, payload: dict):
        other = "ctrl" if sender == "car" else "car"
        async with self.lock:
            peer = self.peers.get(other)
        if peer:
            try:
                await peer.send_text(json.dumps(payload))
            except Exception:
                pass

    async def notify_ready(self, new_role: str):
        """Tell both peers that the room is complete."""
        other = "ctrl" if new_role == "car" else "car"
        async with self.lock:
            new_ws   = self.peers.get(new_role)
            other_ws = self.peers.get(other)
        msg = json.dumps({"type": "ready"})
        for ws in (new_ws, other_ws):
            if ws:
                try:
                    await ws.send_text(msg)
                except Exception:
                    pass

    def has(self, role: str) -> bool:
        return role in self.peers


room = SignalingRoom()


# ──────────────────────────────────────────────────
#  PAGES  (served directly — no Jinja, no template)
# ──────────────────────────────────────────────────
def _read(name: str) -> str:
    return (BASE / name).read_text(encoding="utf-8")


@app.get("/car", response_class=HTMLResponse)
async def car_page():
    return HTMLResponse(_read("car.html"))



# ──────────────────────────────────────────────────
#  ESP32 WebSocket endpoint
#  ESP32 connects here and stays connected.
#  ws://laptop_ip:5000/esp
# ──────────────────────────────────────────────────
@app.websocket("/esp")
async def esp_endpoint(ws: WebSocket):
    await ws.accept()
    await esp32.register(ws)
    log.info("✅ ESP32 connected")

    try:
        while True:
            # Receive keepalive pings from ESP32 (every 5s)
            # If ESP32 dies, this raises WebSocketDisconnect
            msg = await ws.receive_text()
            esp32.touch()
            log.info(f"ESP32 → server: {msg!r}")

    except asyncio.TimeoutError:
        log.warning("ESP32 ping timeout — closing")
    except WebSocketDisconnect:
        log.info("ESP32 disconnected")
    except Exception as e:
        log.warning(f"ESP32 error: {e}")
    finally:
        await esp32.unregister()
        log.info("❌ ESP32 connection removed")


# ──────────────────────────────────────────────────
#  CONTROL ENDPOINTS
#  Browser calls these; FastAPI pushes to ESP32 async
# ──────────────────────────────────────────────────
VALID_DIRS = {"F", "B", "L", "R", "S"}
ROTATION_TIMES = {
    15: 0.25,
    45: 0.45,
    60: 0.75,
    90: 1
}
@app.get("/move")
async def move(dir: str = Query(...)):
    d = dir.upper()
    if d not in VALID_DIRS:
        return JSONResponse({"error": "bad dir"}, status_code=400)
    if not esp32.connected:
        return JSONResponse({"error": "ESP32 not connected"}, status_code=503)
    sent = await esp32.send(d)
    if sent:
        return {"ok": True, "cmd": d}
    return JSONResponse({"error": "send failed"}, status_code=502)

@app.get("/rotate")
async def rotate(dir: str = Query(...), deg: int = Query(...)):
    d = dir.upper()
    if d not in ["L", "R"]:
        return JSONResponse({"error": "Invalid rotation direction"}, status_code=400)
    
    if not esp32.connected:
        return JSONResponse({"error": "ESP32 Offline"}, status_code=503)

    # Get the specific timing you mapped for this degree
    duration = ROTATION_TIMES.get(deg, 0.45)
    
    log.info(f"Rotating {d} for {deg}° ({duration}s)")
    
    await esp32.send(d)          # Start motors
    await asyncio.sleep(duration) # Wait the exact time
    await esp32.send("S")          # Stop motors
    
    return {"ok": True, "deg": deg, "duration": duration}
@app.get("/speed")
async def speed(val: int = Query(200)):
    val = max(80, min(255, val))
    # Map 80-255 → single digit "0"-"9" (1-byte WebSocket frame)
    digit = str(round((val - 80) / (255 - 80) * 9))
    await esp32.send(digit)
    return {"ok": True, "speed": val, "cmd": digit}


# ──────────────────────────────────────────────────
#  WebRTC SIGNALING  (browser ↔ browser)
#  ws://host/ws?role=car|ctrl
# ──────────────────────────────────────────────────
@app.websocket("/ws")
async def signaling_ws(ws: WebSocket, role: str = Query(...)):
    if role not in ("car", "ctrl"):
        await ws.close(code=4000)
        return

    await ws.accept()
    await room.join(role, ws)
    log.info(f"[SIG] {role} joined")

    # Notify both sides if room is now complete
    other = "ctrl" if role == "car" else "car"
    if room.has(other):
        await room.notify_ready(role)

    try:
        while True:
            raw = await ws.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if msg.get("type") in ("offer", "answer", "ice"):
                await room.relay(role, msg)

    except WebSocketDisconnect:
        log.info(f"[SIG] {role} disconnected")
    except Exception as e:
        log.info(f"[SIG] {role} error: {e}")
    finally:
        await room.leave(role, ws)
        await room.relay(role, {"type": "bye", "role": role})

app.mount("/", StaticFiles(directory=BASE / "public", html=True), name="public")
# ──────────────────────────────────────────────────
#  HEALTH
# ──────────────────────────────────────────────────
@app.get("/health")
async def health():
    return {
        "esp32":     esp32.status(),
        "sig_peers": list(room.peers.keys()),
    }


# ──────────────────────────────────────────────────
#  ENTRYPOINT
# ──────────────────────────────────────────────────
if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "app:app",
        host="0.0.0.0",
        port=PORT,
        log_level="info",
        # ws="websockets" is the default — fastest WS implementation
    )

