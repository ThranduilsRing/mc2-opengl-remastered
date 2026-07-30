#!/usr/bin/env python3
"""GAMEPLAY-DRIVER-2 - drive a headless MC2 mission from a JSON scenario.

Boots mc2.exe headless, waits for the mission GUI to come live via the
MC2_UI_COMMAND_FILE ack channel (UI-COMMAND-CHANNEL-1), feeds the scenario's
command steps through that channel with ack-synchronisation, takes screenshots
at the marked capture points, ASSERTS ON ACTUAL WORLD STATE, kills ONLY its own
spawned PID, and writes a run report.

Why the assertions matter
-------------------------
The GAMEPLAY-DRIVER-1 prototype could only substring-match the ack line. That is
not a test: the 2026-07-23 night_light_test run reported ok=True while all five
units it spawned were inert (hp=0) and had ignored every move order, because
`spawn ... -> ok` and `order move ... -> ok` are both true statements about the
COMMAND while being silent about the WORLD. Every assert_* verb below reads the
world back through the channel's readback verbs (`list` / `unit` / `light_state`)
and fails the run when reality disagrees.

Scenario JSON schema
--------------------
{
  "mission": "mc2_10",              # base mission stem (--mission)
  "env": {"MC2_TEST_SPAWN": "1"},   # extra env vars (spawn/teleport need this)
  "camera_park": "x,y,rot,proj,alt@frame",   # optional MC2_CAMERA_PARK
  "boot_timeout": 300,              # seconds to wait for mission-live
  "duration": 600,                  # WALL-CLOCK seconds cap passed to --duration
                                    # (gos_smoke.cpp shouldQuit compares against
                                    #  SDL_GetPerformanceCounter, NOT sim time)
  "settle": 1.0,                    # default seconds to wait after each step
  "steps": [
    {"cmd": "select all", "expect": "ok"},   # assert the ack contains "-> ok"
    {"cmd": "order move 200 200 walk"},
    {"at_ack": "spawn clone 0 0", "cmd": "..."},  # wait for a prior ack first
    {"at_frame": 900, "cmd": "..."},         # EXACT: polls mission_status until
                                             # the engine frame counter >= 900
    {"wait": 2.0, "cmd": "status"},          # extra settle before sending
    {"snapshot": "before_move"},             # record every mover's pos/hp
    {"assert_moved": {"since": "before_move", "unit": 512, "min_dist": 50}},
    {"assert_hp":    {"unit": 512, "min": 0.5}},
    {"assert_pos":   {"unit": 512, "near": [200, 200], "within": 300}},
    {"assert_light": {"is_night": true}},
    {"assert_count": {"filter": "enemy", "min": 1}}
  ],
  "captures": [
    {"name": "after_move", "after_step": 2},  # screenshot after steps[2]
    {"name": "at_frame",   "frame": 900}      # screenshot at an EXACT frame
  ]
}

A step with no "cmd" (just "wait"/"at_frame"/"snapshot"/"assert_*") is a pure
sync or check point. Captures are emitted as `screenshot <name> backbuffer`
verbs and require the DEV-SHELL capture hook (the runner sets MC2_DEV_SHELL=1).

Frame handling: every ack line carries a `[f=<frame>]` prefix (GAMEPLAY-DRIVER-2),
so `at_frame` and capture-by-frame are EXACT - the runner polls `mission_status`
and reads the engine's own counter. The prototype had to approximate these with
wall-clock arithmetic because the ack carried no frame number.

Determinism caveat: MC2_SMOKE_FIXED_TIMESTEP pins scenarioTime AND (since
CAPTURE-DETERMINISM-1) frameLength, but pathing still depends on mover update
order, so unit positions are reproducible only within a tolerance. Assert with
`within` / `min_dist` bands, not exact coordinates.

Usage:
  py -3 tools/scenario_runner.py tools/scenarios/day_night_ab.json \\
      --install A:/Games/mc2-opengl/mc2-win64-v0.3 \\
      --out .claude/captures/gameplay-driver/day_night_ab
  # optional --exe <path> to run a freshly-built exe against the install's data

SAFETY: refuses to run if an mc2.exe is already running FROM the target install;
kills only the PID it spawned.
"""
import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

