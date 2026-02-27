#!/usr/bin/env python3
"""
OCPP 2.0.1 Cloud Controller — Security Profile 3 (WSS / Mutual TLS)
═══════════════════════════════════════════════════════════════════════
Logic:
  1. Charger connects (mutual TLS)
  2. Server sends SendLocalList marking tokens as Invalid → defeats AuthCache
  3. EV plugs in → DummyTokenProvider sends token
  4. Server validates token against VALID_TOKENS whitelist
     - If token NOT in whitelist → returns "Unknown" → charging blocked forever
     - If token IN whitelist → returns "Blocked" → START button enables
  5. Operator clicks START → RequestStartTransaction sent
  6. On next Authorize (remote start) → server returns Accepted → charging begins
  7. Operator clicks STOP → RequestStopTransaction sent
═══════════════════════════════════════════════════════════════════════
"""

import sys, ssl, os, traceback, asyncio, json, logging, threading, queue
from datetime import datetime

print("=" * 55)
print("  OCPP 2.0.1 Cloud Controller  |  SP3 WSS :9001")
print("=" * 55)

try:
    import tkinter as tk
    from tkinter import scrolledtext, messagebox
    print("[OK] tkinter")
except ImportError:
    print("[FAIL] sudo apt install python3-tk"); sys.exit(1)

try:
    import websockets
    print("[OK] websockets", websockets.__version__)
except ImportError:
    print("[FAIL] pip install websockets"); sys.exit(1)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
logger = logging.getLogger("OCPP")

ui_queue = queue.Queue()

# ── Certificate paths ──────────────────────────────────────────────────────────
BASE_DIR   = os.path.dirname(os.path.abspath(__file__))
CERT_DIR   = os.path.join(BASE_DIR, "certs")
CA_CERT    = os.path.join(CERT_DIR, "root-ca.crt")
SERVER_CRT = os.path.join(CERT_DIR, "server_chain.crt")
SERVER_KEY = os.path.join(CERT_DIR, "server.key")

SERVER_HOST = "0.0.0.0"
SERVER_PORT = 9001
ALLOWED_CN  = "cp001"

# ── Token Whitelist ────────────────────────────────────────────────────────────
# Only these tokens will be allowed to initiate charging.
# Any other token → rejected with "Unknown" immediately.
VALID_TOKENS = {"DEADBEEF", "test123"}

def ui_log(msg, level="info"):
    ui_queue.put(("log", msg, level))

def ui_refresh():
    ui_queue.put(("update",))

# ── Shared State ───────────────────────────────────────────────────────────────
state = {
    "connected":            False,
    "charger_id":           "N/A",
    "status":               "Waiting for charger...",
    "evse1":                "Unknown",
    "evse2":                "Unknown",
    "charging_state":       "Idle",
    "transaction_id":       None,
    "active_evse":          None,   # tracks which gun (1 or 2) owns current txn
    "token_authorized":     False,
    "token_id":             "",
    "token_rejected":       False,
    "rejected_token_id":    "",
    "coil_on":              False,
    "remote_start_pending": False,
    "local_list_sent":      False,
    "energy":               "0 Wh",
    "power":                "0 W",
}

clients = {}
pending = {}
loop    = None


# ── SSL ────────────────────────────────────────────────────────────────────────
def build_ssl_context():
    for p in (CA_CERT, SERVER_CRT, SERVER_KEY):
        if not os.path.exists(p):
            raise FileNotFoundError("Missing cert: {}".format(p))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=SERVER_CRT, keyfile=SERVER_KEY)
    ctx.load_verify_locations(cafile=CA_CERT)
    ctx.verify_mode     = ssl.CERT_REQUIRED
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx

def get_client_cn(websocket):
    try:
        ssl_obj = websocket.transport.get_extra_info("ssl_object") or websocket.socket
        cert = ssl_obj.getpeercert()
        if cert:
            for field in cert.get("subject", ()):
                for key, val in field:
                    if key == "commonName":
                        return val
    except Exception as e:
        logger.warning("CN read error: {}".format(e))
    return None


