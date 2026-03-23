#!/usr/bin/env python3
"""
OCPP 2.0.1 Cloud Controller — Security Profile 3 (WSS / Mutual TLS)
ADAPTED FOR: phyBOARD-Lyra AM62xx
═══════════════════════════════════════════════════════════════════════
Lyra-specific changes vs EVCS-Cube / PhyVerso original:
  1. Default .bin filename → Phytec_Lyra.bin  (no MSPM0 MCU on Lyra)
  2. Default .yaml filename → OCPP201_firmware_lyra.yaml
  3. FirmwareStatusNotification log: mentions RAUC instead of MSPM0_bsl_flasher
  4. All serial port references → /dev/ttyS2 (only working UART at 115200)
  5. UI title updated to show Lyra board

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

import sys, ssl, os, traceback, asyncio, json, logging, threading, queue, hashlib
import mysql.connector, sqlite3
from datetime import datetime

print("=" * 55)
print("  OCPP 2.0.1 Cloud Controller  |  SP3 WSS :9001  |  Lyra AM62xx")
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
VALID_TOKENS = {"DEADBEEF", "test123"}

# ══════════════════════════════════════════════════════════════════════════════
# MariaDB — Charging Session History
# ══════════════════════════════════════════════════════════════════════════════

DB_CONFIG = {
    "host":     "localhost",
    "database": "ocpp_server",
    "user":     "ocpp_user",
    "password": "ocpp1234",
}

TARIFF_RS_PER_MIN = 1.0

def _charger_to_mac(charger_id):
    h = hashlib.md5(charger_id.encode()).hexdigest()
    return ":".join([h[i:i+2].upper() for i in range(0, 12, 2)])

def _db_connect():
    return mysql.connector.connect(**DB_CONFIG)

def _db_init():
    try:
        con = _db_connect()
        cur = con.cursor()
        cur.execute("""
            CREATE TABLE IF NOT EXISTS charging_history (
                id                  INT AUTO_INCREMENT PRIMARY KEY,
                ev_mac_address      VARCHAR(100),
                charging_duration   VARCHAR(20),
                charging_start_dt   DATETIME,
                charging_end_dt     DATETIME,
                start_soc           INT,
                end_soc             INT,
                energy_consumption  FLOAT,
                session_end_reason  VARCHAR(100),
                total_cost          FLOAT,
                gun_id              INT,
                charger_id          VARCHAR(100),
                transaction_id      VARCHAR(100)
            )
        """)
        con.commit()
        cur.close()
        con.close()
        logging.info("MariaDB: charging_history table ready")
    except Exception as e:
        logging.error("MariaDB init error: {}".format(e))

_db_init()

_session = {
    "gun_id":         None,
    "token_id":       "",
    "transaction_id": "",
    "start_time":     None,
    "energy_start":   0.0,
}

def _fmt_duration(seconds):
    h = seconds // 3600
    m = (seconds % 3600) // 60
    s = seconds % 60
    return "{:02d}:{:02d}:{:02d}".format(h, m, s)

def _db_save_session(end_time, energy_end_wh, stop_reason):
    try:
        start_time = _session["start_time"]
        if not start_time:
            return
        fmt = "%Y-%m-%dT%H:%M:%SZ"
        try:
            dur_sec = int((datetime.strptime(end_time, fmt) -
                           datetime.strptime(start_time, fmt)).total_seconds())
        except Exception:
            dur_sec = 0

        energy_start  = round(_session["energy_start"], 3)
        energy_end    = round(energy_end_wh, 3)
        consumed_wh   = round(max(0.0, energy_end - energy_start), 3)
        dur_minutes   = dur_sec / 60.0
        total_cost    = round(dur_minutes * TARIFF_RS_PER_MIN, 2)
        duration_str  = _fmt_duration(dur_sec)
        charger_id    = state.get("charger_id", "unknown")
        ev_mac        = _charger_to_mac(charger_id)
        start_dt = datetime.strptime(start_time, fmt).strftime("%Y-%m-%d %H:%M:%S")
        end_dt   = datetime.strptime(end_time,   fmt).strftime("%Y-%m-%d %H:%M:%S")

        con = _db_connect()
        cur = con.cursor()
        cur.execute("""
            INSERT INTO charging_history
                (ev_mac_address, charging_duration, charging_start_dt, charging_end_dt,
                 start_soc, end_soc, energy_consumption, session_end_reason,
                 total_cost, gun_id, charger_id, transaction_id)
            VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
        """, (
            ev_mac, duration_str, start_dt, end_dt,
            None, None, consumed_wh,
            stop_reason or "Unknown", total_cost,
            _session["gun_id"], charger_id, _session["transaction_id"],
        ))
        con.commit()
        cur.close()
        con.close()
        ui_log("💾 SAVED → MariaDB | mac={} gun={} dur={} energy={}Wh cost=Rs.{}".format(
            ev_mac, _session["gun_id"], duration_str, consumed_wh, total_cost))
    except Exception as e:
        ui_log("⚠ MariaDB save error: {}".format(e), "error")
    finally:
        _session.update({"gun_id": None, "token_id": "", "transaction_id": "",
                         "start_time": None, "energy_start": 0.0})

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
    "fota_status":          "Idle",
    "fota_request_id":      None,
    "ota_req_map":          {},
    "pm_voltage_l1":        "— V",
    "pm_voltage_l2":        "— V",
    "pm_voltage_l3":        "— V",
    "pm_current_l1":        "— A",
    "pm_current_l2":        "— A",
    "pm_current_l3":        "— A",
    "pm_power_active":      "— W",
    "pm_power_reactive":    "— VAR",
    "pm_power_apparent":    "— VA",
    "pm_energy_import":     "— Wh",
    "pm_energy_export":     "— Wh",
    "pm_frequency":         "— Hz",
    "pm_power_factor":      "—",
    "pm_last_update":       None,
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
            evse_info  = payload.get("evse", {})
            active_gun = evse_info.get("id", None)
            state["transaction_id"]   = txn_id
            state["active_evse"]      = active_gun
            state["charging_state"]   = charging or "EVConnected"
            state["status"]           = "Transaction started..."
            ui_log("▶ TRANSACTION STARTED | txn={} gun={}".format(txn_id, active_gun), "warning")
            _session["gun_id"]         = active_gun
            _session["token_id"]       = state.get("token_id", "")
            _session["transaction_id"] = txn_id
            _session["start_time"]     = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
            try:
                _session["energy_start"] = float(
                    state.get("pm_energy_import", "0").split()[0])
            except Exception:
                _session["energy_start"] = 0.0

        elif event_type == "Updated":
            state["charging_state"] = charging or state["charging_state"]
            if charging == "Charging":
                state["coil_on"] = True
                state["status"]  = "⚡ CHARGING — COIL ON"
                ui_log("⚡ CHARGING ACTIVE — COIL ON ⚡", "warning")
            else:
                state["coil_on"] = False
                if charging in ("SuspendedEV", "SuspendedEVSE"):
                    state["status"] = "Suspended — COIL OFF"
                elif not charging:
                    ui_log("⚠ chargingState empty in Updated — forcing COIL OFF", "warning")

        elif event_type == "Ended":
            end_time    = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
            stop_reason = txn_info.get("stoppedReason", "Unknown")
            try:
                energy_end = float(state.get("pm_energy_import", "0").split()[0])
            except Exception:
                energy_end = 0.0
            _db_save_session(end_time, energy_end, stop_reason)
            state.update({
                "transaction_id":       None,
                "active_evse":          None,
                "charging_state":       "Idle",
                "token_authorized":     False,
                "token_id":             "",
                "token_rejected":       False,
                "rejected_token_id":    "",
                "remote_start_pending": False,
                "coil_on":              False,
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

    elif action == "FirmwareStatusNotification":
        fw_status = payload.get("status", "Unknown")
        req_id_fw = payload.get("requestId", "?")
        req_map   = state.get("ota_req_map", {})
        file_type = req_map.get(str(req_id_fw), req_map.get(req_id_fw, "unknown"))

        print("")
        print("[FW-STATUS] ╔══════════════════════════════════════════════╗")
        print("[FW-STATUS] ║  FirmwareStatusNotification from charger     ║")
        print("[FW-STATUS] ╚══════════════════════════════════════════════╝")
        print("[FW-STATUS]   status    : {}".format(fw_status))
        print("[FW-STATUS]   requestId : {}".format(req_id_fw))
        print("[FW-STATUS]   file type : {}".format(file_type))
        print("[FW-STATUS] ──────────────────────────────────────────────")

        state["fota_status"] = fw_status

        level = ("warning" if fw_status in ("Downloading", "Installing", "SignatureVerified")
                 else "error" if fw_status in
                     ("DownloadFailed", "InstallationFailed", "InstallVerificationFailed",
                      "SignatureError", "InvalidSignature")
                 else "info")
        ui_log("🔄 [{type}] FIRMWARE STATUS: {st}  (reqId={rid})".format(
            type=file_type, st=fw_status, rid=req_id_fw), level)

        if fw_status == "Downloading":
            print("[FW-STATUS] Charger is downloading {} to /tmp...".format(file_type))
        elif fw_status == "Downloaded":
            print("[FW-STATUS] {} downloaded to /tmp successfully".format(file_type))
        elif fw_status == "SignatureVerified":
            print("[FW-STATUS] Signature verified ✓ — file is authentic")
            if file_type == ".bin":
                print("[FW-STATUS] systemImpl will now (Lyra AM62xx):")
                print("[FW-STATUS]   → Flash: MSPM0_bsl_flasher flash /dev/ttyS3 /tmp/<file>.bin")
                print("[FW-STATUS]   → Restart same manager after flash")
                print("[FW-STATUS]   → Delete .bin from /tmp")
                ui_log("🔑 .bin SignatureVerified → charger running MSPM0 flash now...", "warning")
            elif file_type == ".yaml":
                print("[FW-STATUS] systemImpl will now:")
                print("[FW-STATUS]   → Stop current EVerest manager")
                print("[FW-STATUS]   → Start manager with new yaml config")
                print("[FW-STATUS]   → Delete .yaml from /tmp")
                ui_log("🔑 .yaml SignatureVerified → charger applying new config...", "warning")
            elif file_type == "both":
                print("[FW-STATUS] systemImpl will now:")
                print("[FW-STATUS]   → MSPM0 FLASH .bin on ttyS3")
                print("[FW-STATUS]   → Stop manager → Start with new .yaml → Delete both")
                ui_log("🔑 Both verified → charger flashing + applying config...", "warning")
        elif fw_status == "Installing":
            print("[FW-STATUS] Charger is installing / applying OTA update...")
        elif fw_status == "Installed":
            print("[FW-STATUS] ✓ OTA COMPLETE — {} installed successfully".format(file_type))
            ui_log("🎉 OTA COMPLETE — {} installed! Charger updated.".format(file_type), "warning")
        elif fw_status in ("DownloadFailed", "InstallationFailed",
                           "InstallVerificationFailed", "SignatureError", "InvalidSignature"):
            print("[FW-STATUS] ✗ OTA FAILED: {}  file={}".format(fw_status, file_type))
            ui_log("❌ OTA FAILED [{type}]: {st} — check charger logs".format(
                type=file_type, st=fw_status), "error")

        print("[FW-STATUS] ══════════════════════════════════════════════")
        print("")
        ui_refresh()
        return {}

    elif action in (
        "SecurityEventNotification", "NotifyReport", "NotifyEvent",
        "NotifyChargingLimit", "LogStatusNotification",
        "ReservationStatusUpdate", "ClearedChargingLimit",
    ):
        return {}

    elif action == "SignCertificate":
        return {"status": "Accepted"}

    elif action == "DataTransfer":
        vendor_id  = payload.get("vendorId",  "")
        message_id = payload.get("messageId", "")
        data_raw   = payload.get("data", "")

        print("[DATA-TRANSFER] Received DataTransfer vendorId={} messageId={}".format(
            vendor_id, message_id))

        if vendor_id == "OTA_MANAGER" and message_id == "CleanTmpResult":
            print("[OTA-CLEAN] CleanTmpResult received from charger")
            try:
                result_data = json.loads(data_raw) if isinstance(data_raw, str) else data_raw
                deleted = result_data.get("deleted", [])
                errors  = result_data.get("errors",  [])
                print("[OTA-CLEAN]   Deleted files : {}".format(deleted))
                print("[OTA-CLEAN]   Errors        : {}".format(errors))
                if deleted:
                    ui_log("🗑  OTA-CLEAN: Deleted from /tmp: {}".format(", ".join(deleted)), "warning")
                else:
                    ui_log("🗑  OTA-CLEAN: No old .bin/.yaml found in /tmp — already clean", "warning")
                if errors:
                    ui_log("⚠  OTA-CLEAN errors: {}".format(", ".join(errors)), "error")
            except Exception as e:
                print("[OTA-CLEAN] Could not parse CleanTmpResult data: {}".format(e))
                ui_log("🗑  OTA-CLEAN result received (raw): {}".format(str(data_raw)[:80]), "warning")

        return {"status": "Accepted"}

    else:
        ui_log("UNHANDLED: {}".format(action))
        return {}


def _parse_meter(mv):
    from datetime import datetime as _dt
    state["pm_last_update"] = _dt.now().strftime("%H:%M:%S")

    for sv in mv.get("sampledValue", []):
        m  = sv.get("measurand", "Energy.Active.Import.Register")
        ph = sv.get("phase", "")
        try:
            val = float(sv.get("value", 0))
        except Exception:
            val = 0

        ui_log("METER  measurand={}  phase={}  val={}".format(m, ph, val))

        if m == "Voltage":
            fmt = "{:.1f} V".format(val)
            if   ph in ("L1", "L1-N"): state["pm_voltage_l1"] = fmt
            elif ph in ("L2", "L2-N"): state["pm_voltage_l2"] = fmt
            elif ph in ("L3", "L3-N"): state["pm_voltage_l3"] = fmt
            else:
                state["pm_voltage_l1"] = fmt
                state["pm_voltage_l2"] = "— V"
                state["pm_voltage_l3"] = "— V"
        elif m in ("Current.Import", "Current.Export"):
            fmt = "{:.2f} A".format(val)
            if   ph == "L1": state["pm_current_l1"] = fmt
            elif ph == "L2": state["pm_current_l2"] = fmt
            elif ph == "L3": state["pm_current_l3"] = fmt
            else:
                state["pm_current_l1"] = fmt
                state["pm_current_l2"] = "— A"
                state["pm_current_l3"] = "— A"
        elif m == "Power.Active.Import":
            fmt = "{:.2f} kW".format(val/1000) if val >= 1000 else "{:.1f} W".format(val)
            state["pm_power_active"] = fmt
            state["power"] = fmt
        elif m == "Power.Reactive.Import":
            state["pm_power_reactive"] = "{:.1f} VAR".format(val)
        elif m == "Power.Apparent.Import":
            state["pm_power_apparent"] = "{:.1f} VA".format(val)
        elif m == "Energy.Active.Import.Register":
            fmt = "{:.3f} kWh".format(val/1000) if val >= 1000 else "{:.1f} Wh".format(val)
            state["pm_energy_import"] = fmt
            state["energy"] = fmt
        elif m == "Energy.Active.Export.Register":
            state["pm_energy_export"] = ("{:.3f} kWh".format(val/1000)
                                         if val >= 1000 else "{:.1f} Wh".format(val))
        elif m == "Frequency":
            state["pm_frequency"] = "{:.2f} Hz".format(val)
        elif m == "Power.Factor":
            state["pm_power_factor"] = "{:.3f}".format(val)


async def post_boot_sequence(charger_id):
    await asyncio.sleep(1)
    ui_log("--- Clearing charging profiles ---", "warning")
    await _send_cmd(charger_id, "ClearChargingProfile", {})
    await asyncio.sleep(1)
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
    await asyncio.sleep(20)
    if state["transaction_id"] is not None:
        ui_log("⚠ WATCHDOG: Ended not received after 20s — force-clearing state", "error")
        state.update({
            "transaction_id":       None,
            "active_evse":          None,
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

    if data[0] == 2:
        msg_id  = data[1]
        action  = data[2]
        payload = data[3] if len(data) > 3 else {}
        resp    = await process_action(action, payload)
        await websocket.send(json.dumps([3, msg_id, resp]))

    elif data[0] == 3:
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
                state["coil_on"]        = False
                state["charging_state"] = "Idle"
                state["status"]         = "Stop accepted — waiting for Ended..."
                ui_log("■ STOP ACCEPTED — COIL FORCED OFF", "warning")
                asyncio.ensure_future(_stop_watchdog())
            ui_refresh()

        elif action == "UpdateFirmware":
            s = result.get("status", "?")
            if s == "Accepted":
                state["fota_status"] = "Downloading"
                ui_log("✓ FIRMWARE UPDATE accepted by charger — downloading…", "warning")
            else:
                state["fota_status"] = "Failed"
                ui_log("✗ FIRMWARE UPDATE rejected: {}".format(s), "error")
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
            "pm_voltage_l1":        "— V",
            "pm_voltage_l2":        "— V",
            "pm_voltage_l3":        "— V",
            "pm_current_l1":        "— A",
            "pm_current_l2":        "— A",
            "pm_current_l3":        "— A",
            "pm_power_active":      "— W",
            "pm_power_reactive":    "— VAR",
            "pm_power_apparent":    "— VA",
            "pm_energy_import":     "— Wh",
            "pm_energy_export":     "— Wh",
            "pm_frequency":         "— Hz",
            "pm_power_factor":      "—",
            "pm_last_update":       None,
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
# FOTA — Firmware Over-The-Air Update
# ══════════════════════════════════════════════════════════════════════════════

import socket
import http.server
import socketserver

FIRMWARE_DIR  = os.path.join(BASE_DIR, "frimware")   # ~/ocpp_server/frimware/
HTTP_FW_PORT  = 8080


def _get_local_ip():
    """Return the machine's LAN IP (not 127.0.0.1)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def _load_cert_pem(path):
    """Read a PEM certificate and return just the base64 body (no headers)."""
    try:
        with open(path, "r") as f:
            data = f.read()
        lines = [l.strip() for l in data.splitlines()
                 if l.strip() and "-----" not in l]
        return "".join(lines)
    except Exception as e:
        ui_log("⚠ Could not read cert {}: {}".format(path, e), "error")
        return None


