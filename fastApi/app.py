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
from contextlib import suppress
from typing import Optional

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Query, Body
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

# ══════════════════════════════════════════════════
#  CONFIG
# ══════════════════════════════════════════════════
PORT = int(os.getenv("PORT", "5000"))
BASE = Path(__file__).parent
ICE_JSON_PATH = BASE / "ice_servers.json"
ESP_HEARTBEAT_INTERVAL = 10.0
ESP_HEARTBEAT_TIMEOUT = ESP_HEARTBEAT_INTERVAL * 3
CTRL_HEARTBEAT_INTERVAL = 2.0
CTRL_HEARTBEAT_TIMEOUT = CTRL_HEARTBEAT_INTERVAL * 3
MOTOR_POWER_MAX = 255


def clamp_motor_power(val: int) -> int:
    return max(-MOTOR_POWER_MAX, min(MOTOR_POWER_MAX, int(val)))


def format_motor_mix_command(left: int, right: int) -> str:
    return f"M,{clamp_motor_power(left)},{clamp_motor_power(right)}"

def _parse_stun_servers(raw: str) -> list[str]:
    values = [part.strip() for part in raw.split(",") if part.strip()]
    if values:
        return values
    return [
        "stun:stunserver2025.stunprotocol.org:3478",
        "turn:USERNAME:PASSWORD@turn.example.com:3478?transport=udp",
    ]


def _default_stun_list() -> list[str]:
    return _parse_stun_servers(
        "stun:stunserver2025.stunprotocol.org:3478,turn:USERNAME:PASSWORD@turn.example.com:3478?transport=udp"
    )


def _load_json_ice_servers() -> list[str]:
    if not ICE_JSON_PATH.exists():
        return []
    try:
        data = json.loads(ICE_JSON_PATH.read_text())
    except Exception as exc:
        log.warning("Failed to read %s: %s", ICE_JSON_PATH, exc)
        return []

    entries: list[str] = []
    for server in data.get("iceServers", []):
        if not isinstance(server, dict):
            continue
        urls = server.get("urls")
        if not urls:
            continue
        if isinstance(urls, str):
            urls = [urls]
        username = server.get("username")
        credential = server.get("credential")
        for raw_url in urls:
            url = str(raw_url or "").strip()
            if not url:
                continue
            if url.lower().startswith("turn") and username and credential and "@" not in url:
                scheme, _, rest = url.partition(":")
                if rest:
                    url = f"{scheme}:{username}:{credential}@{rest}"
            entries.append(url)
    return entries


if os.getenv("STUN_SERVERS"):
    STUN_SERVERS = _parse_stun_servers(os.getenv("STUN_SERVERS", ""))
else:
    json_servers = _load_json_ice_servers()
    STUN_SERVERS = json_servers or _default_stun_list()


def split_ice_entries(entries: list[str]) -> tuple[list[str], list[str]]:
    stun: list[str] = []
    turn: list[str] = []
    for entry in entries:
        value = str(entry).strip()
        if not value:
            continue
        if value.lower().startswith("turn"):
            turn.append(value)
        else:
            stun.append(value)
    return stun, turn
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
        self.ws: Optional[WebSocket] = None
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
            if self.ws and self.ws is not ws:
                try:
                    await self.ws.close(code=4401)
                except Exception:
                    pass
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
        if self.connected:
            age = round((time.monotonic() - self.last_seen) * 1000)
            return {"connected": True, "status": "online", "last_seen_ms": age}
        if self.last_seen:
            age = round((time.monotonic() - self.last_seen) * 1000)
        else:
            age = None
        return {"connected": False, "status": "offline", "last_seen_ms": age}