# ── OCPP Message Handling ──────────────────────────────────────────────────────
async def process_action(action, payload):
    now = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")

    if action == "BootNotification":
        cs = payload.get("chargingStation", {})
        state["status"] = "Online — initializing..."
        ui_log("BOOT: {} {}".format(cs.get("vendorName","?"), cs.get("model","?")))
        ui_refresh()
        cid = list(clients.keys())[-1] if clients else None
        if cid:
            asyncio.ensure_future(post_boot_sequence(cid))
        return {"status": "Accepted", "currentTime": now, "interval": 60}

    elif action == "Heartbeat":
        ui_refresh()
        return {"currentTime": now}

    elif action == "StatusNotification":
        evse_id = payload.get("evseId", 0)
        status  = payload.get("connectorStatus", "Unknown")

        if evse_id == 1:
            state["evse1"] = status
        elif evse_id == 2:
            state["evse2"] = status

        if status == "Occupied":
            state["status"] = "EV Connected — waiting for token..."
            ui_log("★ EV PLUGGED IN on Gun{} ★".format(evse_id), "warning")
        elif status == "Available":
            state["token_authorized"]  = False
            state["token_id"]          = ""
            state["token_rejected"]    = False
            state["rejected_token_id"] = ""
            if not state["transaction_id"]:
                state["status"] = "Available — waiting for EV..."

        ui_log("Gun{}: {}".format(evse_id, status))
        ui_refresh()
        return {}

    elif action == "Authorize":
        id_token   = payload.get("idToken", {})
        token_id   = id_token.get("idToken", "UNKNOWN")
        token_type = id_token.get("type", "?")

        # ── Whitelist check ────────────────────────────────────────────────────
        if token_id not in VALID_TOKENS:
            state["token_rejected"]    = True
            state["rejected_token_id"] = token_id
            state["token_authorized"]  = False
            state["status"]            = "⛔ Token '{}' rejected — not authorized".format(token_id)
            ui_log("✗ REJECTED TOKEN: {} ({}) — not in whitelist".format(token_id, token_type), "error")
            ui_refresh()
            return {"idTokenInfo": {"status": "Unknown"}}

        if state.get("remote_start_pending"):
            state["remote_start_pending"] = False
            state["status"] = "⚡ Starting charging..."
            ui_log("AUTH (remote start): {} — ACCEPTED ✓".format(token_id), "warning")
            ui_refresh()
            return {"idTokenInfo": {"status": "Accepted"}}
        else:
            # Valid token on plug-in — block auto-start, wait for operator
            state["token_authorized"]  = True
            state["token_rejected"]    = False
            state["token_id"]          = token_id
            state["status"]            = "Token '{}' valid — press START".format(token_id)
            ui_log("✓ TOKEN ACCEPTED: {} — awaiting operator START".format(token_id), "warning")
            ui_refresh()
            return {"idTokenInfo": {"status": "Blocked"}}

    elif action == "TransactionEvent":
        event_type = payload.get("eventType", "")
        txn_info   = payload.get("transactionInfo", {})
        txn_id     = txn_info.get("transactionId", "")
        charging   = txn_info.get("chargingState", "")

        if event_type == "Started":
            # Capture which EVSE owns this transaction so stop button is
            # only enabled on that gun, not both.
            evse_info = payload.get("evse", {})
            active_gun = evse_info.get("id", None)
            state["transaction_id"]   = txn_id
            state["active_evse"]      = active_gun
            state["charging_state"]   = charging or "EVConnected"
            state["status"]           = "Transaction started..."
            ui_log("▶ TRANSACTION STARTED | txn={} gun={}".format(txn_id, active_gun), "warning")

        elif event_type == "Updated":
            # Use previous state as fallback if charger sends empty chargingState
            state["charging_state"] = charging or state["charging_state"]
            if charging == "Charging":
                state["coil_on"] = True
                state["status"]  = "⚡ CHARGING — COIL ON"
                ui_log("⚡ CHARGING ACTIVE — COIL ON ⚡", "warning")
            else:
                # BUG FIX: coil must go OFF for SuspendedEV, SuspendedEVSE,
                # EVConnected, empty string, or any other non-Charging state.
                # Old code only handled the two Suspended variants, leaving
                # coil ON if firmware sent an unexpected or empty value.
                state["coil_on"] = False
                if charging in ("SuspendedEV", "SuspendedEVSE"):
                    state["status"] = "Suspended — COIL OFF"
                elif not charging:
                    ui_log("⚠ chargingState empty in Updated — forcing COIL OFF", "warning")

        elif event_type == "Ended":
            # ALWAYS unconditionally clear everything on Ended,
            # regardless of chargingState value (may be empty/None from charger)
            state.update({
                "transaction_id":       None,
                "active_evse":          None,   # release gun ownership
                "charging_state":       "Idle",
                "token_authorized":     False,
                "token_id":             "",
                "token_rejected":       False,
                "rejected_token_id":    "",
                "remote_start_pending": False,
                "coil_on":              False,   # always forced OFF
                "status":               "Available — waiting for EV...",
            })
            ui_log("■ TRANSACTION ENDED — COIL OFF ■", "warning")

        for mv in payload.get("meterValue", []):
            _parse_meter(mv)

        ui_log("TXN-{}: chargingState={}".format(event_type, charging))
        ui_refresh()

        resp = {}
        if event_type == "Started":
            resp["idTokenInfo"] = {"status": "Accepted"}
        return resp

    elif action == "MeterValues":
        for mv in payload.get("meterValue", []):
            _parse_meter(mv)
        ui_refresh()
        return {}

    elif action in (
        "SecurityEventNotification", "NotifyReport", "NotifyEvent",
        "FirmwareStatusNotification", "NotifyChargingLimit",
        "LogStatusNotification", "ReservationStatusUpdate", "ClearedChargingLimit",
    ):
        return {}

    elif action == "SignCertificate":
        return {"status": "Accepted"}
    elif action == "DataTransfer":
        return {"status": "Accepted"}
    else:
        ui_log("UNHANDLED: {}".format(action))
        return {}


def _parse_meter(mv):
    for sv in mv.get("sampledValue", []):
        m = sv.get("measurand", "Energy.Active.Import.Register")
        try:
            val = float(sv.get("value", 0))
        except Exception:
            val = 0
        if "Energy.Active.Import" in m:
            state["energy"] = "{:.3f} kWh".format(val/1000) if val > 1000 else "{:.1f} Wh".format(val)
        elif "Power.Active.Import" in m:
            state["power"]  = "{:.2f} kW".format(val/1000) if val > 1000 else "{:.0f} W".format(val)


async def post_boot_sequence(charger_id):
    await asyncio.sleep(1)
    ui_log("--- Clearing charging profiles ---", "warning")
    await _send_cmd(charger_id, "ClearChargingProfile", {})
    await asyncio.sleep(1)

    # Mark ALL known tokens as Invalid in local list to defeat auth cache.
    # Forces charger to always send Authorize to server on plug-in.
    local_list = [
        {
            "idToken": {"idToken": token, "type": "ISO14443"},
            "idTokenInfo": {
                "status": "Invalid",
                "cacheExpiryDateTime": "2099-01-01T00:00:00Z"
            }
        }
        for token in VALID_TOKENS
    ]
    ui_log("--- Sending local list ({} tokens=Invalid) ---".format(len(local_list)), "warning")
    await _send_cmd(charger_id, "SendLocalList", {
        "versionNumber": 1,
        "updateType": "Full",
        "localAuthorizationList": local_list
    })

    state["status"] = "Ready — plug in EV to begin"
    ui_log("--- Ready. Plug in EV to start ---", "warning")
    ui_refresh()