def _load_cert_pem_full(path):
    """Return the full PEM string (headers included) — needed for signingCertificate."""
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except Exception as e:
        ui_log("⚠ Could not read cert {}: {}".format(path, e), "error")
        return None

# ── FIX 1: The orphaned duplicate _get_local_ip body that was here has been
#    REMOVED. It was a bare try/except block at module level (no def statement)
#    that executed on import, corrupted the socket namespace and caused the
#    segmentation fault. ──────────────────────────────────────────────────────


# ── Built-in firmware HTTP server ─────────────────────────────────────────────
_http_server_instance = None

def start_fw_http_server():
    """Serve files from FIRMWARE_DIR on HTTP_FW_PORT in a daemon thread."""
    global _http_server_instance
    if _http_server_instance:
        return

    if not os.path.isdir(FIRMWARE_DIR):
        ui_log("⚠ Firmware dir not found: {}".format(FIRMWARE_DIR), "error")
        return

    class _Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=FIRMWARE_DIR, **kw)
        def log_message(self, fmt, *args):
            ui_log("HTTP-FW: " + fmt % args)

    try:
        srv = socketserver.TCPServer(("0.0.0.0", HTTP_FW_PORT), _Handler)
        srv.allow_reuse_address = True
        _http_server_instance = srv
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        ip = _get_local_ip()
        ui_log("📂 Firmware HTTP server → http://{}:{}/".format(ip, HTTP_FW_PORT), "warning")
    except OSError as e:
        ui_log("⚠ Could not start firmware HTTP server: {}".format(e), "error")