class ControllerManager:
    def __init__(self, esp: ESP32Connection):
        self.esp = esp
        self.ws: Optional[WebSocket] = None
        self.lock = asyncio.Lock()
        self.current_dir: Optional[str] = None
        self.last_seen: float = 0.0
        self._car_online = lambda: False

    def set_car_status_provider(self, provider):
        self._car_online = provider

    async def attach(self, ws: WebSocket):
        previous = None
        async with self.lock:
            if self.ws and self.ws is not ws:
                previous = self.ws
            self.ws = ws
            self.last_seen = time.monotonic()
        if previous:
            await self.stop_drive()
            await self._safe_send({"type": "error", "reason": "replaced"}, previous)
            with suppress(Exception):
                await previous.close(code=4409)

    async def detach(self, ws: WebSocket):
        async with self.lock:
            if self.ws is ws:
                self.ws = None
        await self.stop_drive()

    async def update_heartbeat(self):
        async with self.lock:
            self.last_seen = time.monotonic()

    async def send_status(self):
        payload = {
            "type": "status",
            "esp": self.esp.status(),
            "controller": self.controller_status(),
            "car": {"connected": bool(self._car_online())},
        }
        await self._safe_send(payload)

    def controller_status(self) -> dict:
        if not self.ws:
            age = None
        else:
            age = round((time.monotonic() - self.last_seen) * 1000)
        return {
            "connected": self.ws is not None,
            "last_seen_ms": age,
            "active_dir": self.current_dir,
        }

    async def send_ack(self, cmd: str, status: str, **extra):
        payload = {"type": "ack", "cmd": cmd, "status": status}
        if extra:
            payload.update(extra)
        await self._safe_send(payload)

    async def send_error(self, reason: str, **extra):
        payload = {"type": "error", "reason": reason}
        if extra:
            payload.update(extra)
        await self._safe_send(payload)

    async def set_drive_mix(self, left: int, right: int, label: Optional[str] = None) -> bool:
        await self.stop_drive(send_stop=False)
        sent = await self.esp.send(format_motor_mix_command(left, right))
        if sent:
            self.current_dir = label or f"{clamp_motor_power(left)}/{clamp_motor_power(right)}"
        return sent

    async def stop_drive(self, send_stop: bool = True) -> bool:
        self.current_dir = None
        delivered = not send_stop or self.esp.connected
        if send_stop and self.esp.connected:
            delivered = await self.esp.send("S")
        return delivered

    async def handle_controller_disconnect(self):
        await self.stop_drive()

    async def handle_esp_drop(self):
        await self.stop_drive(send_stop=False)
        await self.send_status()

    async def _safe_send(self, payload: dict, ws: Optional[WebSocket] = None):
        target = ws or self.ws
        if not target:
            return False
        try:
            await target.send_text(json.dumps(payload))
            return True
        except Exception:
            return False


esp32 = ESP32Connection()
controller = ControllerManager(esp32)


# ──────────────────────────────────────────────────
#  WebRTC signaling room  (browser ↔ browser)
# ──────────────────────────────────────────────────
class SignalingRoom:
    def __init__(self):
        self.peers: dict[str, WebSocket] = {}
        self.lock = asyncio.Lock()

    async def join(self, role: str, ws: WebSocket):
        async with self.lock:
            if role in self.peers and self.peers[role] is not ws:
                return False
            self.peers[role] = ws
            return True

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
controller.set_car_status_provider(lambda: room.has("car"))


class SignalingStore:
    def __init__(self):
        self.lock = asyncio.Lock()
        self.offer: Optional[dict] = None
        self.answer: Optional[dict] = None
        self.ice: dict[str, list[dict]] = {"car": [], "ctrl": []}

    async def reset(self):
        async with self.lock:
            self.offer = None
            self.answer = None
            self.ice = {"car": [], "ctrl": []}

    async def set_offer(self, offer: dict):
        async with self.lock:
            self.offer = offer
            self.answer = None
            self.ice = {"car": [], "ctrl": []}

    async def set_answer(self, answer: dict):
        async with self.lock:
            self.answer = answer

    async def append_ice(self, role: str, candidate: dict):
        async with self.lock:
            self.ice.setdefault(role, []).append(candidate)

    async def dump(self) -> dict:
        async with self.lock:
            return {
                "offer": self.offer,
                "answer": self.answer,
                "ice": {k: v[:] for k, v in self.ice.items()},
            }


