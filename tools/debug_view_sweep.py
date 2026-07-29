#!/usr/bin/env python3
"""DEBUG-VIEW-SWEEP-1 — cycle the global debug render views and screenshot each.

Boots mc2.exe headless (MC2_SMOKE_MODE fixed-timestep), waits for mission-live
via the MC2_UI_COMMAND_FILE ack channel (UI-COMMAND-CHANNEL-1), then for each
of the six views issues `debugview N` + settle + `screenshot view_N backbuffer`,
restores view 0, kills ONLY its own spawned PID, and converts the TGAs to PNGs.

Views (gos_SetGlobalDebugView, see RendererFeatureRegistry MC2_DEBUG_VIEW):
  1 shadow-factor    raw shadow term grayscale (terrain); light-only (mechs)
  2 normalized-depth gl_FragCoord.z remapped over the 0.85..1.0 band, grayscale
  3 unlit-albedo     raw texture color, no lighting (all main lanes)
  4 overdraw-proxy   opaque lanes flat gray; VFX real additive accumulation
  5 world-normals    N*0.5+0.5 as RGB
  6 chunk-tint       hashed thin-record palette (terrain); materialIdx (props)

Usage:
  py -3 tools/debug_view_sweep.py --install A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0 \
      --mission mc2_01 --park "3000,-3000,45,35,1200@300" --out sweep_out

SAFETY: refuses to run if ANY mc2.exe is already running (the user may be
playing); kills only the PID it spawned.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))

from smoke_lib import nofocus  # noqa: E402


def mc2_running_from(install_dir):
    """Paths of running mc2.exe processes launched FROM install_dir (the user
    may legitimately be playing from a DIFFERENT install lane; only a busy
    target install blocks the sweep)."""
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-Process mc2 -ErrorAction SilentlyContinue).Path"],
        capture_output=True, text=True).stdout
    norm = os.path.normcase(os.path.abspath(install_dir))
    hits = []
    for line in out.splitlines():
        p = line.strip()
        if p and os.path.normcase(os.path.abspath(os.path.dirname(p))) == norm:
            hits.append(p)
    return hits


class AckReader:
    def __init__(self, path):
        self.path = path
        self.offset = 0

    def poll(self):
        """Return list of new ack lines."""
        try:
            with open(self.path, "r", errors="replace") as f:
                f.seek(self.offset)
                data = f.read()
                self.offset = f.tell()
        except OSError:
            return []
        return [ln.strip() for ln in data.splitlines() if ln.strip()]


def send(cmd_file, line):
    with open(cmd_file, "a") as f:
        f.write(line + "\n")


def wait_ack(ack, prefix, timeout, proc=None):
    """Wait for an ack line starting with prefix. Returns the line or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for ln in ack.poll():
            if ln.startswith(prefix):
                return ln
        if proc is not None and proc.poll() is not None:
            # Final drain: acks may have been flushed right before exit.
            for ln in ack.poll():
                if ln.startswith(prefix):
                    return ln
            raise RuntimeError("mc2.exe exited (code %s) while waiting for ack %r"
                               % (proc.returncode, prefix))
        time.sleep(0.25)
    return None