STATUS_COLORS = {
    "Idle":                      "#94A3B8",
    "Dispatched":                "#38BDF8",
    "Downloading":               "#FACC15",
    "Downloaded":                "#38BDF8",
    "Installing":                "#A78BFA",
    "Installed":                 "#22C55E",
    "InstallationFailed":        "#EF4444",
    "DownloadFailed":            "#EF4444",
    "InstallVerificationFailed": "#EF4444",
    "SignatureError":            "#EF4444",
    "InvalidSignature":          "#EF4444",
    "Failed":                    "#EF4444",
}

STATUS_ICONS = {
    "Idle":                      "⏳",
    "Dispatched":                "📤",
    "Downloading":               "⬇",
    "Downloaded":                "✅",
    "Installing":                "⚙",
    "Installed":                 "🎉",
    "InstallationFailed":        "✗",
    "DownloadFailed":            "✗",
    "InstallVerificationFailed": "✗",
    "Failed":                    "✗",
}

STATUS_HINTS = {
    "Idle":         "Start the HTTP server, then click  ▶  PUSH FIRMWARE.",
    "Dispatched":   "UpdateFirmware sent — awaiting charger acknowledgement…",
    "Downloading":  "Charger is downloading firmware. Watch the log for progress.",
    "Downloaded":   "Download complete — charger will install shortly.",
    "Installing":   "⚠  Installing — do NOT power off the charger!",
    "Installed":    "🎉  Firmware installed! Charger will reboot automatically.",
    "DownloadFailed":     "❌  Download failed. Check URL, HTTP server, and network.",
    "InstallationFailed": "❌  Installation failed. Check charger logs.",
    "InstallVerificationFailed": "❌  Signature / hash check failed.",
    "Failed":       "❌  Firmware update failed. See event log.",
}