# "[f=1234] select all -> ok (5 selected)"
ACK_RE = re.compile(r"^\[f=(\d+)\]\s*(.*)$")
# "  [id=512 name=Zeus class=BattleMech team=0 cmdr=0 pos=(-4928.0,-5397.3,402.7) hp=1.000 ...]"
ID_RE = re.compile(r"\[id=(\d+)\s")
NAME_RE = re.compile(r"name=(.*?)\s+(?:class|pos)=")
POS_RE = re.compile(r"pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)")
HP_RE = re.compile(r"hp=([\d.]+)")
CLASS_RE = re.compile(r"class=(\S+)")
INVIEW_RE = re.compile(r"inView=(\d)")


def mc2_running_from(install_dir):
    """Paths of running mc2.exe processes launched FROM install_dir. Other lanes
    may legitimately be running; only a busy TARGET blocks us."""
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process mc2 -ErrorAction SilentlyContinue).Path"],
        capture_output=True, text=True).stdout
    norm = os.path.normcase(os.path.abspath(install_dir))
    return [p.strip() for p in out.splitlines()
            if p.strip() and os.path.normcase(
                os.path.abspath(os.path.dirname(p.strip()))) == norm]


class AckReader:
    """Incremental reader over <cmd_file>.ack. Splits each line into its
    [f=N] frame prefix and the payload."""

    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.last_frame = None
        # Unconsumed entries. wait_ack() returns the instant it sees its ack
        # line, but a readback verb's payload lines arrive in the SAME fprintf
        # batch, immediately AFTER that line. Without this queue those bytes are
        # already past `offset` and are lost forever -- which is exactly what
        # made every assert_* read an empty payload on the first run.
        self._queue = []

    def _read(self):
        try:
            with open(self.path, "r", errors="replace") as f:
                f.seek(self.offset)
                data = f.read()
                self.offset = f.tell()
        except OSError:
            return
        for ln in data.splitlines():
            ln = ln.rstrip("\n")
            m = ACK_RE.match(ln)
            if m:
                self.last_frame = int(m.group(1))
                self._queue.append((self.last_frame, m.group(2)))
            else:
                self._queue.append((None, ln))    # readback payload line

    def poll(self):
        """Drain everything currently buffered + anything new on disk."""
        self._read()
        out, self._queue = self._queue, []
        return out

    def pushback(self, entries):
        """Return unconsumed entries to the front of the queue."""
        self._queue = list(entries) + self._queue


def send(cmd_file, line):
    with open(cmd_file, "a") as f:
        f.write(line + "\n")


def wait_ack(ack, prefix, timeout, proc=None, collect=None):
    """Wait for an ack whose payload starts with prefix. Returns
    (frame, payload) or (None, None). Non-matching lines land in `collect`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        batch = ack.poll()
        for i, (frame, payload) in enumerate(batch):
            if frame is not None and payload.startswith(prefix):
                # Hand the REST of this batch back: a readback verb's payload
                # lines follow its ack line in the same write.
                ack.pushback(batch[i + 1:])
                return frame, payload
            if collect is not None and payload.strip():
                collect.append(payload)
        if proc is not None and proc.poll() is not None:
            raise RuntimeError("mc2.exe exited (code %s) while waiting for ack %r"
                               % (proc.returncode, prefix))
        time.sleep(0.15)
    return None, None


def query(cmd_file, ack, proc, verb, timeout=30):
    """Send a readback verb and return (ack_payload, [payload lines])."""
    collect = []
    send(cmd_file, verb)
    frame, payload = wait_ack(ack, verb, timeout, proc, collect)
    if payload is None:
        return None, []
    # The readback body is appended immediately after its ack line, but the
    # engine may still be mid-write; poll again until the payload stops growing.
    body = []
    stable = 0
    deadline = time.time() + 5.0
    while time.time() < deadline and stable < 2:
        time.sleep(0.35)
        new = [p for f, p in ack.poll() if f is None and p.strip()]
        if new:
            body.extend(new)
            stable = 0
        else:
            stable += 1
    return payload, collect + body


def parse_units(lines):
    """Parse `list`/`status` payload lines into {id: {...}}."""
    units = {}
    for ln in lines:
        m = ID_RE.search(ln)
        if not m:
            continue
        uid = int(m.group(1))
        rec = {"id": uid, "raw": ln.strip()}
        p = POS_RE.search(ln)
        if p:
            rec["pos"] = (float(p.group(1)), float(p.group(2)), float(p.group(3)))
        h = HP_RE.search(ln)
        if h:
            rec["hp"] = float(h.group(1))
        n = NAME_RE.search(ln)
        if n:
            rec["name"] = n.group(1).strip()
        c = CLASS_RE.search(ln)
        if c:
            rec["class"] = c.group(1)
        v = INVIEW_RE.search(ln)
        if v:
            rec["inView"] = v.group(1) == "1"
        units[uid] = rec
    return units


def current_frame(cmd_file, ack, proc):
    payload, body = query(cmd_file, ack, proc, "mission_status")
    for ln in body:
        m = re.search(r"frame=(\d+)", ln)
        if m:
            return int(m.group(1))
    return None


def wait_frame(cmd_file, ack, proc, target, timeout=180):
    """Poll mission_status until the ENGINE's own frame counter >= target."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        f = current_frame(cmd_file, ack, proc)
        if f is not None:
            last = f
            if f >= target:
                return f
        time.sleep(0.6)
    print("  WARN: frame %d not reached (last seen %s)" % (target, last))
    return last


