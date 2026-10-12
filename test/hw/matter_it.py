#!/usr/bin/env python3
"""Matter integration test on a board running the Matter firmware.

Covers the transitions the host tests cannot reach, because they only
exist with the CHIP stack running:

  1. Cluster creation: the endpoints the configuration asks for exist after
     the reboot -- outputs, inputs, a PWM pad, one sensor per Matter type
     on RS-485 -- with the right device types, and the device boots with
     all of them (no endpoint budget overrun, no SDK assertion).
  2. Reports back to Matter: an output switched over Modbus TCP or CAN, and
     a pad's PWM duty set over Modbus, reach the controller; a Modbus value
     reaches its sensor attribute in the attribute's unit.
  3. Configuration changes with unchanged pins: a changed input invert is
     reported at once, a changed output invert does not move any channel's
     Matter state, an unchanged save asks for no reboot.
  4. Before commissioning nothing answers on Modbus TCP or CAN.

The board is commissioned with chip-tool (fresh storage directory) and
decommissioned again at the end. Its configuration is put back as it was;
NVS (fabrics, counters) is not -- back it up with esptool beforehand if it
matters.

Bench assumptions: an RS-485 device at unit MBM_UNIT whose input registers
0 and 70 hold float32 values (the Eastron SDM630 twin: voltage, frequency),
the board's CAN port on CAN_SSH's can0 at 500 kbit/s (optional), nothing
attached to IO1 (it is driven as PWM).

Environment:
  WS_HOST     board address (required)
  WS_PASS     web password (required; never stored in the repository)
  CHIP_TOOL   path to chip-tool (required)
  MBM_UNIT    RS-485 unit of the test device (default 12)
  CAN_SSH     ssh host with can-utils on can0 (optional; CAN steps skipped)

  python3 test/hw/matter_it.py
"""
import base64, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, urllib.error, urllib.request

HOST = os.environ.get("WS_HOST")
PASS = os.environ.get("WS_PASS")
CT   = os.environ.get("CHIP_TOOL")
UNIT = int(os.environ.get("MBM_UNIT", "12"))
CAN  = os.environ.get("CAN_SSH")
if not (HOST and PASS and CT):
    sys.exit("WS_HOST, WS_PASS and CHIP_TOOL are required")
AUTH = "Basic " + base64.b64encode(("x:" + PASS).encode()).decode()

ok = bad = 0
def check(c, m):
    global ok, bad
    print(("  ok    " if c else "  FEHL  ") + m, flush=True)
    ok += bool(c); bad += (not c)

# ------------------------------------------------------------------ device
def call(method, path, body=None):
    req = urllib.request.Request("http://" + HOST + path, method=method,
                                 headers={"Authorization": AUTH, "Content-Type": "application/json"},
                                 data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, timeout=10) as r: return r.status, r.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()
def get(path): return json.loads(call("GET", path)[1])
def wait_up():
    for _ in range(120):
        try:
            if call("GET", "/api/version")[0] == 200: return
        except Exception: pass
        time.sleep(1)
    sys.exit("board does not come back")
def reboot():
    try: call("POST", "/api/reboot")
    except Exception: pass
    time.sleep(6); wait_up(); time.sleep(12)

# ------------------------------------------------------------------ Modbus TCP
_tid = 0
def recvn(s, n):
    """Exactly n bytes: one recv() may return part of a response."""
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d: raise ConnectionError("connection closed after %d of %d bytes" % (len(b), n))
        b += d
    return b
def mb(pdu, uid=247):
    global _tid
    _tid = (_tid + 1) & 0xFFFF
    with socket.create_connection((HOST, 502), timeout=3) as s:
        s.sendall(struct.pack(">HHHB", _tid, 0, len(pdu) + 1, uid) + pdu)
        tid, proto, length, ruid = struct.unpack(">HHHB", recvn(s, 7))
        if tid != _tid or proto != 0 or ruid != uid or length < 2:
            raise RuntimeError("Modbus header %d/%d/%d/%d" % (tid, proto, length, ruid))
        body = recvn(s, length - 1)   # the length counts the unit id, already read
    if body[0] & 0x80: raise RuntimeError("Modbus exception %d" % body[1])
    return body