signaling_store = SignalingStore()


# ──────────────────────────────────────────────────
#  PAGES  (served directly — no Jinja, no template)
# ──────────────────────────────────────────────────
def _read(name: str) -> str:
    path = PUBLIC_DIR / name
    return path.read_text(encoding="utf-8")


@app.get("/controller", response_class=HTMLResponse)
async def controller_page():
    return HTMLResponse(_read("controller.html"))


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
    await controller.send_status()
    log.info("✅ ESP32 connected")

    try:
        while True:
            # The ESP32 sends an app-level heartbeat as a normal text frame.
            # If the heartbeat stops arriving, treat the socket as dead.
            msg = await asyncio.wait_for(ws.receive_text(), timeout=ESP_HEARTBEAT_TIMEOUT)
            esp32.touch()
            if msg == "heartbeat":
                log.debug("ESP32 heartbeat")
                continue
            log.info(f"ESP32 → server: {msg!r}")

    except asyncio.TimeoutError:
        log.warning("ESP32 heartbeat timeout — closing")
        with suppress(Exception):
            await ws.close(code=4410)
    except WebSocketDisconnect:
        log.info("ESP32 disconnected")
    except Exception as e:
        log.warning(f"ESP32 error: {e}")
    finally:
        await esp32.unregister()
        await controller.handle_esp_drop()
        log.info("❌ ESP32 connection removed")


# ──────────────────────────────────────────────────
#  CONTROLLER CHANNEL (WebSocket)
# ──────────────────────────────────────────────────
@app.websocket("/ctrl")
async def controller_endpoint(ws: WebSocket):
    await ws.accept()
    await controller.attach(ws)
    log.info("🎮 Controller connected")
    await controller.send_status()

    async def status_loop():
        try:
            while True:
                await asyncio.sleep(CTRL_HEARTBEAT_INTERVAL)
                if controller.ws is not ws:
                    break
                await controller.send_status()
        except asyncio.CancelledError:
            pass

    status_task = asyncio.create_task(status_loop())

    try:
        while True:
            try:
                raw = await asyncio.wait_for(ws.receive_text(), timeout=CTRL_HEARTBEAT_TIMEOUT)
            except asyncio.TimeoutError:
                await controller.send_error("heartbeat_timeout")
                await ws.close(code=4410)
                break

            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                await controller.send_error("bad_json")
                continue

            await controller.update_heartbeat()
            msg_type = (msg.get("type") or "").lower()

            if msg_type == "heartbeat":
                await controller.send_ack("heartbeat", "ok")
                continue

            if msg_type == "hello":
                await controller.send_ack("hello", "ok", version=msg.get("version"))
                continue

            if msg_type == "stop":
                delivered = await controller.stop_drive()
                status = "ok" if delivered else "error"
                await controller.send_ack("stop", status, delivered=delivered)
                await controller.send_status()
                continue

            if msg_type == "drive_analog":
                left = msg.get("left")
                right = msg.get("right")
                if not isinstance(left, (int, float)) or not isinstance(right, (int, float)):
                    await controller.send_ack("drive_analog", "error", reason="bad_value")
                    continue
                if not esp32.connected:
                    await controller.send_ack("drive_analog", "error", reason="esp_offline")
                    continue
                clamped_left = clamp_motor_power(round(left))
                clamped_right = clamp_motor_power(round(right))
                label = str(msg.get("label") or "").strip().upper() or None
                sent = await controller.set_drive_mix(clamped_left, clamped_right, label=label)
                if not sent:
                    await controller.send_ack("drive_analog", "error", reason="esp_offline")
                    continue
                await controller.send_ack(
                    "drive_analog",
                    "ok",
                    left=clamped_left,
                    right=clamped_right,
                    label=label,
                )
                await controller.send_status()
                continue

            if msg_type == "status":
                await controller.send_status()
                continue

            await controller.send_error("unsupported_type", type=msg.get("type"))

    except WebSocketDisconnect:
        log.info("🎮 Controller disconnected")
    except Exception as e:
        log.info(f"🎮 Controller error: {e}")
    finally:
        status_task.cancel()
        with suppress(asyncio.CancelledError):
            await status_task
        await controller.detach(ws)
        await controller.send_status()


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
    joined = await room.join(role, ws)
    if not joined:
        await ws.send_text(json.dumps({"type": "error", "reason": "role_taken"}))
        await ws.close(code=4409)
        log.info(f"[SIG] {role} refused (role already occupied)")
        return
    log.info(f"[SIG] {role} joined")

    # Notify both sides if room is now complete
    other = "ctrl" if role == "car" else "car"
    if room.has(other):
        await room.notify_ready(role)
        # Tell the existing peer a new peer joined, so car can re-send its offer
        await room.relay(other, {"type": "hello", "client": role})

    try:
        while True:
            raw = await ws.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if "type" not in msg:
                msg["type"] = "custom"
            await room.relay(role, msg)

    except WebSocketDisconnect:
        log.info(f"[SIG] {role} disconnected")
    except Exception as e:
        log.info(f"[SIG] {role} error: {e}")
    finally:
        await room.leave(role, ws)
        await room.relay(role, {"type": "peer_left", "role": role})