def tga_to_png(tga_path, png_path):
    try:
        from PIL import Image
        Image.open(tga_path).convert("RGB").save(png_path)
        return True
    except ImportError:
        pass
    import zlib
    with open(tga_path, "rb") as f:
        hdr = f.read(18)
        idlen, _, imgtype = hdr[0], hdr[1], hdr[2]
        w, h = struct.unpack("<HH", hdr[12:16])
        bpp, desc = hdr[16], hdr[17]
        if imgtype != 2 or bpp not in (24, 32):
            print("  cannot convert %s (type=%d bpp=%d) without PIL"
                  % (tga_path, imgtype, bpp))
            return False
        f.read(idlen)
        n = bpp // 8
        raw = f.read(w * h * n)
    top_down = bool(desc & 0x20)
    rows = []
    for y in range(h):
        sy = y if top_down else (h - 1 - y)
        row = raw[sy * w * n:(sy + 1) * w * n]
        rgb = bytearray(w * 3)
        for x in range(w):
            b, g, r = row[x * n], row[x * n + 1], row[x * n + 2]
            rgb[x * 3:x * 3 + 3] = bytes((r, g, b))
        rows.append(b"\x00" + bytes(rgb))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
    with open(png_path, "wb") as f:
        f.write(png)
    return True


def tga_mean_luma(path):
    """Mean luminance of an uncompressed 24/32-bit TGA. Lets a scenario prove a
    lighting change actually reached the pixels."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    idlen, imgtype, bpp = d[0], d[2], d[16]
    w, h = struct.unpack("<HH", d[12:16])
    if imgtype != 2 or bpp not in (24, 32):
        return None
    off, px = 18 + idlen, bpp // 8
    tot = cnt = 0
    for i in range(0, w * h, 7):
        b = off + i * px
        tot += d[b] + d[b + 1] + d[b + 2]
        cnt += 1
    return tot / (cnt * 3.0) if cnt else None


def do_capture(cmd_file, ack, name, shot_dir, out_dir, proc, report):
    """Issue a screenshot verb, wait for the TGA, copy + convert to PNG."""
    tga = os.path.join(shot_dir, name + ".tga")
    try:
        os.remove(tga)
    except OSError:
        pass
    send(cmd_file, "screenshot %s backbuffer" % name)
    frame, payload = wait_ack(ack, "screenshot %s" % name, 30, proc)
    ok = bool(payload and "-> ok" in payload)
    entry = {"name": name, "frame": frame, "ack": payload, "ok": ok,
             "png": None, "mean_luma": None}
    if ok:
        deadline = time.time() + 20
        while time.time() < deadline and not os.path.isfile(tga):
            time.sleep(0.2)
        if os.path.isfile(tga):
            time.sleep(0.5)   # let the write finish
            shutil.copy2(tga, os.path.join(out_dir, name + ".tga"))
            out_png = os.path.join(out_dir, name + ".png")
            if tga_to_png(tga, out_png):
                entry["png"] = out_png
            entry["mean_luma"] = tga_mean_luma(tga)
        else:
            entry["ok"] = False
            entry["note"] = "TGA never appeared (needs MC2_DEV_SHELL=1)"
    report["captures"].append(entry)
    print("  capture %-22s frame=%s luma=%s %s"
          % (name, frame,
             ("%.2f" % entry["mean_luma"]) if entry["mean_luma"] else "n/a",
             "-> " + (entry["png"] or "FAILED")))
    return entry["ok"]


# --------------------------------------------------------------------------
# Assertions. Each returns (ok, detail).
# --------------------------------------------------------------------------
def _unit_snapshot(cmd_file, ack, proc):
    _, body = query(cmd_file, ack, proc, "list all")
    return parse_units(body)


def assert_moved(spec, snaps, cmd_file, ack, proc):
    since = spec.get("since")
    if since not in snaps:
        return False, "no snapshot named %r (add a {\"snapshot\": ...} step)" % since
    now = _unit_snapshot(cmd_file, ack, proc)
    uid = int(spec["unit"])
    min_d = float(spec.get("min_dist", 1.0))
    a, b = snaps[since].get(uid), now.get(uid)
    if not a or not b:
        return False, "unit %d missing (before=%s after=%s)" % (uid, bool(a), bool(b))
    if "pos" not in a or "pos" not in b:
        return False, "unit %d has no position in one of the samples" % uid
    d = math.dist(a["pos"][:2], b["pos"][:2])
    return (d >= min_d,
            "unit %d moved %.1f wu (need >= %.1f) %s -> %s"
            % (uid, d, min_d, tuple(round(v, 1) for v in a["pos"][:2]),
               tuple(round(v, 1) for v in b["pos"][:2])))


def assert_hp(spec, snaps, cmd_file, ack, proc):
    uid = int(spec["unit"])
    now = _unit_snapshot(cmd_file, ack, proc)
    u = now.get(uid)
    if not u or "hp" not in u:
        return False, "unit %d not found in `list all`" % uid
    lo, hi = float(spec.get("min", 0.0)), float(spec.get("max", 1.0))
    return lo <= u["hp"] <= hi, "unit %d hp=%.3f (want %.3f..%.3f)" % (uid, u["hp"], lo, hi)


def assert_pos(spec, snaps, cmd_file, ack, proc):
    uid = int(spec["unit"])
    now = _unit_snapshot(cmd_file, ack, proc)
    u = now.get(uid)
    if not u or "pos" not in u:
        return False, "unit %d not found in `list all`" % uid
    near = spec["near"]
    within = float(spec.get("within", 100.0))
    d = math.dist(u["pos"][:2], (float(near[0]), float(near[1])))
    return d <= within, ("unit %d at %s is %.1f wu from %s (want <= %.1f)"
                         % (uid, tuple(round(v, 1) for v in u["pos"][:2]), d,
                            tuple(near), within))


def assert_count(spec, snaps, cmd_file, ack, proc):
    filt = spec.get("filter", "all")
    _, body = query(cmd_file, ack, proc, "list %s" % filt)
    n = len(parse_units(body))
    lo = int(spec.get("min", 0))
    hi = int(spec.get("max", 10 ** 9))
    return lo <= n <= hi, "list %s -> %d unit(s) (want %d..%d)" % (filt, n, lo, hi)


def assert_light(spec, snaps, cmd_file, ack, proc):
    _, body = query(cmd_file, ack, proc, "light_state")
    txt = " ".join(body)
    got = {}
    for key, pat in (("is_night", r"isNight=(\d)"),
                     ("light_pitch", r"lightPitch=([-\d.]+)"),
                     ("night_factor", r"nightFactor=([\d.]+)")):
        m = re.search(pat, txt)
        if m:
            got[key] = float(m.group(1))
    if not got:
        return False, "could not parse light_state payload: %r" % txt[:160]
    problems = []
    if "is_night" in spec:
        want = 1.0 if spec["is_night"] else 0.0
        if got.get("is_night") != want:
            problems.append("isNight=%s want %s" % (got.get("is_night"), want))
    for key, lo_k, hi_k in (("light_pitch", "pitch_min", "pitch_max"),
                            ("night_factor", "night_factor_min", "night_factor_max")):
        if lo_k in spec and got.get(key, -1e9) < float(spec[lo_k]):
            problems.append("%s=%s < %s" % (key, got.get(key), spec[lo_k]))
        if hi_k in spec and got.get(key, 1e9) > float(spec[hi_k]):
            problems.append("%s=%s > %s" % (key, got.get(key), spec[hi_k]))
    detail = "light_state %s" % got
    if problems:
        # NOTE: nightFactor is NOT purely a function of the sun. Weather::update
        # (code/weather.cpp) raises it to rainLightLevel/4 while it is raining
        # and takes the MAX, so a rainy DAY mission reports a high nightFactor.
        # Assert on isNight / lightPitch for a sun check; use nightFactor only
        # when you know the weather.
        return False, detail + " FAILED: " + "; ".join(problems)
    return True, detail


ASSERTS = {
    "assert_moved": assert_moved,
    "assert_hp": assert_hp,
    "assert_pos": assert_pos,
    "assert_count": assert_count,
    "assert_light": assert_light,
}


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("scenario", help="path to the scenario .json")
    p.add_argument("--install", required=True, help="install dir (cwd + data)")
    p.add_argument("--exe", default=None,
                   help="override exe path (default <install>/mc2.exe); use to "
                        "run a freshly-built build64 exe against install data")
    p.add_argument("--out", required=True, help="output dir for captures + report")
    p.add_argument("--keep-running", action="store_true",
                   help="do not kill the engine at the end (manual inspection)")
    args = p.parse_args()

    scn = json.load(open(args.scenario))
    install = os.path.abspath(args.install)
    exe = os.path.abspath(args.exe) if args.exe else os.path.join(install, "mc2.exe")
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    busy = mc2_running_from(install)
    if busy:
        print("REFUSING: mc2.exe already running from the target install:")
        for b in busy:
            print("   " + b)
        return 2
    if not os.path.isfile(exe):
        print("no such exe: %s" % exe)
        return 2

    cmd_file = os.path.join(out_dir, "ui_commands.txt")
    ack_file = cmd_file + ".ack"
    for f in (cmd_file, ack_file):
        try:
            os.remove(f)
        except OSError:
            pass
    open(cmd_file, "w").close()
    shot_dir = os.path.join(install, "dev_shell_out")

    mission = scn.get("mission", "mc2_01")
    boot_timeout = float(scn.get("boot_timeout", 300))
    duration = int(scn.get("duration", 600))
    default_settle = float(scn.get("settle", 1.0))

    env = dict(os.environ)
    env.update({
        "MC2_SMOKE_MODE": "1",
        "MC2_SMOKE_FIXED_TIMESTEP": "1",
        "MC2_NO_LAUNCHER": "1",
        "MC2_DEV_SHELL": "1",
        "MC2_UI_COMMAND_FILE": cmd_file,
        "MC2_LOG": "1",   # without this the engine NUL-redirects stdout/stderr
        # MANDATORY for agent-driven runs: keep the window small and off the
        # user's screen. MC2_WINDOWED alone opens an options.cfg-sized window
        # covering the whole desktop, and SDL calls ShowWindow(SW_SHOW)
        # explicitly, so STARTUPINFO/wShowWindow does NOT suppress it.
        "MC2_WINDOWED": "1",
        "MC2_WIDTH": "640",
        "MC2_HEIGHT": "480",
        "MC2_NO_CURSOR_GRAB": "1",
    })
    for k, v in (scn.get("env") or {}).items():
        env[str(k)] = str(v)
    if scn.get("camera_park"):
        env["MC2_CAMERA_PARK"] = str(scn["camera_park"])

    log_path = os.path.join(out_dir, "mc2_run.log")
    log = open(log_path, "w")
    print("launching %s --profile stock --mission %s (cwd=%s)" % (exe, mission, install))
    proc = subprocess.Popen(
        [exe, "--profile", "stock", "--mission", mission, "--duration", str(duration)],
        cwd=install, env=env, stdout=log, stderr=subprocess.STDOUT)
    print("spawned pid=%d (only this PID will be killed)" % proc.pid)

    report = {"scenario": os.path.basename(args.scenario), "mission": mission,
              "exe": exe, "pid": proc.pid, "steps": [], "captures": [],
              "assertions": [], "ok": False}
    ack = AckReader(ack_file)
    rc = 0
    snaps = {}

    try:
        # --- wait for mission-live: the first ack IS the probe --------------
        print("waiting for mission-live (<= %ds) ..." % boot_timeout)
        t0 = time.time()
        live = False
        while time.time() - t0 < boot_timeout:
            if proc.poll() is not None:
                raise RuntimeError("mc2.exe exited (code %s) before mission-live"
                                   % proc.returncode)
            send(cmd_file, "mission_status")
            time.sleep(2.0)
            if any(f is not None for f, _ in ack.poll()):
                live = True
                break
        if not live:
            raise RuntimeError("mission never came live within %ds" % boot_timeout)
        print("mission live after %.0fs (engine frame %s)"
              % (time.time() - t0, ack.last_frame))
        report["boot_seconds"] = round(time.time() - t0, 1)

        by_step = {}
        for c in (scn.get("captures") or []):
            if "after_step" in c:
                by_step.setdefault(int(c["after_step"]), []).append(c)
        by_frame = [c for c in (scn.get("captures") or []) if "frame" in c]

        for idx, step in enumerate(scn.get("steps") or []):
            srec = {"index": idx, "step": step}

            if step.get("at_ack"):
                wait_ack(ack, step["at_ack"], float(step.get("timeout", 60)), proc)
            if step.get("at_frame") is not None:
                got = wait_frame(cmd_file, ack, proc, int(step["at_frame"]))
                srec["frame_reached"] = got
            if step.get("wait"):
                time.sleep(float(step["wait"]))

            if step.get("snapshot"):
                snaps[step["snapshot"]] = _unit_snapshot(cmd_file, ack, proc)
                srec["snapshot"] = {"name": step["snapshot"],
                                    "units": len(snaps[step["snapshot"]])}
                print("[%02d] snapshot %-16s %d unit(s)"
                      % (idx, step["snapshot"], len(snaps[step["snapshot"]])))

            fired = False
            for key, fn in ASSERTS.items():
                if key not in step:
                    continue
                fired = True
                ok, detail = fn(step[key], snaps, cmd_file, ack, proc)
                arec = {"step": idx, "kind": key, "ok": ok, "detail": detail}
                report["assertions"].append(arec)
                print("[%02d] %-14s %s  %s" % (idx, key, "PASS" if ok else "FAIL", detail))
                if not ok:
                    rc = 1

            if step.get("cmd"):
                cmd = step["cmd"]
                collect = []
                send(cmd_file, cmd)
                frame, payload = wait_ack(ack, cmd, float(step.get("timeout", 60)),
                                          proc, collect)
                time.sleep(float(step.get("settle", default_settle)))
                extra = [p for f, p in ack.poll() if f is None and p.strip()]
                srec.update({"cmd": cmd, "frame": frame, "ack": payload,
                             "extra": collect + extra})
                print("[%02d] %-34s %s" % (idx, cmd, payload))
                for ln in srec["extra"]:
                    print("       " + ln.strip())
                if payload is None:
                    srec["error"] = "no ack"
                    rc = 1
                elif step.get("expect") and step["expect"] not in payload:
                    srec["error"] = "expected %r in ack" % step["expect"]
                    print("       EXPECT FAILED: %s" % srec["error"])
                    rc = 1
            elif not fired and not step.get("snapshot"):
                print("[%02d] (sync only)" % idx)

            report["steps"].append(srec)

            for c in by_step.get(idx, []):
                if not do_capture(cmd_file, ack, c["name"], shot_dir, out_dir,
                                  proc, report):
                    rc = 1

        for c in by_frame:
            wait_frame(cmd_file, ack, proc, int(c["frame"]))
            if not do_capture(cmd_file, ack, c["name"], shot_dir, out_dir,
                              proc, report):
                rc = 1

        report["ok"] = (rc == 0)
    except Exception as exc:                      # noqa: BLE001
        report["error"] = "%s: %s" % (type(exc).__name__, exc)
        print("ERROR: %s" % report["error"])
        rc = 1
    finally:
        if not args.keep_running:
            try:
                proc.kill()
            except Exception:                     # noqa: BLE001
                pass
        log.close()
        try:
            shutil.copy2(ack_file, os.path.join(out_dir, "ui_commands.txt.ack"))
        except OSError:
            pass
        with open(os.path.join(out_dir, "report.json"), "w") as f:
            json.dump(report, f, indent=2)

    failed = [a for a in report["assertions"] if not a["ok"]]
    print("\n%s  steps=%d assertions=%d (%d failed) captures=%d  -> %s"
          % ("PASS" if report["ok"] else "FAIL", len(report["steps"]),
             len(report["assertions"]), len(failed), len(report["captures"]),
             os.path.join(out_dir, "report.json")))
    return rc


if __name__ == "__main__":
    sys.exit(main())