def coil(addr, on): mb(struct.pack(">BHH", 5, addr, 0xFF00 if on else 0))
def hreg(addr, v):  mb(struct.pack(">BHH", 6, addr, v))
def port_open():
    try: socket.create_connection((HOST, 502), timeout=2).close(); return True
    except OSError: return False

# ------------------------------------------------------------------ chip-tool
STORE = tempfile.mkdtemp(prefix="matter_it_")
def ct(*args, t=120):
    r = subprocess.run([CT] + list(args) + ["--storage-directory", STORE], capture_output=True, text=True, timeout=t)
    return r.returncode, re.sub(r"\x1b\[[0-9;]*m", "", r.stdout + r.stderr)   # chip-tool colours its log
RETRIES = []
def ct_retry(*args):
    """chip-tool once more when the transaction itself failed (rc != 0):
    on the bench one read in a run now and then does, a different one each
    time. Every retry is listed at the end, so a pattern does not hide."""
    rc, out = ct(*args)
    if rc != 0:
        tail = [l[-160:] for l in out.splitlines() if l.strip()][-3:]
        RETRIES.append((" ".join(args), rc, tail))
        rc, out = ct(*args)
    return rc, out
def read(cluster, attr, ep, key):
    rc, out = ct_retry(cluster, "read", attr, "1", str(ep))
    m = re.findall(r"%s: (\S+)" % key, out)
    if rc == 0 and m: return m[-1]
    print("    (chip-tool %s %s %d: rc %d)\n    " % (cluster, attr, ep, rc)
          + "\n    ".join([l[-160:] for l in out.splitlines() if l.strip()][-6:]), flush=True)
    return None
def until(cluster, attr, ep, key, want, limit=6.0):
    """Seconds until the attribute reads want, or None: a report is
    asynchronous (the change goes through the CHIP task)."""
    t0 = time.time()
    while True:
        if read(cluster, attr, ep, key) == want: return round(time.time() - t0, 1)
        if time.time() - t0 > limit: return None
        time.sleep(0.3)
def device_types(ep):
    rc, out = ct_retry("descriptor", "read", "device-type-list", "1", str(ep))
    dts = [int(x) for x in re.findall(r"DeviceType: (\d+)", out)]
    if not dts:
        print("    (chip-tool descriptor %d: rc %d)\n    " % (ep, rc)
              + "\n    ".join([l[-160:] for l in out.splitlines() if l.strip()][-6:]), flush=True)
    return dts

# ------------------------------------------------------------------ CAN
def can(cmd, t=15):
    return subprocess.run(["ssh", CAN, cmd], capture_output=True, text=True, timeout=t).stdout

# ======================================================================
c0 = get("/api/config")
wait_up()
check(get("/api/version").get("variant") == "matter", "Matter-Firmware laeuft")

row = lambda **k: dict({"enable": True, "transport": "rtu", "unit_id": UNIT, "fc": "input", "type": "f32",
                        "word_swap": False, "scale": 1, "interval_ms": 1000, "host": "", "port": 502}, **k)
KINDS = [("temperature", 0, 0.1, 0x0302, "temperaturemeasurement", "measured-value", "MeasuredValue", 100),
         ("humidity",    70, 1,  0x0307, "relativehumiditymeasurement", "measured-value", "MeasuredValue", 100),
         ("pressure",    0,  4,  0x0305, "pressuremeasurement", "measured-value", "MeasuredValue", 1),
         ("flow",        70, 1,  0x0306, "flowmeasurement", "measured-value", "MeasuredValue", 10),
         ("power",       0,  10, 0x0510, "electricalpowermeasurement", "active-power", "ActivePower", 1e3),
         ("energy",      70, 100, 0x0510, "electricalenergymeasurement", "cumulative-energy-imported", "Energy", 1e6)]
