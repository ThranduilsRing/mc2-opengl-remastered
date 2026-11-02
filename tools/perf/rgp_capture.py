"""Capture one Radeon GPU Profiler (.rgp) of the engine at a bookmark pose, no mouse or focus taken.

usage: py -3 tools/perf/rgp_capture.py --bookmarks mc2_10_camera --pose wolfman --mission mc2_10 --out <dir> [--exe EXE] [--width 1920 --height 1080]

How: RadeonDeveloperService (RDS, port 27300) + Radeon Developer Panel (minimized, no activation) apply the Profiling
feature to the AMD driver with Auto capture mode = Frame; the engine launches off-screen through the harness runner and the
driver writes the .rgp itself at --frame. No hotkey and no input injection (a global hotkey would hit the owner's foreground
window). The Panel's Profiling output_path/frame index are set in its preset for the run and restored afterwards.
Uses the tray RadeonDeveloperService.exe, NOT RadeonDeveloperServiceCLI.exe: the CLI logs "Failed to query AMD driver service name"
on this host, so the driver never connects mc2.exe and no .rgp is written. rds.log/panel.log are copied to --out as evidence.
Exits 2 if no .rgp appears. Only processes this script started are killed.
"""
import argparse, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "scripts")]
SUITE = Path("C:/Users/Joe/Downloads/RadeonDeveloperToolSuite-2026-02-02-1757")
PRESETS = Path(os.environ["APPDATA"]) / "RadeonDeveloperPanel" / "presets.json"
PANEL_LOG = PRESETS.with_name("log.txt")


def port_open(port=27300):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def set_output(path, frame=None):
    """Point the Panel's Profiling output_path at path; returns the original preset text for restore."""
    text = PRESETS.read_text(encoding="utf-8")
    doc = json.loads(text)
    mod = doc["current"]["modules"]["Profiling"]
    data = json.loads(mod["data"])
    if frame:
        data["frame_capture_index"] = int(frame)   # Panel auto-capture mode Frame (opencl_auto_trigger=1): no hotkey
    data["output_path"] = str(path).replace("\\", "/").rstrip("/") + "/"
    mod["data"] = json.dumps(data, separators=(",", ":"))
    mod["enabled"] = True
    PRESETS.write_text(json.dumps(doc, indent=4), encoding="utf-8")
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bookmarks", required=True, help="bookmark file stem under tests/visual/bookmarks")
    ap.add_argument("--pose", required=True)
    ap.add_argument("--mission", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--exe", type=Path, default=Path("E:/mc2-installs/lane8/mc2.exe"))
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frame", type=int, default=1500, help="Panel auto-capture (Frame mode) at this frame index; no hotkey, no input injection")
    ap.add_argument("--settle", type=float, default=8.0, help="seconds after the view ack before the run ends")
    a = ap.parse_args()
    import run_battle_bench as battle
    from smoke_lib import host_lease, runner
    mark = next(m for m in json.loads((ROOT / "tests/visual/bookmarks" / f"{a.bookmarks}.json").read_text())["bookmarks"] if m["name"] == a.pose)
    a.out.mkdir(parents=True, exist_ok=True)
    before = {p.name for p in a.out.glob("*.rgp")}
    children, original = [], set_output(a.out, a.frame)
    try:
        hidden = subprocess.STARTUPINFO(); hidden.dwFlags = 1; hidden.wShowWindow = 7   # SW_SHOWMINNOACTIVE
        if not port_open():
            rds_log = open(a.out / "rds.log", "wb")
            # background-launch: allow RDS console service, never an engine.
            children.append(subprocess.Popen([str(SUITE / "RadeonDeveloperService.exe")], cwd=SUITE,
                                             stdout=rds_log, stderr=subprocess.STDOUT, creationflags=0x08000000))
            time.sleep(2)
        # background-launch: allow minimized no-activate profiler panel, never an engine.
        children.append(subprocess.Popen([str(SUITE / "RadeonDeveloperPanel.exe"), "--connect", "localhost"], cwd=SUITE, startupinfo=hidden))
        time.sleep(8)   # panel connects and pushes the Profiling settings
        ch = a.out / "commands.txt"
        ch.write_text(f"bench_output {a.out / 'frames.jsonl'}\n{battle.view_command(mark)}\n", encoding="utf-8")
        env = battle.environment(ch, a.width, a.height)
        env["MC2_VK_GPU_TIMESTAMPS"] = "0"
        fired = []
        def trigger():
            ack = ch.with_suffix(ch.suffix + ".ack")
            while not (ack.exists() and "bench_view" in ack.read_text(errors="replace")):
                time.sleep(0.2)
            time.sleep(a.settle)
            fired.append(time.time())
        threading.Thread(target=trigger, daemon=True).start()
        with host_lease.exclusive_window(f"rgp {a.mission} {a.pose}", timeout_s=1500):
            res = runner.run_one(runner.RunConfig(exe=[str(a.exe)], profile="stock", stem=a.mission, duration=int(a.settle) + 25,
                heartbeat_timeout_load_s=180, heartbeat_timeout_play_s=60, grace_s=150, env_extra=env, show_window=True))
        (a.out / "engine.log").write_text(res.stdout_text, encoding="utf-8")
        shutil.copy(PANEL_LOG, a.out / "panel.log")   # evidence: does the Panel list an mc2.exe client?
        new = sorted(set(p.name for p in a.out.glob("*.rgp")) - before)
        print("hotkey sent:", bool(fired), "new rgp:", new)
        return 0 if new else 2
    finally:
        PRESETS.write_text(original, encoding="utf-8")
        for c in reversed(children):
            c.terminate()


if __name__ == "__main__":
    sys.exit(main())