async def _stop_watchdog():
    """
    Safety net: if the charger accepts STOP but never sends TransactionEvent Ended
    (e.g. power-off-under-load timeout, firmware bug), force-clear state after 20s
    so the UI doesn't stay locked with an active transaction forever.
    """
    await asyncio.sleep(20)
    if state["transaction_id"] is not None:
        ui_log("⚠ WATCHDOG: Ended not received after 20s — force-clearing state", "error")
        state.update({
            "transaction_id":       None,
            "active_evse":          None,   # release gun ownership
            "charging_state":       "Idle",
            "token_authorized":     False,
            "token_id":             "",
            "token_rejected":       False,
            "rejected_token_id":    "",
            "remote_start_pending": False,
            "coil_on":              False,
            "status":               "Available — (watchdog reset)",
        })
        ui_refresh()


# ── WebSocket handlers ─────────────────────────────────────────────────────────
async def handle_message(websocket, raw):
    try:
        data = json.loads(raw)
    except Exception:
        return
    if not isinstance(data, list) or len(data) < 3:
        return

    if data[0] == 2:   # CALL from charger
        msg_id  = data[1]
        action  = data[2]
        payload = data[3] if len(data) > 3 else {}
        resp    = await process_action(action, payload)
        await websocket.send(json.dumps([3, msg_id, resp]))

    elif data[0] == 3: # CALLRESULT from charger
        msg_id = data[1]
        result = data[2] if len(data) > 2 else {}
        action = pending.pop(msg_id, "?")

        if action == "SendLocalList":
            s = result.get("status", "?")
            if s == "Accepted":
                state["local_list_sent"] = True
                ui_log("✓ LocalList accepted — auth cache defeated", "warning")
            else:
                ui_log("⚠ LocalList status: {} — charger may use cached auth".format(s), "error")
            ui_refresh()

        elif action == "RequestStartTransaction":
            s = result.get("status", "?")
            if s == "Accepted":
                ui_log("✓ START ACCEPTED by charger", "warning")
                state["status"] = "Starting charging..."
            else:
                ui_log("✗ START REJECTED: {}".format(s), "error")
                state["token_authorized"]     = True
                state["remote_start_pending"] = False
            ui_refresh()

        elif action == "RequestStopTransaction":
            s = result.get("status", "?")
            ui_log("{} STOP: {}".format("✓" if s=="Accepted" else "✗", s),
                   "warning" if s=="Accepted" else "error")
            if s == "Accepted":
                # BUG FIX: force coil OFF immediately — do not wait for
                # TransactionEvent Ended which can be delayed or never arrive
                # due to firmware bugs or power-off-under-load scenarios.
                state["coil_on"]        = False
                state["charging_state"] = "Idle"
                state["status"]         = "Stop accepted — waiting for Ended..."
                ui_log("■ STOP ACCEPTED — COIL FORCED OFF", "warning")
                # Also start watchdog to fully clear txn state if Ended never comes
                asyncio.ensure_future(_stop_watchdog())
            ui_refresh()

        else:
            ui_log("RESULT [{}]: {}".format(action, json.dumps(result)[:80]))

    elif data[0] == 4:
        msg_id = data[1]
        action = pending.pop(msg_id, "?")
        ui_log("CALL ERROR [{}]: {}".format(action, data), "error")


async def on_connect(websocket):
    client_cn = get_client_cn(websocket)

    if client_cn is None:
        ui_log("✗ REJECTED: No client certificate!", "error")
        await websocket.close(1008, "Client certificate required")
        return

    if client_cn != ALLOWED_CN:
        ui_log("✗ REJECTED: Unknown CN='{}' (expected '{}')".format(client_cn, ALLOWED_CN), "error")
        await websocket.close(1008, "Unauthorized CN")
        return

    try:
        path = websocket.request.path
    except Exception:
        path = "/"
    charger_id = path.strip("/").split("/")[-1] or "Charger"

    state.update({
        "connected":       True,
        "charger_id":      charger_id,
        "status":          "Connected (TLS ✓)",
        "local_list_sent": False,
    })
    clients[charger_id] = websocket

    ui_log("[+] CHARGER CONNECTED id={} CN={}".format(charger_id, client_cn), "warning")
    ui_refresh()

    try:
        async for msg in websocket:
            await handle_message(websocket, msg)
    except Exception as e:
        ui_log("[-] ERROR: {}".format(str(e)[:80]), "error")
    finally:
        clients.pop(charger_id, None)
        # BUG FIX: restore ALL state to its initial/idle values so the UI
        # returns to exactly the same state as when the server first started.
        # Old code had two bugs:
        #   1. Used wrong keys "Gun1"/"Gun2" (should be "evse1"/"evse2") so
        #      the connector badges never reset to "Unknown" on disconnect.
        #   2. Did not reset charger_id, energy, power, or active_evse.
        state.update({
            "connected":            False,
            "charger_id":           "N/A",
            "status":               "Waiting for charger...",
            "evse1":                "Unknown",
            "evse2":                "Unknown",
            "charging_state":       "Idle",
            "transaction_id":       None,
            "active_evse":          None,
            "token_authorized":     False,
            "token_id":             "",
            "token_rejected":       False,
            "rejected_token_id":    "",
            "coil_on":              False,
            "remote_start_pending": False,
            "local_list_sent":      False,
            "energy":               "0 Wh",
            "power":                "0 W",
        })
        ui_log("[-] CHARGER DISCONNECTED")
        ui_refresh()


async def _send_cmd(charger_id, action, payload):
    ws = clients.get(charger_id)
    if not ws:
        ui_log("ERR: No charger connected!", "error")
        return
    msg_id = "cs-{}".format(datetime.now().strftime("%H%M%S%f"))
    pending[msg_id] = action
    await ws.send(json.dumps([2, msg_id, action, payload]))
    ui_log("→ [{}]".format(action))


def run_cmd(action, payload=None):
    if payload is None:
        payload = {}
    if not clients:
        ui_log("ERR: No charger connected!", "error")
        return
    cid = list(clients.keys())[0]
    if loop:
        asyncio.run_coroutine_threadsafe(_send_cmd(cid, action, payload), loop)