class FotaWindow:
    BG     = "#0F172A"
    CARD   = "#1E293B"
    CARD2  = "#111827"
    BORDER = "#334155"
    FG     = "#F8FAFC"
    DIM    = "#94A3B8"
    ACCENT = "#38BDF8"
    GREEN  = "#22C55E"
    RED    = "#EF4444"
    YELLOW = "#FACC15"
    BLUE   = "#3B82F6"
    BLUE2  = "#2563EB"
    INDIGO = "#6366F1"

    def __init__(self, parent_root):
        self.win = tk.Toplevel(parent_root)
        self.win.title("Firmware & Config OTA Update  |  OCPP 2.0.1  |  Lyra AM62xx")
        self.win.configure(bg=self.BG)
        self.win.geometry("820x740")
        self.win.minsize(700, 660)
        self.win.resizable(True, True)
        self.win.lift()
        self.win.focus_force()
        self._http_started = False
        self._build()
        self.win.update_idletasks()
        self._poll()

    def _build(self):
        root = self.win

        # ── Header ────────────────────────────────────────────────────────────
        hdr = tk.Frame(root, bg=self.CARD2)
        hdr.pack(side="top", fill="x")
        tk.Label(hdr, text="  OTA BUNDLE UPDATE  —  Lyra AM62xx",
                 bg=self.CARD2, fg=self.ACCENT,
                 font=("Helvetica Neue", 13, "bold"),
                 pady=12).pack(side="left")
        tk.Label(hdr, text="OCPP 2.0.1  .  UpdateFirmware  ",
                 bg=self.CARD2, fg=self.DIM,
                 font=("Helvetica Neue", 9)).pack(side="right", padx=14)
        tk.Frame(root, bg=self.ACCENT, height=2).pack(side="top", fill="x")

        # ── Footer buttons ────────────────────────────────────────────────────
        foot = tk.Frame(root, bg=self.BG)
        foot.pack(side="bottom", fill="x", padx=20, pady=14)

        btn_row = tk.Frame(foot, bg=self.BG)
        btn_row.pack(fill="x")

        self.clean_btn = tk.Button(
            btn_row, text="CLEAN /tmp on Charger",
            command=self._clean_tmp_on_charger,
            bg="#7C3AED", fg="#FFFFFF",
            activebackground="#5B21B6", activeforeground="#FFFFFF",
            font=("Helvetica Neue", 9, "bold"),
            relief="flat", pady=12, padx=16, cursor="hand2")
        self.clean_btn.pack(side="left", padx=(0, 8))

        self.push_btn = tk.Button(
            btn_row, text="PUSH OTA BUNDLE",
            command=self._push_firmware,
            bg=self.BLUE, fg="#FFFFFF",
            activebackground=self.BLUE2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 11, "bold"),
            relief="flat", pady=12, padx=24, cursor="hand2")
        self.push_btn.pack(side="left", padx=(0, 10))

        tk.Button(btn_row, text="Close",
                  command=self.win.destroy,
                  bg=self.BORDER, fg=self.DIM,
                  activebackground="#475569", activeforeground=self.FG,
                  font=("Helvetica Neue", 10, "bold"),
                  relief="flat", pady=12, padx=16, cursor="hand2"
                  ).pack(side="left")

        # ── Body ──────────────────────────────────────────────────────────────
        body = tk.Frame(root, bg=self.BG)
        body.pack(side="top", fill="both", expand=True, padx=20, pady=(14, 0))

        # ── Section 1: HTTP Server ─────────────────────────────────────────────
        self._section_hdr(body, "1  LOCAL HTTP FIRMWARE SERVER")
        srv_row = tk.Frame(body, bg=self.BG)
        srv_row.pack(fill="x", pady=(4, 0))
        srv_row.grid_columnconfigure(0, weight=1)
        srv_row.grid_columnconfigure(1, weight=0)

        self.lbl_http = tk.Label(srv_row,
            text="HTTP server not started",
            bg=self.CARD, fg=self.DIM,
            font=("Courier New", 9),
            anchor="w", padx=10, pady=8,
            relief="flat",
            highlightbackground=self.BORDER, highlightthickness=1)
        self.lbl_http.grid(row=0, column=0, sticky="ew", padx=(0, 8))

        self.start_http_btn = tk.Button(
            srv_row, text="Start HTTP Server",
            command=self._start_http,
            bg=self.GREEN, fg="#000000",
            activebackground="#16A34A", activeforeground="#FFFFFF",
            font=("Helvetica Neue", 9, "bold"),
            relief="flat", padx=14, pady=8, cursor="hand2")
        self.start_http_btn.grid(row=0, column=1, sticky="e")

        tk.Label(body,
                 text="  Serves files from:  {}".format(FIRMWARE_DIR),
                 bg=self.BG, fg=self.DIM,
                 font=("Helvetica Neue", 8)).pack(anchor="w", pady=(2, 10))

        # ── Section 2: Folder contents preview ────────────────────────────────
        self._section_hdr(body, "2  FOLDER TO SEND  (all files packed into ota_bundle.tar.gz)")

        folder_row = tk.Frame(body, bg=self.BG)
        folder_row.pack(fill="x", pady=(4, 2))

        tk.Label(folder_row,
                 text="  Folder:  {}".format(FIRMWARE_DIR),
                 bg=self.CARD, fg="#34D399",
                 font=("Courier New", 9),
                 anchor="w", padx=10, pady=8,
                 relief="flat",
                 highlightbackground=self.BORDER, highlightthickness=1
                 ).pack(side="left", fill="x", expand=True)

        tk.Button(folder_row, text="Preview Files",
                  command=self._preview_files,
                  bg=self.CARD, fg=self.DIM,
                  activebackground="#334155", activeforeground=self.FG,
                  font=("Helvetica Neue", 9), relief="flat",
                  padx=12, pady=8, cursor="hand2"
                  ).pack(side="left", padx=(8, 0))

        self.lbl_files = tk.Label(body,
                 text="  (click Preview Files to list files in folder)",
                 bg=self.BG, fg=self.DIM,
                 font=("Helvetica Neue", 8), anchor="w", justify="left")
        self.lbl_files.pack(fill="x", padx=4, pady=(2, 10))

        # ── Section 3: Signing Certificate ────────────────────────────────────
        self._section_hdr(body, "3  SIGNING CERTIFICATE  (fw-signing.crt)")
        cert_row = tk.Frame(body, bg=self.BG)
        cert_row.pack(fill="x", pady=(4, 2))
        cert_row.grid_columnconfigure(0, weight=1)
        cert_row.grid_columnconfigure(1, weight=0)
        default_cert = os.path.join(BASE_DIR, "certs", "fw-signing.crt")
        self.cert_var = tk.StringVar(value=default_cert)
        tk.Entry(cert_row, textvariable=self.cert_var,
                 bg=self.CARD, fg="#22D3EE", insertbackground=self.ACCENT,
                 relief="flat", font=("Courier New", 9), bd=0,
                 highlightbackground=self.BORDER, highlightthickness=1
                 ).grid(row=0, column=0, sticky="ew", ipady=7, padx=(0, 8))
        tk.Button(cert_row, text="Browse",
                  command=self._browse_cert,
                  bg=self.BORDER, fg=self.FG,
                  activebackground="#475569", activeforeground=self.FG,
                  font=("Helvetica Neue", 9), relief="flat",
                  padx=10, pady=7, cursor="hand2"
                  ).grid(row=0, column=1, sticky="e")
        tk.Label(body,
                 text="  Use fw-signing.crt (leaf cert signed by MF_ROOT_CA)",
                 bg=self.BG, fg=self.DIM,
                 font=("Helvetica Neue", 8)).pack(anchor="w", pady=(2, 10))

        # ── Section 4: Update Parameters ──────────────────────────────────────
        self._section_hdr(body, "4  UPDATE PARAMETERS")
        params = tk.Frame(body, bg=self.BG)
        params.pack(fill="x", pady=(4, 10))
        for i in range(3):
            params.grid_columnconfigure(i, weight=1)
        labels   = ["Retries", "Retry Interval (s)", "Request ID"]
        defaults = ["3", "60", str(int(datetime.now().strftime("%H%M%S")))]
        self._param_vars = []
        for i, (lbl, val) in enumerate(zip(labels, defaults)):
            tk.Label(params, text=lbl, bg=self.BG, fg=self.DIM,
                     font=("Helvetica Neue", 8, "bold"), anchor="w"
                     ).grid(row=0, column=i, sticky="w",
                            padx=(0 if i == 0 else 8, 0))
            v = tk.StringVar(value=val)
            tk.Entry(params, textvariable=v,
                     bg=self.CARD, fg=self.FG, relief="flat",
                     font=("Helvetica Neue", 10),
                     highlightbackground=self.BORDER, highlightthickness=1
                     ).grid(row=1, column=i, sticky="ew",
                            padx=(0 if i == 0 else 8, 0), ipady=6)
            self._param_vars.append(v)
        self.retries_var, self.interval_var, self.req_id_var = self._param_vars

        # ── Section 5: Firmware Status ─────────────────────────────────────────
        self._section_hdr(body, "5  FIRMWARE STATUS  (live)")
        status_card = tk.Frame(body, bg=self.CARD,
                               highlightbackground=self.BORDER,
                               highlightthickness=1)
        status_card.pack(fill="x", pady=(4, 0))
        inner = tk.Frame(status_card, bg=self.CARD)
        inner.pack(fill="x", padx=14, pady=12)
        self.lbl_fw_icon = tk.Label(inner, text="...", bg=self.CARD, fg=self.DIM,
                                    font=("Helvetica Neue", 24))
        self.lbl_fw_icon.pack(side="left", padx=(0, 14))
        detail = tk.Frame(inner, bg=self.CARD)
        detail.pack(side="left", fill="x", expand=True)
        self.lbl_fw_status = tk.Label(detail, text="Idle — not started",
                                      bg=self.CARD, fg=self.DIM,
                                      font=("Helvetica Neue", 13, "bold"), anchor="w")
        self.lbl_fw_status.pack(anchor="w")
        self.lbl_fw_hint = tk.Label(detail, text=STATUS_HINTS["Idle"],
                                    bg=self.CARD, fg=self.DIM,
                                    font=("Helvetica Neue", 9),
                                    anchor="w", wraplength=580, justify="left")
        self.lbl_fw_hint.pack(anchor="w")

    def _section_hdr(self, parent, text):
        f = tk.Frame(parent, bg=self.CARD2)
        f.pack(fill="x", pady=(6, 0))
        tk.Label(f, text="  " + text,
                 bg=self.CARD2, fg=self.ACCENT,
                 font=("Helvetica Neue", 8, "bold"),
                 pady=5).pack(side="left")
        tk.Frame(parent, bg=self.BORDER, height=1).pack(fill="x")

    def _preview_files(self):
        try:
            if not os.path.isdir(FIRMWARE_DIR):
                self.lbl_files.config(
                    text="  Folder not found: {}".format(FIRMWARE_DIR),
                    fg=self.RED)
                return
            files = [
                f for f in os.listdir(FIRMWARE_DIR)
                if os.path.isfile(os.path.join(FIRMWARE_DIR, f))
                and not f.endswith(".sig")
                and f != "ota_bundle.tar.gz"
            ]
            if not files:
                self.lbl_files.config(
                    text="  No files found in folder — add files first",
                    fg=self.RED)
            else:
                names = "  " + "   |   ".join(
                    "{} ({} bytes)".format(f, os.path.getsize(os.path.join(FIRMWARE_DIR, f)))
                    for f in sorted(files))
                self.lbl_files.config(
                    text=names + "   ->  {} file(s) will be packed".format(len(files)),
                    fg="#34D399")
        except Exception as e:
            self.lbl_files.config(text="  Error: {}".format(e), fg=self.RED)

    def _clean_tmp_on_charger(self):
        if not clients:
            messagebox.showwarning("No Charger",
                "No charger connected.", parent=self.win)
            return
        ui_log("OTA-CLEAN: Sending DataTransfer to delete /tmp/*.bin and /tmp/*.yaml", "warning")
        payload = {
            "vendorId":  "OTA_MANAGER",
            "messageId": "CleanTmp",
            "data":      json.dumps({"path": "/tmp", "patterns": ["*.bin", "*.yaml", "*.tar.gz"]}),
        }
        run_cmd("DataTransfer", payload)
        ui_log("OTA-CLEAN: DataTransfer(CleanTmp) sent", "warning")
        self.clean_btn.config(state="disabled", bg="#334155", text="Clean Sent")
        self.win.after(4000, lambda: self.clean_btn.config(
            state="normal", bg="#7C3AED", fg="#FFFFFF",
            text="CLEAN /tmp on Charger"))

    def _start_http(self):
        start_fw_http_server()
        ip = _get_local_ip()
        self.lbl_http.config(
            text="RUNNING  ->  http://{}:{}/".format(ip, HTTP_FW_PORT),
            fg=self.GREEN)
        self.start_http_btn.config(state="disabled", bg=self.BORDER,
                                   text="Server Running", fg=self.DIM)
        self._http_started = True

    def _browse_cert(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(
            parent=self.win, title="Select Signing Certificate",
            initialdir=os.path.join(BASE_DIR, "certs"),
            filetypes=[("PEM / CRT files", "*.pem *.crt *.cer"), ("All files", "*.*")])
        if path:
            self.cert_var.set(path)

    def _push_firmware(self):
        import base64, subprocess, tarfile

        # ── Checks ────────────────────────────────────────────────────────────
        if not clients:
            messagebox.showwarning("No Charger", "No charger connected.", parent=self.win)
            return
        if not self._http_started:
            if not messagebox.askyesno("HTTP Server Not Started",
                "Local HTTP server not running.\nStart now?", parent=self.win):
                return
            self._start_http()

        cert_path = self.cert_var.get().strip()
        if not cert_path or not os.path.exists(cert_path):
            messagebox.showerror("Certificate Not Found",
                "Signing certificate not found:\n{}".format(cert_path), parent=self.win)
            return
        signing_cert = _load_cert_pem_full(cert_path)
        if not signing_cert:
            return

        try:
            retries  = int(self.retries_var.get().strip())
            interval = int(self.interval_var.get().strip())
            req_id   = int(self.req_id_var.get().strip())
        except ValueError:
            messagebox.showerror("Invalid Input",
                "Retries, interval, request ID must be integers.", parent=self.win)
            return

        # ── Step 1: List all files in frimware/ folder ────────────────────────
        if not os.path.isdir(FIRMWARE_DIR):
            messagebox.showerror("Folder Not Found",
                "Firmware folder not found:\n{}".format(FIRMWARE_DIR), parent=self.win)
            return

        files = [
            f for f in os.listdir(FIRMWARE_DIR)
            if os.path.isfile(os.path.join(FIRMWARE_DIR, f))
            and not f.endswith(".sig")
            and f != "ota_bundle.tar.gz"
        ]

        if not files:
            messagebox.showerror("No Files",
                "No files found in:\n{}\n\nAdd files to the folder first.".format(FIRMWARE_DIR),
                parent=self.win)
            return

        print("[OTA-BUNDLE] Folder: {}".format(FIRMWARE_DIR))
        print("[OTA-BUNDLE] Files to pack:")
        for f in sorted(files):
            print("[OTA-BUNDLE]   + {}  ({} bytes)".format(
                f, os.path.getsize(os.path.join(FIRMWARE_DIR, f))))

        # ── Step 2: Pack all files into ota_bundle.tar.gz ─────────────────────
        bundle_path = os.path.join(FIRMWARE_DIR, "ota_bundle.tar.gz")
        try:
            with tarfile.open(bundle_path, "w:gz") as tar:
                for f in sorted(files):
                    tar.add(os.path.join(FIRMWARE_DIR, f), arcname=f)
            bundle_size = os.path.getsize(bundle_path)
            print("[OTA-BUNDLE] Bundle created: {} ({} bytes)".format(bundle_path, bundle_size))
            ui_log("Bundle packed: {} file(s) -> ota_bundle.tar.gz  ({} bytes)".format(
                len(files), bundle_size), "warning")
        except Exception as e:
            messagebox.showerror("Pack Failed", "Failed to create bundle:\n{}".format(e),
                                 parent=self.win)
            ui_log("Bundle pack failed: {}".format(e), "error")
            return

        # ── Step 3: Sign the bundle ────────────────────────────────────────────
        key_path = os.path.join(BASE_DIR, "certs", "fw-signing.key")
        sig_b64  = None
        if os.path.exists(key_path):
            sig_path = bundle_path + ".sig"
            r = subprocess.run(
                ["openssl", "dgst", "-sha256", "-sign", key_path,
                 "-out", sig_path, bundle_path],
                capture_output=True, text=True)
            if r.returncode == 0:
                with open(sig_path, "rb") as f:
                    sig_b64 = base64.b64encode(f.read()).decode("ascii")
                print("[OTA-BUNDLE] Bundle signed OK -> {}".format(sig_path))
                ui_log("Bundle signed -> ota_bundle.tar.gz.sig", "warning")
            else:
                print("[OTA-BUNDLE] WARNING: signing failed: {}".format(r.stderr))
                ui_log("Bundle signing failed", "error")
        else:
            print("[OTA-BUNDLE] WARNING: fw-signing.key not found - no signature")
            ui_log("fw-signing.key not found - pushing without signature", "error")

        # ── Step 4: Send ONE UpdateFirmware pointing to the bundle ────────────
        ip         = _get_local_ip()
        bundle_url = "http://{}:{}/ota_bundle.tar.gz".format(ip, HTTP_FW_PORT)
        now_iso    = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")

        state["ota_req_map"]     = {req_id: ".tar.gz"}
        state["fota_status"]     = "Dispatched"
        state["fota_request_id"] = req_id

        fw = {"location": bundle_url, "retrieveDateTime": now_iso,
              "signingCertificate": signing_cert}
        if sig_b64:
            fw["signature"] = sig_b64

        payload = {"requestId": req_id, "firmware": fw,
                   "retries": retries, "retryInterval": interval}

        print("[OTA-BUNDLE] Sending UpdateFirmware -> {}".format(bundle_url))
        print("[OTA-BUNDLE] requestId = {}  signed = {}".format(
            req_id, "yes" if sig_b64 else "no"))

        ui_log("OTA BUNDLE -> UpdateFirmware  id={}  url={}".format(
            req_id, bundle_url), "warning")
        run_cmd("UpdateFirmware", payload)

        self.req_id_var.set(str(req_id + 1))
        self.push_btn.config(state="disabled", bg="#334155",
                             text="Waiting for charger...")
        ui_log("Bundle dispatched - waiting for charger...", "warning")

    def _poll(self):
        try:
            fota  = state.get("fota_status", "Idle")
            color = STATUS_COLORS.get(fota, self.DIM)
            icon  = STATUS_ICONS.get(fota, "...")
            hint  = STATUS_HINTS.get(fota, "")
            self.lbl_fw_status.config(text=fota, fg=color)
            self.lbl_fw_icon.config(text=icon, fg=color)
            self.lbl_fw_hint.config(text=hint)
            in_flight = fota in ("Dispatched", "Downloading", "Installing")
            if not in_flight:
                self.push_btn.config(state="normal", bg=self.BLUE,
                                     text="PUSH OTA BUNDLE")
        except tk.TclError:
            return
        self.win.after(500, self._poll)

# ══════════════════════════════════════════════════════════════════════════════
# POWER METER — Live Meter Values Window
# ══════════════════════════════════════════════════════════════════════════════

class PowerMeterWindow:
    BG     = "#0A0F1E"
    CARD   = "#0F1929"
    CARD2  = "#060D18"
    BORDER = "#1A2744"
    FG     = "#E8F4FD"
    DIM    = "#4A6580"
    ACCENT = "#00D4FF"
    GREEN  = "#00FF88"
    YELLOW = "#FFD700"
    ORANGE = "#FF8C00"
    RED    = "#FF3B5C"
    TEAL   = "#00BFA5"
    PURPLE = "#7B61FF"

    def __init__(self, parent_root):
        self.win = tk.Toplevel(parent_root)
        self.win.title("Power Meter — Live Values")
        self.win.configure(bg=self.BG)
        self.win.geometry("900x620")
        self.win.minsize(780, 520)
        self.win.resizable(True, True)
        self.win.lift()
        self.win.focus_force()
        self._build()
        self.win.update_idletasks()
        self._poll()

    def _build(self):
        root = self.win
        hdr = tk.Frame(root, bg=self.CARD2)
        hdr.pack(side="top", fill="x")
        tk.Label(hdr, text="  ⚡  POWER METER",
                 bg=self.CARD2, fg=self.ACCENT,
                 font=("Helvetica Neue", 13, "bold"),
                 pady=12).pack(side="left")
        self.lbl_update = tk.Label(hdr, text="Last update: —",
                                   bg=self.CARD2, fg=self.DIM,
                                   font=("Helvetica Neue", 9))
        self.lbl_update.pack(side="right", padx=16)
        tk.Label(hdr, text="OCPP 2.0.1  ·  MeterValues  ",
                 bg=self.CARD2, fg=self.DIM,
                 font=("Helvetica Neue", 9)).pack(side="right")
        tk.Frame(root, bg=self.ACCENT, height=2).pack(side="top", fill="x")

        foot = tk.Frame(root, bg=self.BG)
        foot.pack(side="bottom", fill="x", padx=20, pady=12)
        self.lbl_live = tk.Label(foot, text="○  Waiting for meter data…",
                                 bg=self.BG, fg=self.DIM, font=("Helvetica Neue", 9))
        self.lbl_live.pack(side="left")
        tk.Button(foot, text="✕  Close", command=self.win.destroy,
                  bg=self.BORDER, fg=self.DIM,
                  activebackground="#1A2744", activeforeground=self.FG,
                  font=("Helvetica Neue", 10, "bold"),
                  relief="flat", pady=10, padx=16, cursor="hand2"
                  ).pack(side="right")

        body = tk.Frame(root, bg=self.BG)
        body.pack(side="top", fill="both", expand=True, padx=16, pady=(12, 0))
        body.grid_columnconfigure(0, weight=1)
        body.grid_columnconfigure(1, weight=1)
        body.grid_columnconfigure(2, weight=1)

        v_card = self._section(body, "⚡  VOLTAGE")
        v_card.grid(row=0, column=0, sticky="nsew", padx=(0, 6), pady=(0, 6))
        self.lbl_v1 = self._metric(v_card, "L1 – N", "— V", self.ACCENT)
        self.lbl_v2 = self._metric(v_card, "L2 – N", "— V", self.ACCENT)
        self.lbl_v3 = self._metric(v_card, "L3 – N", "— V", self.ACCENT)

        i_card = self._section(body, "〰  CURRENT")
        i_card.grid(row=0, column=1, sticky="nsew", padx=(3, 3), pady=(0, 6))
        self.lbl_i1 = self._metric(i_card, "L1", "— A", self.YELLOW)
        self.lbl_i2 = self._metric(i_card, "L2", "— A", self.YELLOW)
        self.lbl_i3 = self._metric(i_card, "L3", "— A", self.YELLOW)

        p_card = self._section(body, "⚙  POWER")
        p_card.grid(row=0, column=2, sticky="nsew", padx=(6, 0), pady=(0, 6))
        self.lbl_pa = self._metric(p_card, "Active",   "— W",   self.GREEN)
        self.lbl_pr = self._metric(p_card, "Reactive", "— VAR", self.PURPLE)
        self.lbl_pp = self._metric(p_card, "Apparent", "— VA",  self.TEAL)

        e_card = self._section(body, "🔋  ENERGY")
        e_card.grid(row=1, column=0, sticky="nsew", padx=(0, 6), pady=(0, 0))
        self.lbl_ei = self._metric(e_card, "Import", "— Wh", self.GREEN)
        self.lbl_ee = self._metric(e_card, "Export", "— Wh", self.ORANGE)

        g_card = self._section(body, "🌐  GRID")
        g_card.grid(row=1, column=1, sticky="nsew", padx=(3, 3), pady=(0, 0))
        self.lbl_freq = self._metric(g_card, "Frequency",    "— Hz", self.TEAL)
        self.lbl_pf   = self._metric(g_card, "Power Factor", "—",    self.YELLOW)

        s_card = self._section(body, "📊  SESSION TOTALS")
        s_card.grid(row=1, column=2, sticky="nsew", padx=(6, 0), pady=(0, 0))
        self.lbl_sess_pwr = self._metric(s_card, "Power Now",      "— W",  self.GREEN)
        self.lbl_sess_nrg = self._metric(s_card, "Session Energy", "— Wh", self.ACCENT)

    def _section(self, parent, title):
        f = tk.Frame(parent, bg=self.CARD,
                     highlightbackground=self.BORDER, highlightthickness=1)
        hdr = tk.Frame(f, bg=self.CARD2)
        hdr.pack(fill="x")
        tk.Label(hdr, text="  " + title, bg=self.CARD2, fg=self.ACCENT,
                 font=("Helvetica Neue", 8, "bold"), pady=6).pack(side="left")
        tk.Frame(f, bg=self.BORDER, height=1).pack(fill="x")
        return f

    def _metric(self, parent, label, default, color):
        row = tk.Frame(parent, bg=self.CARD)
        row.pack(fill="x", padx=12, pady=5)
        tk.Label(row, text=label, bg=self.CARD, fg=self.DIM,
                 font=("Helvetica Neue", 8), width=14, anchor="w").pack(side="left")
        dot = tk.Label(row, text="●", bg=self.CARD, fg=self.DIM,
                       font=("Helvetica Neue", 7))
        dot.pack(side="left", padx=(0, 6))
        val = tk.Label(row, text=default, bg=self.CARD, fg=color,
                       font=("Courier New", 11, "bold"), anchor="e")
        val.pack(side="right")
        val._dot   = dot
        val._color = color
        return val

    def _set(self, lbl, text):
        try:
            old = lbl.cget("text")
            lbl.config(text=text)
            if old != text:
                lbl._dot.config(fg=lbl._color)
                self.win.after(400, lambda: lbl._dot.config(fg=self.DIM))
        except tk.TclError:
            pass

    def _poll(self):
        try:
            self._set(self.lbl_v1,  state["pm_voltage_l1"])
            self._set(self.lbl_v2,  state["pm_voltage_l2"])
            self._set(self.lbl_v3,  state["pm_voltage_l3"])
            self._set(self.lbl_i1,  state["pm_current_l1"])
            self._set(self.lbl_i2,  state["pm_current_l2"])
            self._set(self.lbl_i3,  state["pm_current_l3"])
            self._set(self.lbl_pa,  state["pm_power_active"])
            self._set(self.lbl_pr,  state["pm_power_reactive"])
            self._set(self.lbl_pp,  state["pm_power_apparent"])
            self._set(self.lbl_ei,  state["pm_energy_import"])
            self._set(self.lbl_ee,  state["pm_energy_export"])
            self._set(self.lbl_freq,state["pm_frequency"])
            self._set(self.lbl_pf,  state["pm_power_factor"])
            self._set(self.lbl_sess_pwr, state["power"])
            self._set(self.lbl_sess_nrg, state["energy"])
            ts = state["pm_last_update"]
            if ts:
                self.lbl_update.config(text="Last update: {}".format(ts), fg=self.GREEN)
                self.lbl_live.config(text="● Live data streaming", fg=self.GREEN)
            else:
                self.lbl_update.config(text="Last update: —", fg=self.DIM)
                self.lbl_live.config(
                    text="○  No meter data yet — ensure powermeter is wired in YAML",
                    fg=self.ORANGE)
        except tk.TclError:
            return
        self.win.after(500, self._poll)


# ══════════════════════════════════════════════════════════════════════════════
# Main App
# ══════════════════════════════════════════════════════════════════════════════

class App:
    BG        = "#0F172A"
    BG2       = "#0F172A"
    BG3       = "#1E293B"
    CARD      = "#1E293B"
    CARD2     = "#111827"
    BORDER    = "#334155"
    BORDER2   = "#334155"
    FG        = "#F8FAFC"
    FG2       = "#94A3B8"
    DIM       = "#94A3B8"
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
    TEAL      = "#00BFA5"
    FONT_H    = ("Helvetica Neue", )
    FONT_M    = "TkFixedFont"

    def __init__(self, root):
        self.root = root
        self.root.title("Phytec  |  OCPP 2.0.1 Cloud Controller  |  SP3")
        self.root.configure(bg=self.BG)
        self.root.resizable(True, True)
        try:
            self.root.state("zoomed")
        except Exception:
            try:
                self.root.attributes("-zoomed", True)
            except Exception:
                self.root.update_idletasks()
                sw = self.root.winfo_screenwidth()
                sh = self.root.winfo_screenheight()
                self.root.geometry("{}x{}+0+0".format(sw, sh))

        self.root.update_idletasks()
        self._sw = self.root.winfo_screenwidth()
        self._sh = self.root.winfo_screenheight()
        self.root.bind("<Configure>", self._on_resize)
        self._last_w = 0
        self._build()
        self._start_server()
        self._poll()

    def _scale(self, base, w):
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

    def _build(self):
        self.root.grid_rowconfigure(0, weight=0)
        self.root.grid_rowconfigure(1, weight=0)
        self.root.grid_rowconfigure(2, weight=1)
        self.root.grid_rowconfigure(3, weight=0)
        self.root.grid_columnconfigure(0, weight=1)
        self._build_header()
        self._build_main()
        self._build_footer()
        self._tick()

    def _build_header(self):
        hdr = tk.Frame(self.root, bg=self.CARD2)
        hdr.grid(row=0, column=0, sticky="ew")
        hdr.grid_columnconfigure(0, weight=1)
        hdr.grid_columnconfigure(1, weight=0)
        hdr.grid_columnconfigure(2, weight=1)

        left = tk.Frame(hdr, bg=self.CARD2)
        left.grid(row=0, column=0, sticky="w", padx=(14, 0), pady=8)
        self.pill_server  = self._pill(left, "WSS SERVER", "● OFFLINE")
        self.pill_server.pack(side="left", padx=5)
        self.pill_charger = self._pill(left, "CHARGER", "NOT CONNECTED")
        self.pill_charger.pack(side="left", padx=5)
        self.pill_tls     = self._pill(left, "TLS", "SP3 mTLS")
        self.pill_tls.pack(side="left", padx=5)

        centre = tk.Frame(hdr, bg=self.CARD2)
        centre.grid(row=0, column=1, sticky="ns")
        logo_canvas = tk.Canvas(centre, bg=self.CARD2, highlightthickness=0,
                                width=130, height=32)
        logo_canvas.pack(pady=(6, 0))
        self._draw_phytec_logo(logo_canvas)
        self.lbl_title = tk.Label(centre, text="OCPP CLOUD CONTROLLER",
                                  bg=self.CARD2, fg=self.ACCENT,
                                  font=("Helvetica Neue", 9, "bold"), anchor="center")
        self.lbl_title.pack(pady=(0, 6))

        right = tk.Frame(hdr, bg=self.CARD2)
        right.grid(row=0, column=2, sticky="e", padx=(0, 16))

        fw_btn = tk.Frame(right, bg="#1A1033",
                          highlightbackground="#6366F1", highlightthickness=1,
                          cursor="hand2")
        fw_btn.pack(side="left", padx=(0, 14))
        fw_btn.bind("<Button-1>", lambda e: self._open_fota())

        fw_icon = tk.Label(fw_btn, text="⬆", bg="#1A1033", fg="#A78BFA",
                           font=("Helvetica Neue", 11),
                           padx=8, pady=4, cursor="hand2")
        fw_icon.pack(side="left")
        fw_icon.bind("<Button-1>", lambda e: self._open_fota())

        fw_lbl_top = tk.Frame(fw_btn, bg="#1A1033")
        fw_lbl_top.pack(side="left", padx=(0, 8))
        fw_lbl_top.bind("<Button-1>", lambda e: self._open_fota())

        tk.Label(fw_lbl_top, text="FIRMWARE", bg="#1A1033", fg="#6366F1",
                 font=("Helvetica Neue", 6, "bold"), cursor="hand2").pack(anchor="w")
        self.lbl_fw_hdr = tk.Label(fw_lbl_top, text="OTA UPDATE", bg="#1A1033",
                                   fg="#A78BFA", font=("Helvetica Neue", 9, "bold"),
                                   cursor="hand2")
        self.lbl_fw_hdr.pack(anchor="w")
        self.lbl_fw_hdr.bind("<Button-1>", lambda e: self._open_fota())

        self.lbl_clock = tk.Label(right, text="", bg=self.CARD2, fg=self.DIM,
                                  font=("Helvetica Neue", 8))
        self.lbl_clock.pack(side="left", expand=True)

        accent = tk.Frame(self.root, bg=self.ACCENT, height=2)
        accent.grid(row=1, column=0, sticky="ew")

    def _draw_phytec_logo(self, canvas):
        W, H = 130, 34
        text = "PHYTEC"
        font = ("Helvetica Neue", 22, "bold")
        cx, cy = W // 2, H // 2
        canvas.create_text(cx+1, cy+2, text=text, font=font, fill="#1A1A1A", anchor="center")
        canvas.create_text(cx,   cy+1, text=text, font=font, fill="#7A7A7A", anchor="center")
        canvas.create_text(cx,   cy,   text=text, font=font, fill="#B8B8B8", anchor="center")
        canvas.create_text(cx,   cy-1, text=text, font=font, fill="#DCDCDC", anchor="center")
        canvas.create_text(cx-1, cy-2, text=text, font=font, fill="#F0F0F0", anchor="center")

    def _pill(self, parent, label, value):
        f = tk.Frame(parent, bg=self.BORDER, padx=10, pady=4)
        tk.Label(f, text=label, bg=self.BORDER, fg=self.DIM,
                 font=("Helvetica Neue", 7, "bold")).pack(anchor="w")
        v = tk.Label(f, text=value, bg=self.BORDER, fg=self.FG2,
                     font=("Helvetica Neue", 9, "bold"))
        v.pack(anchor="w")
        f._val = v
        return f

    def _build_main(self):
        main = tk.Frame(self.root, bg=self.BG)
        main.grid(row=2, column=0, sticky="nsew")
        main.grid_rowconfigure(0, weight=0)
        main.grid_rowconfigure(1, weight=1)
        main.grid_columnconfigure(0, weight=1)
        self._build_kpi_bar(main)
        self._build_body(main)

    def _build_kpi_bar(self, parent):
        bar = tk.Frame(parent, bg=self.CARD, height=72)
        bar.grid(row=0, column=0, sticky="ew")
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

        tk.Frame(parent, bg=self.BORDER, height=1).grid(row=0, column=0, sticky="sew")

    def _build_body(self, parent):
        body = tk.Frame(parent, bg=self.BG)
        body.grid(row=1, column=0, sticky="nsew")
        body.grid_rowconfigure(0, weight=1)
        body.grid_columnconfigure(0, weight=1, minsize=340)
        body.grid_columnconfigure(1, weight=2)
        self._build_left_panel(body)
        self._build_right_panel(body)

    def _build_left_panel(self, parent):
        left = tk.Frame(parent, bg=self.BG)
        left.grid(row=0, column=0, sticky="nsew", padx=(12, 6), pady=10)
        left.grid_rowconfigure(3, weight=0)
        left.grid_rowconfigure(4, weight=1)
        left.grid_columnconfigure(0, weight=1)

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

        auth_card = self._section(left, "AUTHORIZATION")
        auth_card.grid(row=1, column=0, sticky="ew", pady=(0, 6))
        auth_inner = tk.Frame(auth_card, bg=self.CARD)
        auth_inner.pack(fill="x", padx=10, pady=6)
        self.lbl_status = tk.Label(auth_inner, text="Waiting for charger...",
                                   bg=self.CARD, fg=self.DIM,
                                   font=("Helvetica Neue", 10, "bold"),
                                   wraplength=260, justify="center")
        self.lbl_status.pack(padx=6, pady=2)

        cs_card = self._section(left, "SESSION STATE")
        cs_card.grid(row=2, column=0, sticky="ew", pady=(0, 6))
        cs_inner = tk.Frame(cs_card, bg=self.CARD)
        cs_inner.pack(fill="x", padx=10, pady=6)
        self.lbl_state = tk.Label(cs_inner, text="Idle",
                                  bg=self.CARD, fg=self.DIM,
                                  font=("Helvetica Neue", 10, "bold"))
        self.lbl_state.pack()

        ctrl_card = self._section(left, "OPERATOR CONTROL")
        ctrl_card.grid(row=3, column=0, sticky="ew", pady=(0, 6))
        ctrl_inner = tk.Frame(ctrl_card, bg=self.CARD)
        ctrl_inner.pack(fill="x", padx=10, pady=8)

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
            relief="flat", cursor="arrow", pady=10, state="disabled")
        self.start_btn1.grid(row=0, column=0, sticky="ew", padx=(0, 3))

        self.stop_btn1 = tk.Button(
            g1_btns, text="■  Stop Charging",
            command=lambda: self.cmd_stop(1),
            bg=self.BORDER, fg=self.DIM,
            activebackground=self.RED2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 10, "bold"),
            relief="flat", cursor="arrow", pady=10, state="disabled")
        self.stop_btn1.grid(row=0, column=1, sticky="ew", padx=(3, 0))

        tk.Frame(ctrl_inner, bg=self.BORDER, height=1).pack(fill="x", pady=(0, 8))

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
            relief="flat", cursor="arrow", pady=10, state="disabled")
        self.start_btn2.grid(row=0, column=0, sticky="ew", padx=(0, 3))

        self.stop_btn2 = tk.Button(
            g2_btns, text="■  Stop Charging",
            command=lambda: self.cmd_stop(2),
            bg=self.BORDER, fg=self.DIM,
            activebackground=self.RED2, activeforeground="#FFFFFF",
            font=("Helvetica Neue", 10, "bold"),
            relief="flat", cursor="arrow", pady=10, state="disabled")
        self.stop_btn2.grid(row=0, column=1, sticky="ew", padx=(3, 0))

        self.evse_var = tk.IntVar(value=1)

        pm_card = tk.Frame(left, bg=self.CARD,
                           highlightbackground="#00D4FF", highlightthickness=1,
                           cursor="hand2")
        pm_card.grid(row=4, column=0, sticky="new", pady=(0, 0))
        pm_card.bind("<Button-1>", lambda e: self._open_powermeter())

        pm_hdr = tk.Frame(pm_card, bg=self.CARD2)
        pm_hdr.pack(fill="x")
        pm_hdr.bind("<Button-1>", lambda e: self._open_powermeter())
        tk.Label(pm_hdr, text="⚡  POWER METER",
                 bg=self.CARD2, fg=self.ACCENT,
                 font=("Helvetica Neue", 8, "bold"),
                 padx=12, pady=6).pack(side="left")
        tk.Label(pm_hdr, text="LIVE VALUES  ›",
                 bg=self.CARD2, fg=self.DIM,
                 font=("Helvetica Neue", 8)).pack(side="right", padx=10)
        tk.Frame(pm_card, bg=self.ACCENT, height=1).pack(fill="x")

        pm_inner = tk.Frame(pm_card, bg=self.CARD)
        pm_inner.pack(fill="x", padx=10, pady=8)
        pm_inner.bind("<Button-1>", lambda e: self._open_powermeter())
        pm_inner.grid_columnconfigure(0, weight=1)
        pm_inner.grid_columnconfigure(1, weight=1)

        def _pm_row(row, label, key, color):
            tk.Label(pm_inner, text=label, bg=self.CARD, fg=self.DIM,
                     font=("Helvetica Neue", 8), anchor="w").grid(
                         row=row, column=0, sticky="w", pady=2)
            lbl = tk.Label(pm_inner, text="— ", bg=self.CARD, fg=color,
                           font=("Courier New", 9, "bold"), anchor="e")
            lbl.grid(row=row, column=1, sticky="e", pady=2)
            lbl._key = key
            return lbl

        self.lbl_pm_v  = _pm_row(0, "Voltage",  "pm_voltage_l1",   self.ACCENT)
        self.lbl_pm_a  = _pm_row(1, "Current",  "pm_current_l1",   "#FFD700")
        self.lbl_pm_w  = _pm_row(2, "Power",    "pm_power_active", self.GREEN)
        self.lbl_pm_wh = _pm_row(3, "Energy",   "pm_energy_import",self.TEAL)

    def _build_right_panel(self, parent):
        right = tk.Frame(parent, bg=self.BG)
        right.grid(row=0, column=1, sticky="nsew", padx=(6, 12), pady=10)
        right.grid_rowconfigure(0, weight=1)
        right.grid_columnconfigure(0, weight=1)

        log_card = self._section(right, "EVENT LOG")
        log_card.grid(row=0, column=0, sticky="nsew")

        toolbar = tk.Frame(log_card, bg=self.CARD)
        toolbar.pack(fill="x", padx=12, pady=(4, 0))
        for txt, col in (("● INFO", self.DIM), ("● WARN", self.YELLOW), ("● ERROR", self.RED)):
            tk.Label(toolbar, text=txt, bg=self.CARD, fg=col,
                     font=("Helvetica Neue", 8)).pack(side="left", padx=4)
        tk.Button(toolbar, text=" CLEAR ", command=self._clear,
                  bg=self.BLUE, fg="#FFFFFF",
                  activebackground=self.BLUE2, activeforeground="#FFFFFF",
                  font=("Helvetica Neue", 9, "bold"),
                  relief="flat", padx=12, pady=4, cursor="hand2"
                  ).pack(side="right", padx=8)

        log_frame = tk.Frame(log_card, bg=self.CARD)
        log_frame.pack(fill="both", expand=True, padx=12, pady=(6, 12))
        self.log_box = scrolledtext.ScrolledText(
            log_frame, bg="#0B1220", fg="#E2E8F0",
            font=("Courier New", 9), wrap=tk.WORD, state="disabled",
            relief="flat", insertbackground=self.ACCENT,
            selectbackground="#1E40AF", padx=8, pady=6)
        self.log_box.pack(fill="both", expand=True)
        self.log_box.tag_config("info",    foreground="#CBD5E1")
        self.log_box.tag_config("warning", foreground=self.YELLOW)
        self.log_box.tag_config("error",   foreground=self.RED)

    def _build_footer(self):
        sep = tk.Frame(self.root, bg=self.BORDER, height=1)
        sep.grid(row=3, column=0, sticky="ew")
        foot = tk.Frame(self.root, bg=self.CARD2, height=28)
        foot.grid(row=3, column=0, sticky="ew")
        foot.grid_propagate(False)
        foot.grid_columnconfigure(1, weight=1)
        tk.Label(foot, text="  PHYTEC  ·  OCPP 2.0.1 Cloud Controller ",
                 bg=self.CARD2, fg=self.DIM,
                 font=("Helvetica Neue", 8)).grid(row=0, column=0, sticky="w")
        self.lbl_foot_status = tk.Label(foot, text="System initialising...",
                                        bg=self.CARD2, fg=self.DIM,
                                        font=("Helvetica Neue", 8))
        self.lbl_foot_status.grid(row=0, column=2, sticky="e", padx=12)

    def _section(self, parent, title):
        f = tk.Frame(parent, bg=self.CARD,
                     highlightbackground=self.BORDER2, highlightthickness=1)
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

        self.pill_server._val.config(
            text="● ONLINE  :{}".format(SERVER_PORT) if ok else "○ OFFLINE",
            fg=self.GREEN if ok else self.RED)
        self.pill_charger._val.config(
            text=s["charger_id"] if ok else "NOT CONNECTED",
            fg=self.CYAN if ok else self.DIM)

        cs = s["charging_state"]
        cs_color = (self.GREEN  if cs == "Charging"   else
                    self.YELLOW if cs == "EVConnected" else self.DIM)
        self.lbl_kpi_state.config(text=cs, fg=cs_color)
        self.lbl_kpi_charger.config(
            text=s["charger_id"] if ok else "Not Connected",
            fg=self.CYAN if ok else self.DIM)

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
        self.lbl_state.config(text=cs, fg=cs_color)

        txn_active = bool(s["transaction_id"])
        token_ok   = ok and s["token_authorized"] and not txn_active
        g1_occ = (e1 == "Occupied")
        g2_occ = (e2 == "Occupied")
        can_start1 = token_ok and g1_occ
        can_start2 = token_ok and g2_occ

        active_gun = s.get("active_evse")
        if txn_active and active_gun is None:
            can_stop1 = ok
            can_stop2 = ok
        else:
            can_stop1 = ok and txn_active and (active_gun == 1)
            can_stop2 = ok and txn_active and (active_gun == 2)

        self.start_btn1.config(
            state="normal" if can_start1 else "disabled",
            bg=self.GREEN  if can_start1 else self.BORDER,
            fg="#FFFFFF"   if can_start1 else self.DIM,
            cursor="hand2" if can_start1 else "arrow")
        self.stop_btn1.config(
            state="normal" if can_stop1 else "disabled",
            bg=self.RED    if can_stop1 else self.BORDER,
            fg="#FFFFFF"   if can_stop1 else self.DIM,
            cursor="hand2" if can_stop1 else "arrow")
        self.start_btn2.config(
            state="normal" if can_start2 else "disabled",
            bg=self.GREEN  if can_start2 else self.BORDER,
            fg="#FFFFFF"   if can_start2 else self.DIM,
            cursor="hand2" if can_start2 else "arrow")
        self.stop_btn2.config(
            state="normal" if can_stop2 else "disabled",
            bg=self.RED    if can_stop2 else self.BORDER,
            fg="#FFFFFF"   if can_stop2 else self.DIM,
            cursor="hand2" if can_stop2 else "arrow")

        if cs == "Charging":
            ft, fc = "● CHARGING ACTIVE", self.GREEN
        elif ok:
            ft, fc = "● CHARGER CONNECTED", self.CYAN
        else:
            ft, fc = "○ Awaiting charger connection...", self.DIM
        self.lbl_foot_status.config(text=ft, fg=fc)

        fota     = state.get("fota_status", "Idle")
        fw_color = STATUS_COLORS.get(fota, self.DIM)
        fw_icon  = STATUS_ICONS.get(fota, "⏳")
        try:
            self.lbl_fw_hdr.config(
                text="{}  {}".format(fw_icon, fota if fota != "Idle" else "OTA UPDATE"),
                fg=fw_color)
        except Exception:
            pass

        for lbl, key in [(self.lbl_pm_v,  "pm_voltage_l1"),
                         (self.lbl_pm_a,  "pm_current_l1"),
                         (self.lbl_pm_w,  "pm_power_active"),
                         (self.lbl_pm_wh, "pm_energy_import")]:
            lbl.config(text=state.get(key, "— "))

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

    def _open_fota(self):
        FotaWindow(self.root)

    def _open_powermeter(self):
        PowerMeterWindow(self.root)

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