def tga_to_png(tga_path, png_path):
    """Convert. Prefer PIL; fall back to a minimal uncompressed-TGA reader."""
    try:
        from PIL import Image
        Image.open(tga_path).convert("RGB").save(png_path)
        return True
    except ImportError:
        pass
    # Minimal type-2 (uncompressed truecolor) TGA -> PNG via zlib
    import zlib
    with open(tga_path, "rb") as f:
        hdr = f.read(18)
        idlen, _, imgtype = hdr[0], hdr[1], hdr[2]
        w, h = struct.unpack("<HH", hdr[12:16])
        bpp, desc = hdr[16], hdr[17]
        if imgtype != 2 or bpp not in (24, 32):
            print("  cannot convert %s (type=%d bpp=%d) without PIL" % (tga_path, imgtype, bpp))
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


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--install", required=True, help="install dir containing mc2.exe")
    p.add_argument("--mission", default="mc2_01")
    p.add_argument("--park", default=None,
                   help='MC2_CAMERA_PARK value "x,y,rot,proj,alt[@frame]"')
    p.add_argument("--out", required=True, help="output dir for view_N.png")
    p.add_argument("--views", default="1,2,3,4,5,6")
    p.add_argument("--settle", type=float, default=0.6,
                   help="seconds to settle after each debugview switch (~10-20 frames)")
    p.add_argument("--boot-timeout", type=float, default=240.0)
    p.add_argument("--source", choices=["scene", "backbuffer"], default="scene",
                   help="(channel driver only) scene = offscreen scene FBO "
                        "(pre-HUD/post; most deterministic); backbuffer = final "
                        "frame incl. HUD/post")
    p.add_argument("--driver", choices=["frames", "channel"], default="frames",
                   help="frames (default): engine-side MC2_DEBUG_VIEW_SWEEP "
                        "frame-scheduled sequencer — captures land on identical "
                        "sim frames run-to-run (byte-comparable). channel: "
                        "wall-clock debugview/screenshot verbs over "
                        "MC2_UI_COMMAND_FILE (frame alignment NOT guaranteed).")
    p.add_argument("--schedule", default="600,30",
                   help="frames driver: '<startFrame>,<settleFrames>'")
    args = p.parse_args()

    install = os.path.abspath(args.install)
    exe = os.path.join(install, "mc2.exe")
    if not os.path.isfile(exe):
        print("no mc2.exe in %s" % install)
        return 2

    busy = mc2_running_from(install)
    if busy:
        print("ABORT: mc2.exe already running from the target install "
              "(user may be playing): %s. Not launching; nothing was touched."
              % ", ".join(busy))
        return 3

    os.makedirs(args.out, exist_ok=True)
    cmd_file = os.path.join(args.out, "ui_commands.txt")
    ack_file = cmd_file + ".ack"
    for f in (cmd_file, ack_file):
        try:
            os.remove(f)
        except OSError:
            pass
    open(cmd_file, "w").close()

    shot_dir = os.path.join(install, "dev_shell_out")
    views = [int(v) for v in args.views.split(",") if v.strip()]
    for v in views:
        try:
            os.remove(os.path.join(shot_dir, "view_%d.tga" % v))
        except OSError:
            pass

    env = dict(os.environ)
    env.update({
        "MC2_SMOKE_MODE": "1",
        "MC2_SMOKE_FIXED_TIMESTEP": "1",
        "MC2_NO_LAUNCHER": "1",
        "MC2_DEV_SHELL": "1",
        "MC2_UI_COMMAND_FILE": os.path.abspath(cmd_file),
        "MC2_LOG": "1",  # without this the engine NUL-redirects stdout/stderr
    })
    if args.park:
        env["MC2_CAMERA_PARK"] = args.park
    if args.driver == "frames":
        env["MC2_DEBUG_VIEW_SWEEP"] = args.schedule

    log_path = os.path.join(args.out, "mc2_sweep.log")
    log = open(log_path, "w")
    print("launching %s --profile stock --mission %s" % (exe, args.mission))
    # --duration is SIM seconds (fixed timestep runs sim fast headless); give
    # generous headroom — the sweep kills its own PID right after the last
    # capture, so wall runtime stays short regardless.
    proc = nofocus.spawn([exe, "--profile", "stock", "--mission", args.mission,
                          "--duration", "600"],
                         cwd=install, env=env, stdout=log, stderr=subprocess.STDOUT)
    print("spawned pid=%d (only this PID will be killed)" % proc.pid)

    rc = 0
    ack = AckReader(ack_file)
    try:
      if args.driver == "frames":
        # Engine-side frame-scheduled sequencer: just wait for the TGAs to land
        # (in order), then kill. Frame-keyed -> byte-comparable across runs.
        deadline = time.time() + args.boot_timeout
        for v in views:
            tga = os.path.join(shot_dir, "view_%d.tga" % v)
            while time.time() < deadline and not os.path.isfile(tga):
                if proc.poll() is not None:
                    raise RuntimeError("mc2.exe exited (code %s) before %s; see %s"
                                       % (proc.returncode, tga, log_path))
                time.sleep(0.25)
            if not os.path.isfile(tga):
                raise RuntimeError("timed out waiting for %s; see %s" % (tga, log_path))
            print("  captured view_%d" % v)
        time.sleep(0.5)  # let the last TGA write finish
      else:
        # Mission-live probe: the command channel is only polled by
        # MissionInterfaceManager::update, so the first ack == mission gui live.
        send(cmd_file, "debugview 0")
        if wait_ack(ack, "debugview 0 ->", args.boot_timeout, proc) is None:
            raise RuntimeError("mission never came live (no ack in %.0fs); see %s"
                               % (args.boot_timeout, log_path))
        print("mission live")
        # Keep the live run SHORT: if a camera park is requested, wait for its
        # log marker (park frame reached) instead of a blind sleep; else a brief
        # settle for the intro swoop.
        if args.park:
            deadline = time.time() + 60
            parked = False
            while time.time() < deadline and not parked:
                with open(log_path, "r", errors="replace") as lf:
                    parked = "[CAMERA_PARK v1]" in lf.read()
                if not parked:
                    time.sleep(0.5)
            print("camera park %s" % ("landed" if parked else "NOT seen (continuing)"))
            time.sleep(1.0)
        else:
            time.sleep(2.0)

        for v in views:
            send(cmd_file, "debugview %d" % v)
            ln = wait_ack(ack, "debugview %d ->" % v, 30, proc)
            print("  %s" % ln)
            if ln is None or "-> ok" not in ln:
                raise RuntimeError("debugview %d not acked ok" % v)
            time.sleep(args.settle)
            suffix = " backbuffer" if args.source == "backbuffer" else ""
            send(cmd_file, "screenshot view_%d%s" % (v, suffix))
            ln = wait_ack(ack, "screenshot view_%d " % v, 30, proc)
            print("  %s" % ln)
            if ln is None or "-> ok" not in ln:
                raise RuntimeError("screenshot view_%d not acked ok" % v)
            tga = os.path.join(shot_dir, "view_%d.tga" % v)
            deadline = time.time() + 20
            while time.time() < deadline and not os.path.isfile(tga):
                time.sleep(0.25)
            if not os.path.isfile(tga):
                raise RuntimeError("screenshot file never appeared: %s" % tga)
            time.sleep(0.5)  # let the TGA write finish

        send(cmd_file, "debugview 0")
        wait_ack(ack, "debugview 0 ->", 15, proc)
    except Exception as e:
        print("ERROR: %s" % e)
        rc = 1
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=15)
        log.close()

    if rc == 0:
        for v in views:
            tga = os.path.join(shot_dir, "view_%d.tga" % v)
            png = os.path.join(args.out, "view_%d.png" % v)
            shutil.copy2(tga, os.path.join(args.out, "view_%d.tga" % v))
            if tga_to_png(tga, png):
                print("wrote %s" % png)
    return rc


if __name__ == "__main__":
    sys.exit(main())