# What the sensors report in -- the attributes' whole range (matter_mbm.cpp).
RANGES = {"temperature": ("-27315", "32767"), "humidity": ("0", "10000"),
          "pressure": ("-32767", "32767"), "flow": ("0", "65534")}
rows = [row(name="s%d" % i, reg=r, scale=sc, matter=k) for i, (k, r, sc, *_ ) in enumerate(KINDS)]
setup = {"modbus": {"enable": True, "rs485_role": "master", "rs485_tout_ms": 1000, "tcp_server": True, "tcp_uid": 247},
         "mbm": rows, "aux": [{"mode": "pwm"}, {"mode": "standard"}, {"mode": "standard"}],
         "di": [{"invert": False}] * 8, "dout": [{"invert": False}] * 8}
if CAN: setup["can"] = {"mode": 1, "bitrate": 500000, "base_id": 256, "tx_interval_ms": 0}
try:
    print("1. Konfiguration und Neustart")
    st, b = call("POST", "/api/config", setup)
    check(st == 200, "Konfiguration gespeichert: %d %s" % (st, b[:80]))
    reboot()
    try: call("POST", "/api/matter/decommission")     # a fabric left from an earlier run
    except Exception: pass
    time.sleep(3); wait_up(); time.sleep(10)

    print("4. Vor der Inbetriebnahme")
    check(not port_open(), "Modbus TCP antwortet nicht")
    if CAN:
        out = can("timeout 3 candump -L can0,100:7FF")
        check("#" not in out, "CAN sendet keinen Heartbeat")

    pairing = get("/api/matter/pairing")
    rc, out = ct("pairing", "code", "1", pairing["manual_code"], "--bypass-attestation-verifier", "true", t=300)
    check(rc == 0 and "commissioning completed with success" in out, "Inbetriebnahme (rc %d)" % rc)
    for _ in range(30):
        if get("/api/config")["modbus"]["running"]: break
        time.sleep(1)
    check(get("/api/config")["modbus"]["running"], "Modbus laeuft nach der Inbetriebnahme")
    if CAN:
        check(get("/api/config")["can"].get("running"), "CAN laeuft nach der Inbetriebnahme")

    print("1. Cluster-Erzeugung")
    rc, out = ct("descriptor", "read", "parts-list", "1", "0")
    parts = [int(x) for x in re.findall(r"\[\d+\]: (\d+)", out)]
    want = 16 + 1 + len(KINDS)
    check(parts == list(range(1, want + 1)), "Endpunkte 1..%d: %s" % (want, parts))
    check(device_types(1) == [0x010A] and device_types(9) == [0x0015], "DO1 Plug-in Unit, DI1 Contact Sensor")
    check(device_types(17) == [0x010B], "IO1 als PWM: Dimmable Plug-in Unit")
    time.sleep(3)
    for i, (k, _, _, dt, cl, attr, key, f) in enumerate(KINDS):
        ep = 18 + i
        st = {x["name"]: x for x in get("/api/modbus/status")["tcp_master"]}
        v = read(cl, attr, ep, key)
        want_v = st["s%d" % i].get("value")
        good = v not in (None, "null") and want_v is not None and abs(int(v) - want_v * f) <= max(abs(want_v * f) * 0.01, 1)
        dts = device_types(ep)
        check(dt in dts and good, "%s auf Endpunkt %d: %s (Modbus %s), Geraetetypen %s" % (k, ep, v, want_v, [hex(x) for x in dts]))
        if k in RANGES:
            lo = read(cl, "min-measured-value", ep, "MinMeasuredValue")
            hi = read(cl, "max-measured-value", ep, "MaxMeasuredValue")
            check((lo, hi) == RANGES[k], "%s: Messbereich %s..%s (erwartet %s..%s)" % ((k, lo, hi) + RANGES[k]))

    print("2. Rueckmeldung an Matter")
    for i in range(3):
        coil(0, True)
        t = until("onoff", "on-off", 1, "OnOff", "TRUE")
        check(t is not None, "Modbus-Coil 1 an -> Matter DO1 an (%s s)" % t)
        coil(0, False)
        t = until("onoff", "on-off", 1, "OnOff", "FALSE")
        check(t is not None, "Modbus-Coil 1 aus -> Matter DO1 aus (%s s)" % t)
    if CAN:
        can("cansend can0 102#0102")
        t = until("onoff", "on-off", 2, "OnOff", "TRUE"); check(t is not None, "CAN SET DO2 -> Matter DO2 an (%s s)" % t)
        can("cansend can0 102#0202")
        t = until("onoff", "on-off", 2, "OnOff", "FALSE"); check(t is not None, "CAN CLEAR DO2 -> Matter DO2 aus (%s s)" % t)
    hreg(2, 500); time.sleep(1.5)                    # HR 40003: IO1 duty 50.0 %
    lvl = read("levelcontrol", "current-level", 17, "CurrentLevel")
    on = read("onoff", "on-off", 17, "OnOff")
    check(on == "TRUE" and lvl == "127", "Modbus IO1-Duty 50 %% -> Matter an, Level %s (127)" % lvl)
    ct("onoff", "off", "1", "17"); ct("onoff", "on", "1", "17"); time.sleep(1)
    duty = struct.unpack(">H", mb(struct.pack(">BHH", 3, 2, 1))[2:4])[0]
    check(duty in (500, 502), "Matter aus/an danach: Duty %d (~500, der gemerkte Level folgt Modbus)" % duty)
    hreg(2, 0); time.sleep(1.5)
    check(read("onoff", "on-off", 17, "OnOff") == "FALSE", "Modbus IO1-Duty 0 -> Matter aus")

    print("3. Konfigurationsaenderung bei unveraenderten Pins")
    di1 = read("booleanstate", "state-value", 9, "StateValue")
    st, b = call("POST", "/api/config", {"di": [{"invert": True}] + [{"invert": False}] * 7})
    time.sleep(1.5)
    di1b = read("booleanstate", "state-value", 9, "StateValue")
    check(st == 200 and di1 in ("TRUE", "FALSE") and di1b == ("FALSE" if di1 == "TRUE" else "TRUE"),
          "DI1 invertiert: Matter %s -> %s ohne Pegelwechsel" % (di1, di1b))
    call("POST", "/api/config", {"di": [{"invert": False}] * 8}); time.sleep(1.5)
    check(read("booleanstate", "state-value", 9, "StateValue") == di1, "DI1 zurueck: Matter wieder %s" % di1)
    before = [read("onoff", "on-off", ep, "OnOff") for ep in range(1, 9)]
    st, b = call("POST", "/api/config", {"dout": [{"invert": True}] + [{"invert": False}] * 7})
    time.sleep(1.5)
    after = [read("onoff", "on-off", ep, "OnOff") for ep in range(1, 9)]
    check(st == 200 and before == after, "DO1 invertiert: kein Matter-Zustand bewegt (%s)" % after)
    call("POST", "/api/config", {"dout": [{"invert": False}] * 8})
    st, b = call("POST", "/api/config", {"di": [{"invert": False}] * 8})
    check(st == 200 and "reboot_required" not in b, "unveraendertes Speichern: kein Neustart verlangt (%s)" % b[:60])
finally:
    print("Aufraeumen")
    try: ct("pairing", "unpair", "1", t=60)
    except Exception: pass
    keep = {k: c0[k] for k in ("modbus", "mbm", "aux", "di", "dout", "can") if k in c0}
    keep["modbus"] = {k: v for k, v in keep["modbus"].items() if k != "running"}
    if "can" in keep: keep["can"] = {k: v for k, v in keep["can"].items() if k != "running"}
    call("POST", "/api/config", keep)
    reboot()
    shutil.rmtree(STORE, ignore_errors=True)
for cmd, rc, tail in RETRIES:
    print("  (wiederholt: chip-tool %s, rc %d)\n      %s" % (cmd, rc, "\n      ".join(tail)))
print("%d ok, %d FEHL, %d chip-tool-Wiederholung(en)" % (ok, bad, len(RETRIES)))
sys.exit(1 if bad else 0)