def asyncio_thread():
    global loop
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    async def _run():
        try:
            ssl_ctx = build_ssl_context()
            ui_log("✓ TLS context loaded (CA + server cert)")
        except FileNotFoundError as e:
            ui_log("✗ CERT ERROR: {}".format(e), "error")
            return

        try:
            from websockets.asyncio.server import serve
        except ImportError:
            from websockets import serve

        server = await serve(
            on_connect, SERVER_HOST, SERVER_PORT,
            ssl=ssl_ctx, subprotocols=["ocpp2.0.1"],
        )
        ui_log("★ WSS Server listening on :{}".format(SERVER_PORT))
        await server.serve_forever()

    loop.run_until_complete(_run())


# ══════════════════════════════════════════════════════════════════════════════
# CORPORATE FULL-SCREEN RESPONSIVE UI
# ══════════════════════════════════════════════════════════════════════════════
class App:
    # ── Modern Enterprise Dark Theme ──────────────────────────────────────
    BG        = "#0F172A"
    BG2       = "#0F172A"   # same as BG — no more BG2 confusion
    BG3       = "#1E293B"   # alias → use CARD below
    CARD      = "#1E293B"
    CARD2     = "#111827"   # darker card header
    BORDER    = "#334155"
    BORDER2   = "#334155"   # unified border colour
    FG        = "#F8FAFC"
    FG2       = "#94A3B8"
    DIM       = "#94A3B8"   # unified dim colour (was #475569 — too dark to read)
    GREEN     = "#22C55E"
    GREEN2    = "#16A34A"
    RED       = "#EF4444"
    RED2      = "#DC2626"
    YELLOW    = "#FACC15"
    BLUE      = "#3B82F6"
    BLUE2     = "#2563EB"
    CYAN      = "#22D3EE"
    INDIGO    = "#6366F1"
    ACCENT    = "#38BDF8"
    FONT_H    = ("Helvetica Neue", )
    FONT_M    = "TkFixedFont"

    def __init__(self, root):
        self.root = root
        self.root.title("Phytec  |  OCPP 2.0.1 Cloud Controller  |  SP3")
        self.root.configure(bg=self.BG)

        # ── Make truly full-screen & responsive ───────────────────────────
        self.root.resizable(True, True)
        try:
            # Windows / some Linux WMs
            self.root.state("zoomed")
        except Exception:
            try:
                # GTK / most Linux WMs (Ubuntu, Debian, etc.)
                self.root.attributes("-zoomed", True)
            except Exception:
                # Fallback: manually size to screen
                self.root.update_idletasks()
                sw = self.root.winfo_screenwidth()
                sh = self.root.winfo_screenheight()
                self.root.geometry("{}x{}+0+0".format(sw, sh))

        # Detect real screen dimensions
        self.root.update_idletasks()
        self._sw = self.root.winfo_screenwidth()
        self._sh = self.root.winfo_screenheight()

        # Bind resize so fonts / layout scale with window
        self.root.bind("<Configure>", self._on_resize)
        self._last_w = 0

        self._build()
        self._start_server()
        self._poll()

    # ── Responsive scaling helpers ─────────────────────────────────────────
    def _scale(self, base, w):
        """Return a font size scaled to current window width."""
        ratio = max(0.7, min(1.6, w / 1280))
        return max(8, int(base * ratio))

    def _on_resize(self, event):
        if event.widget != self.root:
            return
        w = event.width
        if abs(w - self._last_w) < 10:
            return
        self._last_w = w
        self._rescale_fonts(w)

    def _rescale_fonts(self, w):
        s = self._scale
        try:
            self.lbl_title.config(font=("Helvetica Neue", s(9, w), "bold"))
            # lbl_sub removed — nothing to rescale here
            self.lbl_clock.config(font=("Helvetica Neue", s(8, w)))
            for lbl in self._kpi_vals:
                lbl.config(font=("Helvetica Neue", s(13, w), "bold"))
            for lbl in self._kpi_keys:
                lbl.config(font=("Helvetica Neue", s(7, w)))
            self.lbl_status.config(font=("Helvetica Neue", s(10, w), "bold"))
            self.lbl_state.config(font=("Helvetica Neue", s(10, w), "bold"))
            for lbl in self._evse_val_lbls:
                lbl.config(font=("Helvetica Neue", s(10, w), "bold"))
            for btn in (self.start_btn1, self.stop_btn1,
                        self.start_btn2, self.stop_btn2):
                btn.config(font=("Helvetica Neue", s(10, w), "bold"))
            self.log_box.config(font=("Courier New", s(9, w)))
        except Exception:
            pass

    # ── Layout ────────────────────────────────────────────────────────────
    def _build(self):
        self.root.grid_rowconfigure(0, weight=0)   # header
        self.root.grid_rowconfigure(1, weight=0)   # accent stripe
        self.root.grid_rowconfigure(2, weight=1)   # main content
        self.root.grid_rowconfigure(3, weight=0)   # footer
        self.root.grid_columnconfigure(0, weight=1)

        self._build_header()
        self._build_main()
        self._build_footer()

        # start the clock
        self._tick()

    # ── HEADER ────────────────────────────────────────────────────────────
    def _build_header(self):
        # Layout: [pills LEFT] | [PHYTEC + OCPP CLOUD CONTROLLER CENTER] | [clock RIGHT]
        hdr = tk.Frame(self.root, bg=self.CARD2)
        hdr.grid(row=0, column=0, sticky="ew")
        hdr.grid_columnconfigure(0, weight=1)   # left pills expands
        hdr.grid_columnconfigure(1, weight=0)   # centre brand fixed
        hdr.grid_columnconfigure(2, weight=1)   # right clock expands

        # LEFT: status pills
        left = tk.Frame(hdr, bg=self.CARD2)
        left.grid(row=0, column=0, sticky="w", padx=(14, 0), pady=8)

        self.pill_server  = self._pill(left, "WSS SERVER", "● OFFLINE")
        self.pill_server.pack(side="left", padx=5)
        self.pill_charger = self._pill(left, "CHARGER", "NOT CONNECTED")
        self.pill_charger.pack(side="left", padx=5)
        self.pill_tls     = self._pill(left, "TLS", "SP3 mTLS")
        self.pill_tls.pack(side="left", padx=5)

        # CENTRE: PHYTEC logo (line 1) + OCPP CLOUD CONTROLLER (line 2), both centred
        centre = tk.Frame(hdr, bg=self.CARD2)
        centre.grid(row=0, column=1, sticky="ns")

        logo_canvas = tk.Canvas(centre, bg=self.CARD2, highlightthickness=0,
                                width=130, height=32)
        logo_canvas.pack(pady=(6, 0))
        self._draw_phytec_logo(logo_canvas)

        self.lbl_title = tk.Label(
            centre, text="OCPP CLOUD CONTROLLER",
            bg=self.CARD2, fg=self.ACCENT,
            font=("Helvetica Neue", 9, "bold"), anchor="center",
        )
        self.lbl_title.pack(pady=(0, 6))

        # RIGHT: clock
        right = tk.Frame(hdr, bg=self.CARD2)
        right.grid(row=0, column=2, sticky="e", padx=(0, 16))
        self.lbl_clock = tk.Label(right, text="", bg=self.CARD2, fg=self.DIM,
                                  font=("Helvetica Neue", 8))
        self.lbl_clock.pack(expand=True)

        # Accent stripe at very bottom of header
        accent = tk.Frame(self.root, bg=self.ACCENT, height=2)
        accent.grid(row=1, column=0, sticky="ew")

    def _draw_phytec_logo(self, canvas):
        W, H = 130, 34
        text  = "PHYTEC"
        font  = ("Helvetica Neue", 22, "bold")
        cx, cy = W // 2, H // 2
        canvas.create_text(cx+1, cy+2, text=text, font=font, fill="#1A1A1A", anchor="center")
        canvas.create_text(cx,   cy+1, text=text, font=font, fill="#7A7A7A", anchor="center")
        canvas.create_text(cx,   cy,   text=text, font=font, fill="#B8B8B8", anchor="center")
        canvas.create_text(cx,   cy-1, text=text, font=font, fill="#DCDCDC", anchor="center")
        canvas.create_text(cx-1, cy-2, text=text, font=font, fill="#F0F0F0", anchor="center")

    def _pill(self, parent, label, value):
        """Returns a composite frame acting as a status pill."""
        f = tk.Frame(parent, bg=self.BORDER, padx=10, pady=4)
        tk.Label(f, text=label, bg=self.BORDER, fg=self.DIM,
                 font=("Helvetica Neue", 7, "bold")).pack(anchor="w")
        v = tk.Label(f, text=value, bg=self.BORDER, fg=self.FG2,
                     font=("Helvetica Neue", 9, "bold"))
        v.pack(anchor="w")
        f._val = v
        return f

    # ── MAIN CONTENT ──────────────────────────────────────────────────────
    def _build_main(self):
        main = tk.Frame(self.root, bg=self.BG)
        main.grid(row=2, column=0, sticky="nsew")
        main.grid_rowconfigure(0, weight=0)   # KPI bar
        main.grid_rowconfigure(1, weight=1)   # two-column area
        main.grid_columnconfigure(0, weight=1)

        self._build_kpi_bar(main)
        self._build_body(main)

    def _build_kpi_bar(self, parent):
        bar = tk.Frame(parent, bg=self.CARD, height=72)
        bar.grid(row=0, column=0, sticky="ew", padx=0, pady=0)
        bar.grid_propagate(False)

        kpi_data = [
            ("CHARGING STATE",  "lbl_kpi_state",   "Idle",          self.DIM),
            ("CHARGER ID",      "lbl_kpi_charger",  "Not Connected", self.DIM),
            ("GUN STATUS",      "lbl_kpi_evse",     "—",             self.DIM),
            ("TRANSACTION",     "lbl_kpi_txn",      "None",          self.DIM),
            ("TOKEN",           "lbl_kpi_token",    "—",             self.DIM),
        ]

        self._kpi_vals = []
        self._kpi_keys = []

        for i, (key, attr, val, color) in enumerate(kpi_data):
            bar.grid_columnconfigure(i, weight=1)
            cell = tk.Frame(bar, bg=self.CARD)
            cell.grid(row=0, column=i, sticky="nsew", padx=1)

            if i > 0:
                tk.Frame(cell, bg=self.BORDER, width=1).pack(side="left", fill="y")

            inner = tk.Frame(cell, bg=self.CARD)
            inner.pack(expand=True, fill="both", padx=10, pady=6)

            k_lbl = tk.Label(inner, text=key, bg=self.CARD, fg=self.DIM,
                             font=("Helvetica Neue", 7), anchor="w")
            k_lbl.pack(anchor="w")
            v_lbl = tk.Label(inner, text=val, bg=self.CARD, fg=color,
                             font=("Helvetica Neue", 13, "bold"), anchor="w")
            v_lbl.pack(anchor="w")

            self._kpi_keys.append(k_lbl)
            self._kpi_vals.append(v_lbl)
            setattr(self, attr, v_lbl)

        # bottom separator
        tk.Frame(parent, bg=self.BORDER, height=1).grid(
            row=0, column=0, sticky="sew")

    def _build_body(self, parent):
        body = tk.Frame(parent, bg=self.BG)
        body.grid(row=1, column=0, sticky="nsew")
        body.grid_rowconfigure(0, weight=1)
        body.grid_columnconfigure(0, weight=1, minsize=340)
        body.grid_columnconfigure(1, weight=2)

        self._build_left_panel(body)
        self._build_right_panel(body)

    # ── LEFT PANEL ────────────────────────────────────────────────────────
    def _build_left_panel(self, parent):
        left = tk.Frame(parent, bg=self.BG)
        left.grid(row=0, column=0, sticky="nsew", padx=(12, 6), pady=10)
        left.grid_rowconfigure(3, weight=1)
        left.grid_columnconfigure(0, weight=1)

        # ── Connector Status ──────────────────────────────────────────────
        evse_card = self._section(left, "CONNECTOR STATUS")
        evse_card.grid(row=0, column=0, sticky="ew", pady=(0, 6))

        self._evse_val_lbls = []
        self._evse_frames   = []

        evse_inner = tk.Frame(evse_card, bg=self.CARD)
        evse_inner.pack(fill="x", padx=10, pady=6)
        evse_inner.grid_columnconfigure(0, weight=1)
        evse_inner.grid_columnconfigure(1, weight=1)

        for i, label in enumerate(("Gun 1", "Gun 2")):
            f = tk.Frame(evse_inner, bg=self.CARD2,
                         highlightbackground=self.BORDER2, highlightthickness=1)
            f.grid(row=0, column=i, sticky="ew",
                   padx=(0, 4) if i == 0 else (4, 0))

            tk.Label(f, text=label, bg=self.CARD2, fg=self.DIM,
                     font=("Helvetica Neue", 8, "bold")).pack(pady=(6, 1))

            dot = tk.Label(f, text="●", bg=self.CARD2, fg=self.DIM,
                           font=("Helvetica Neue", 8))
            dot.pack()

            val = tk.Label(f, text="Unknown", bg=self.CARD2, fg=self.DIM,
                           font=("Helvetica Neue", 10, "bold"))
            val.pack(pady=(1, 6))

            val._dot = dot
            self._evse_val_lbls.append(val)
            self._evse_frames.append(f)

        self.lbl_evse1 = self._evse_val_lbls[0]
        self.lbl_evse2 = self._evse_val_lbls[1]

        # ── Authorization ─────────────────────────────────────────────────
        auth_card = self._section(left, "AUTHORIZATION")
        auth_card.grid(row=1, column=0, sticky="ew", pady=(0, 6))

        auth_inner = tk.Frame(auth_card, bg=self.CARD)
        auth_inner.pack(fill="x", padx=10, pady=6)

        self.lbl_status = tk.Label(auth_inner,
                                   text="Waiting for charger...",
                                   bg=self.CARD, fg=self.DIM,
                                   font=("Helvetica Neue", 10, "bold"),
                                   wraplength=260, justify="center")
        self.lbl_status.pack(padx=6, pady=2)

        # ── Session State ─────────────────────────────────────────────────
        cs_card = self._section(left, "SESSION STATE")
        cs_card.grid(row=2, column=0, sticky="ew", pady=(0, 6))

        cs_inner = tk.Frame(cs_card, bg=self.CARD)
        cs_inner.pack(fill="x", padx=10, pady=6)

        self.lbl_state = tk.Label(cs_inner, text="Idle",
                                  bg=self.CARD, fg=self.DIM,
                                  font=("Helvetica Neue", 10, "bold"))
        self.lbl_state.pack()

        # ── Operator Control ──────────────────────────────────────────────
        ctrl_card = self._section(left, "OPERATOR CONTROL")
        ctrl_card.grid(row=3, column=0, sticky="sew")

        ctrl_inner = tk.Frame(ctrl_card, bg=self.CARD)
        ctrl_inner.pack(fill="x", padx=10, pady=8)

        # ── Gun 1 row ─────────────────────────────────────────────────────
        g1_lbl = tk.Label(ctrl_inner, text="Gun 1", bg=self.CARD, fg=self.DIM,
                          font=("Helvetica Neue", 8, "bold"))
        g1_lbl.pack(anchor="w")

        g1_btns = tk.Frame(ctrl_inner, bg=self.CARD)
        g1_btns.pack(fill="x", pady=(3, 8))
        g1_btns.grid_columnconfigure(0, weight=1)
        g1_btns.grid_columnconfigure(1, weight=1)

        self.start_btn1 = tk.Button(
            g1_btns, text="▶  Start Charging",
            command=lambda: self.cmd_start(1),
            bg=self.BORDER, fg=self.DIM,
            activebackground=self.GREEN2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 10, "bold"),
            relief="flat", cursor="arrow", pady=10, state="disabled"
        )
        self.start_btn1.grid(row=0, column=0, sticky="ew", padx=(0, 3))

        self.stop_btn1 = tk.Button(
            g1_btns, text="■  Stop Charging",
            command=lambda: self.cmd_stop(1),
            bg=self.BORDER, fg=self.DIM,
            activebackground=self.RED2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 10, "bold"),
            relief="flat", cursor="arrow", pady=10, state="disabled"
        )
        self.stop_btn1.grid(row=0, column=1, sticky="ew", padx=(3, 0))

        # ── Divider ───────────────────────────────────────────────────────
        tk.Frame(ctrl_inner, bg=self.BORDER, height=1).pack(fill="x", pady=(0, 8))

        # ── Gun 2 row ─────────────────────────────────────────────────────
        g2_lbl = tk.Label(ctrl_inner, text="Gun 2", bg=self.CARD, fg=self.DIM,
                          font=("Helvetica Neue", 8, "bold"))
        g2_lbl.pack(anchor="w")

        g2_btns = tk.Frame(ctrl_inner, bg=self.CARD)
        g2_btns.pack(fill="x", pady=(3, 0))
        g2_btns.grid_columnconfigure(0, weight=1)
        g2_btns.grid_columnconfigure(1, weight=1)

        self.start_btn2 = tk.Button(
            g2_btns, text="▶  Start Charging",
            command=lambda: self.cmd_start(2),
            bg=self.BORDER, fg=self.DIM,
            activebackground=self.GREEN2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 10, "bold"),
            relief="flat", cursor="arrow", pady=10, state="disabled"
        )
        self.start_btn2.grid(row=0, column=0, sticky="ew", padx=(0, 3))

        self.stop_btn2 = tk.Button(
            g2_btns, text="■  Stop Charging",
            command=lambda: self.cmd_stop(2),
            bg=self.BORDER, fg=self.DIM,
            activebackground=self.RED2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 10, "bold"),
            relief="flat", cursor="arrow", pady=10, state="disabled"
        )
        self.stop_btn2.grid(row=0, column=1, sticky="ew", padx=(3, 0))

        # Keep evse_var for compatibility with cmd_start/cmd_stop
        self.evse_var = tk.IntVar(value=1)

    # ── RIGHT PANEL — Event Log ────────────────────────────────────────────
    def _build_right_panel(self, parent):
        right = tk.Frame(parent, bg=self.BG)
        right.grid(row=0, column=1, sticky="nsew", padx=(6, 12), pady=10)
        right.grid_rowconfigure(0, weight=1)
        right.grid_columnconfigure(0, weight=1)

        log_card = self._section(right, "EVENT LOG")
        log_card.grid(row=0, column=0, sticky="nsew")

        # toolbar inside log header
        toolbar = tk.Frame(log_card, bg=self.CARD)
        toolbar.pack(fill="x", padx=12, pady=(4, 0))

        # tags legend
        for txt, col in (("● INFO", self.DIM), ("● WARN", self.YELLOW),
                          ("● ERROR", self.RED)):
            tk.Label(toolbar, text=txt, bg=self.CARD, fg=col,
                     font=("Helvetica Neue", 8)).pack(side="left", padx=4)

        tk.Button(toolbar,
                  text=" CLEAR ",
                  command=self._clear,
                  bg=self.BLUE,
                  fg="#FFFFFF",
                  activebackground=self.BLUE2,
                  activeforeground="#FFFFFF",
                  font=("Helvetica Neue", 9, "bold"),
                  relief="flat",
                  padx=12,
                  pady=4,
                  cursor="hand2"
        ).pack(side="right", padx=8)

        # log area
        log_frame = tk.Frame(log_card, bg=self.CARD)
        log_frame.pack(fill="both", expand=True, padx=12, pady=(6, 12))

        self.log_box = scrolledtext.ScrolledText(
            log_frame,
            bg="#0B1220",
            fg="#E2E8F0",
            font=("Courier New", 9),
            wrap=tk.WORD,
            state="disabled",
            relief="flat",
            insertbackground=self.ACCENT,
            selectbackground="#1E40AF",
            padx=8,
            pady=6,
        )
        self.log_box.pack(fill="both", expand=True)
        self.log_box.tag_config("info",    foreground="#CBD5E1")
        self.log_box.tag_config("warning", foreground=self.YELLOW)
        self.log_box.tag_config("error",   foreground=self.RED)

    # ── FOOTER ────────────────────────────────────────────────────────────
    def _build_footer(self):
        sep = tk.Frame(self.root, bg=self.BORDER, height=1)
        sep.grid(row=3, column=0, sticky="ew")

        foot = tk.Frame(self.root, bg=self.CARD2, height=28)
        foot.grid(row=3, column=0, sticky="ew")
        foot.grid_propagate(False)
        foot.grid_columnconfigure(1, weight=1)

        tk.Label(foot,
                 text="  PHYTEC  ·  OCPP 2.0.1 Cloud Controller ",
                 bg=self.CARD2, fg=self.DIM,
                 font=("Helvetica Neue", 8)).grid(row=0, column=0, sticky="w")

        self.lbl_foot_status = tk.Label(foot, text="System initialising...",
                                        bg=self.CARD2, fg=self.DIM,
                                        font=("Helvetica Neue", 8))
        self.lbl_foot_status.grid(row=0, column=2, sticky="e", padx=12)

    # ── Helpers ───────────────────────────────────────────────────────────
    def _section(self, parent, title):
        """A titled section card."""
        f = tk.Frame(parent, bg=self.CARD,
                     highlightbackground=self.BORDER2,
                     highlightthickness=1)
        hdr = tk.Frame(f, bg=self.CARD2)
        hdr.pack(fill="x")

        tk.Label(hdr, text=title, bg=self.CARD2, fg=self.ACCENT,
                 font=("Helvetica Neue", 8, "bold"),
                 padx=12, pady=6).pack(side="left")
        tk.Frame(f, bg=self.BORDER, height=1).pack(fill="x")
        return f

    def _tick(self):
        self.lbl_clock.config(text=datetime.now().strftime("%Y-%m-%d   %H:%M:%S  UTC+0"))
        self.root.after(1000, self._tick)

    # ── Poll & Update ─────────────────────────────────────────────────────
    def _poll(self):
        try:
            for _ in range(50):
                item = ui_queue.get_nowait()
                if item[0] == "log":
                    self._log(item[1], item[2])
                elif item[0] == "update":
                    self._update()
        except queue.Empty:
            pass
        self.root.after(100, self._poll)

    def _update(self):
        s  = state
        ok = s["connected"]

        # ── Header pills ──────────────────────────────────────────────────
        self.pill_server._val.config(
            text="● ONLINE  :{}".format(SERVER_PORT) if ok else "○ OFFLINE",
            fg=self.GREEN if ok else self.RED)
        self.pill_charger._val.config(
            text=s["charger_id"] if ok else "NOT CONNECTED",
            fg=self.CYAN if ok else self.DIM)

        # ── KPI bar ───────────────────────────────────────────────────────
        cs = s["charging_state"]
        cs_color = (self.GREEN  if cs == "Charging"   else
                    self.YELLOW if cs == "EVConnected" else self.DIM)
        self.lbl_kpi_state.config(text=cs, fg=cs_color)

        self.lbl_kpi_charger.config(
            text=s["charger_id"] if ok else "Not Connected",
            fg=self.CYAN if ok else self.DIM)

        # Gun Status — full words, no truncation
        e1, e2 = s["evse1"], s["evse2"]
        evse_summary = "G1: {}   G2: {}".format(e1, e2)
        evse_col = (self.GREEN  if "Charging"  in (e1, e2) else
                    self.YELLOW if "Occupied"  in (e1, e2) else
                    self.CYAN   if "Available" in (e1, e2) else self.DIM)
        self.lbl_kpi_evse.config(text=evse_summary, fg=evse_col)

        if s["transaction_id"]:
            self.lbl_kpi_txn.config(text=str(s["transaction_id"])[:18], fg=self.CYAN)
            self.lbl_kpi_token.config(text=s["token_id"] or "—", fg=self.YELLOW)
        else:
            self.lbl_kpi_txn.config(text="None", fg=self.DIM)
            self.lbl_kpi_token.config(
                text=s["token_id"] or "—",
                fg=self.YELLOW if s["token_authorized"] else self.DIM)

        # ── Gun badges — occupied=yellow, everything else=blue/dim ───────
        # Rule: Occupied → yellow border+text; Available → cyan/blue;
        #       Charging → green; Unknown → dim
        evse_cfg = [("evse1", self.lbl_evse1, self._evse_frames[0]),
                    ("evse2", self.lbl_evse2, self._evse_frames[1])]
        for key, lbl, frame in evse_cfg:
            v = s[key]
            if v == "Occupied":
                col, border = self.YELLOW, self.YELLOW
            elif v == "Available":
                col, border = self.BLUE, self.BLUE
            elif v == "Charging":
                col, border = self.GREEN, self.GREEN2
            else:
                col, border = self.DIM, self.BORDER2
            lbl.config(text=v, fg=col)
            lbl._dot.config(fg=col)
            frame.config(highlightbackground=border)

        # ── Auth status label ─────────────────────────────────────────────
        status_text = s["status"]
        if s["token_rejected"]:
            sc = self.RED
        elif s["token_authorized"]:
            sc = self.YELLOW
        elif "CHARGING" in status_text.upper():
            sc = self.GREEN
        elif any(kw in status_text for kw in ("Available", "Ready", "Online", "Connected")):
            sc = self.CYAN
        else:
            sc = self.DIM
        self.lbl_status.config(text=status_text, fg=sc)

        # ── Session state label ───────────────────────────────────────────
        self.lbl_state.config(text=cs, fg=cs_color)

        # ── Per-Gun smart button enable ────────────────────────────────────
        # Start: only enable for the gun that is Occupied AND token is authorized
        #        AND no active transaction
        # Stop:  only enable if there is an active transaction
        txn_active = bool(s["transaction_id"])
        token_ok   = ok and s["token_authorized"] and not txn_active

        g1_occ = (e1 == "Occupied")
        g2_occ = (e2 == "Occupied")

        can_start1 = token_ok and g1_occ
        can_start2 = token_ok and g2_occ

        # BUG FIX: Stop button must only be enabled on the gun that owns the
        # active transaction — not on both guns simultaneously.
        # active_evse is set when TransactionEvent Started arrives and records
        # which evse.id the charger reported. If it is None (charger omitted
        # the evse field, which some firmware does) we fall back to enabling
        # both stop buttons so the operator is never locked out.
        active_gun = s.get("active_evse")
        if txn_active and active_gun is None:
            # Fallback: unknown which gun → enable both (safe default)
            can_stop1 = ok
            can_stop2 = ok
        else:
            can_stop1 = ok and txn_active and (active_gun == 1)
            can_stop2 = ok and txn_active and (active_gun == 2)

        self.start_btn1.config(
            state="normal" if can_start1 else "disabled",
            bg=self.GREEN  if can_start1 else self.BORDER,
            fg="#FFFFFF"   if can_start1 else self.DIM,
            cursor="hand2" if can_start1 else "arrow",
        )
        self.stop_btn1.config(
            state="normal" if can_stop1 else "disabled",
            bg=self.RED    if can_stop1 else self.BORDER,
            fg="#FFFFFF"   if can_stop1 else self.DIM,
            cursor="hand2" if can_stop1 else "arrow",
        )
        self.start_btn2.config(
            state="normal" if can_start2 else "disabled",
            bg=self.GREEN  if can_start2 else self.BORDER,
            fg="#FFFFFF"   if can_start2 else self.DIM,
            cursor="hand2" if can_start2 else "arrow",
        )
        self.stop_btn2.config(
            state="normal" if can_stop2 else "disabled",
            bg=self.RED    if can_stop2 else self.BORDER,
            fg="#FFFFFF"   if can_stop2 else self.DIM,
            cursor="hand2" if can_stop2 else "arrow",
        )

        # ── Footer ────────────────────────────────────────────────────────
        if cs == "Charging":
            ft, fc = "● CHARGING ACTIVE", self.GREEN
        elif ok:
            ft, fc = "● CHARGER CONNECTED", self.CYAN
        else:
            ft, fc = "○ Awaiting charger connection...", self.DIM
        self.lbl_foot_status.config(text=ft, fg=fc)

    def _log(self, msg, level):
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        prefix = {"warning": "[WARN ] ", "error": "[ERROR] "}.get(level, "[INFO ] ")
        self.log_box.configure(state="normal")
        self.log_box.insert("end", "{} {} {}\n".format(ts, prefix, msg), level)
        self.log_box.see("end")
        self.log_box.configure(state="disabled")

    def _clear(self):
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")

    def cmd_start(self, evse=1):
        if not clients:
            messagebox.showwarning("No Charger", "No charger is connected.")
            return
        if not state["token_authorized"]:
            messagebox.showwarning("Token Required",
                                   "No valid token detected.\nPlug in EV and wait for authorization.")
            return
        if state["transaction_id"]:
            messagebox.showwarning("Session Active", "A transaction is already in progress.")
            return
        token = state["token_id"]
        state["remote_start_pending"] = True
        state["token_authorized"]     = False
        ui_refresh()
        ui_log("☁ CLOUD START → Gun{} token={}".format(evse, token), "warning")
        run_cmd("RequestStartTransaction", {
            "idToken":       {"idToken": token, "type": "ISO14443"},
            "evseId":        evse,
            "remoteStartId": int(datetime.now().strftime("%H%M%S"))
        })

    def cmd_stop(self, evse=None):
        txn = state["transaction_id"]
        if not txn:
            messagebox.showwarning("No Session", "No active transaction to stop.")
            return
        ui_log("☁ CLOUD STOP → txn={}".format(txn), "warning")
        run_cmd("RequestStopTransaction", {"transactionId": txn})

    def _start_server(self):
        threading.Thread(target=asyncio_thread, daemon=True).start()

    def on_close(self):
        self.root.destroy()


if __name__ == "__main__":
    try:
        root = tk.Tk()
        app  = App(root)
        root.protocol("WM_DELETE_WINDOW", app.on_close)
        root.mainloop()
    except Exception as e:
        print("[FATAL] {}".format(e))
        traceback.print_exc()
        sys.exit(1)
