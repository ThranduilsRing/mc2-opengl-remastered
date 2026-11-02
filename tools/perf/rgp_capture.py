"""Capture one Radeon GPU Profiler (.rgp) of the engine at a bookmark pose, no mouse or focus taken.

usage: py -3 tools/perf/rgp_capture.py --bookmarks mc2_10_camera --pose wolfman --mission mc2_10 --out <dir> [--exe EXE] [--width 1920 --height 1080]

How: RadeonDeveloperService (RDS, port 27300) + Radeon Developer Panel (minimized, no activation) apply the Profiling
feature to the AMD driver; the engine launches off-screen through the harness runner; after the pose is applied the
driver's capture hotkey (Ctrl+Alt+C, shortcut_sequence in the Panel preset) is injected with SendInput (keyboard only,
no mouse). The Panel's Profiling output_path is pointed at --out for the run and restored afterwards.
The Panel is a Qt GUI with no capture CLI, so this is the only non-manual route; if Panel launch or the hotkey fails the
script says so and exits 2 (manual route: Panel > Capture > Profiling > Capture profile while the pose is up).
Only processes this script started are killed.
"""
import argparse, ctypes, json, os, socket, subprocess, sys, threading, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "scripts")]
SUITE = Path("C:/Users/Joe/Downloads/RadeonDeveloperToolSuite-2026-02-02-1757")
PRESETS = Path(os.environ["APPDATA"]) / "RadeonDeveloperPanel" / "presets.json"
VK_CONTROL, VK_MENU, VK_C, KEYUP = 0x11, 0x12, 0x43, 0x2


def port_open(port=27300):
    with socket.socket() as s:
        s.settimeout(0.5)
        return s.connect_ex(("127.0.0.1", port)) == 0


def set_output(path):
    """Point the Panel's Profiling output_path at path; returns the original preset text for restore."""
    text = PRESETS.read_text(encoding="utf-8")
    doc = json.loads(text)
    mod = doc["current"]["modules"]["Profiling"]
    data = json.loads(mod["data"])
    data["output_path"] = str(path).replace("\\", "/").rstrip("/") + "/"
    mod["data"] = json.dumps(data, separators=(",", ":"))
    mod["enabled"] = True
    PRESETS.write_text(json.dumps(doc, indent=4), encoding="utf-8")
    return text


def send_hotkey():
    u = ctypes.windll.user32
    for vk, flags in ((VK_CONTROL, 0), (VK_MENU, 0), (VK_C, 0), (VK_C, KEYUP), (VK_MENU, KEYUP), (VK_CONTROL, KEYUP)):
        u.keybd_event(vk, 0, flags, 0)
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bookmarks", required=True, help="bookmark file stem under tests/visual/bookmarks")
    ap.add_argument("--pose", required=True)
    ap.add_argument("--mission", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--exe", type=Path, default=Path("E:/mc2-installs/lane8/mc2.exe"))
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--settle", type=float, default=8.0, help="seconds after the view ack before the hotkey")
    a = ap.parse_args()
    import run_battle_bench as battle
    from smoke_lib import host_lease, runner
    mark = next(m for m in json.loads((ROOT / "tests/visual/bookmarks" / f"{a.bookmarks}.json").read_text())["bookmarks"] if m["name"] == a.pose)
    a.out.mkdir(parents=True, exist_ok=True)
    before = {p.name for p in a.out.glob("*.rgp")}
    children, original = [], set_output(a.out)
    try:
        hidden = subprocess.STARTUPINFO(); hidden.dwFlags = 1; hidden.wShowWindow = 7   # SW_SHOWMINNOACTIVE
        if not port_open():
            children.append(subprocess.Popen([str(SUITE / "RadeonDeveloperServiceCLI.exe"), "--port", "27300"], cwd=SUITE,
                                             creationflags=0x08000000))
            time.sleep(2)
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
            time.sleep(a.settle); send_hotkey(); fired.append(time.time())
        threading.Thread(target=trigger, daemon=True).start()
        with host_lease.exclusive_window(f"rgp {a.mission} {a.pose}", timeout_s=1500):
            res = runner.run_one(runner.RunConfig(exe=[str(a.exe)], profile="stock", stem=a.mission, duration=int(a.settle) + 25,
                heartbeat_timeout_load_s=180, heartbeat_timeout_play_s=60, grace_s=150, env_extra=env, show_window=True))
        (a.out / "engine.log").write_text(res.stdout_text, encoding="utf-8")
        new = sorted(set(p.name for p in a.out.glob("*.rgp")) - before)
        print("hotkey sent:", bool(fired), "new rgp:", new)
        return 0 if new else 2
    finally:
        PRESETS.write_text(original, encoding="utf-8")
        for c in reversed(children):
            c.terminate()


if __name__ == "__main__":
    sys.exit(main())