# ──────────────────────────────────────────────────
#  WebRTC signaling REST shims (fallback for legacy clients)
# ──────────────────────────────────────────────────
@app.post("/sig/reset")
async def sig_reset():
    await signaling_store.reset()
    return {"ok": True}


@app.post("/sig/offer")
async def sig_offer(offer: dict = Body(...)):
    await signaling_store.set_offer(offer)
    return {"ok": True}


@app.get("/sig/offer")
async def sig_offer_get():
    data = await signaling_store.dump()
    return {"offer": data["offer"]}


@app.post("/sig/answer")
async def sig_answer(answer: dict = Body(...)):
    await signaling_store.set_answer(answer)
    return {"ok": True}


@app.get("/sig/answer")
async def sig_answer_get():
    data = await signaling_store.dump()
    return {"answer": data["answer"]}


@app.post("/sig/ice/{role}")
async def sig_ice_post(role: str, candidate: dict = Body(...)):
    if role not in ("car", "ctrl"):
        return JSONResponse({"error": "bad role"}, status_code=400)
    await signaling_store.append_ice(role, candidate)
    return {"ok": True}


@app.get("/sig/ice/{role}")
async def sig_ice_get(role: str):
    if role not in ("car", "ctrl"):
        return JSONResponse({"error": "bad role"}, status_code=400)
    data = await signaling_store.dump()
    return {"ice": data["ice"].get(role, [])}


@app.get("/config")
async def config():
    return {"stun_servers": STUN_SERVERS}


@app.get("/ice-servers")
async def ice_servers():
    stun, turn = split_ice_entries(STUN_SERVERS)
    return {
        "stun": stun,
        "turn": turn,
        "stun_servers": STUN_SERVERS,
    }


app.mount("/", StaticFiles(directory=BASE / "public", html=True), name="public")
# ──────────────────────────────────────────────────
#  HEALTH
# ──────────────────────────────────────────────────
@app.get("/health")
async def health():
    return {
        "esp32":     esp32.status(),
        "controller": controller.controller_status(),
        "car": {"connected": room.has("car")},
        "sig_peers": list(room.peers.keys()),
    }


# ──────────────────────────────────────────────────
#  ENTRYPOINT
# ──────────────────────────────────────────────────
if __name__ == "__main__":
    import uvicorn
