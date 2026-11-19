#!/usr/bin/env python3
"""Player-path harness: launch the game the way a player does and drive the real front end with scripted input.

Every other smoke boots straight into `--mission` under MC2_SMOKE_MODE (movie decode skipped) or uses MC2_BOOT_TO_* shortcuts, so
none of them exercises "main menu -> New Campaign -> mission 1 -> first comm video" or "main menu -> Load Game -> resume". This
runner does, with NO OS input: the engine's scripted input player (MC2_INPUT_SCRIPT, GameOS/gameos/gos_input.cpp) feeds mouse
position/buttons and keys into the same state gos_GetMouseInfo / gos_GetKeyStatus report, so the real GUI hit-testing and button
handlers run and the window can stay unfocused, minimised or parked off-screen (nofocus.spawn).

Scenario files live in tests/player_path/<name>.pp, one line each:
  #! timeout 600            hard wall-clock limit for the whole scenario (seconds)
  #! expect <regex>         a log line that must appear (all must)
  #! forbid <regex>         a log line that must NOT appear
  #! differ A B FRAC        captures {out}/A.tga and {out}/B.tga must differ in at least FRAC of sampled bytes (proves the camera moved, e.g. the intro pan)
  await <regex> [seconds]   harness-side: wait until a log line matches (default 120 s), then continue
  setup save <src> <name>   harness-side: copy a fixture save into the profile savegame folder before launch
  <anything else>           forwarded to the engine script player (wait N | move X Y | click X Y [l|r] | key NAME | shot PATH | mark TEXT ...)
  {out} in a line is replaced by the evidence directory.
The last engine command should be `mark done`; the runner ends the game after seeing it. Result: PASS/FAIL per scenario, JSON in
<out>/<scenario>/result.json, exit code 0 only if every scenario passed.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_lib import fingerprint, nofocus  # noqa: E402
from smoke_lib import via_menu  # noqa: E402

SCEN_DIR = ROOT / "tests" / "player_path"
DEFAULT_PROOFS = Path("E:/mc2-archive-2026-10/proofs/player-path")
STRIP_ENV = via_menu.STRIP_ENV + ("MC2_BOOT_TO_BAY", "MC2_BOOT_VIDEO", "MC2_MENU_CANARY_SKIP_INTRO", "MC2_INPUT_SCRIPT")
DEFAULT_FORBID = [r"^CRASH:", r"MC2 Remastered Crash Report", r"requireOwner"]
REAL_SAVE_DIR = Path.home() / ".mechcommander2" / "savegame"   # the player's saves: a run must never write here


def real_saves_snapshot() -> dict:
    return {f.name: (f.stat().st_mtime_ns, f.stat().st_size) for f in REAL_SAVE_DIR.glob("*")} if REAL_SAVE_DIR.is_dir() else {}


def require_save_redirect(env: dict, out: Path) -> Path:
    """MC2_SAVE_DIR must point inside this run's evidence dir (never the player's savegame folder), or the run is refused."""
    d = env.get("MC2_SAVE_DIR", "")
    if not d:
        raise SystemExit("[PLAYER_PATH] REFUSED: MC2_SAVE_DIR is not set (the run would write the player's saves)")
    dp, op = Path(d).resolve(), out.resolve()
    if dp == REAL_SAVE_DIR.resolve() or op not in dp.parents:
        raise SystemExit(f"[PLAYER_PATH] REFUSED: MC2_SAVE_DIR={d} is not a per-run directory under {out}")
    return dp


def diff_fraction(a: Path, b: Path) -> float:
    """Fraction of sampled bytes that differ between two same-size TGA captures (header skipped); -1.0 when a file is missing or the sizes differ."""
    try:
        da, db = a.read_bytes()[18:], b.read_bytes()[18:]
    except OSError:
        return -1.0
    if len(da) != len(db) or not da:
        return -1.0
    step = 97
    n = len(da[::step])
    return sum(1 for x, y in zip(da[::step], db[::step]) if abs(x - y) > 8) / n


def parse_scenario(path: Path) -> dict:
    sc = {"name": path.stem, "timeout": 600.0, "expect": [], "forbid": list(DEFAULT_FORBID), "differ": [], "steps": [], "setup": []}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#!"):
            key, _, val = line[2:].strip().partition(" ")
            if key == "timeout":
                sc["timeout"] = float(val)
            elif key == "expect":
                sc["expect"].append(val.strip())
            elif key == "forbid":
                sc["forbid"].append(val.strip())
            elif key == "differ":
                a, b, frac = val.split()
                sc["differ"].append((a, b, float(frac)))
            continue
        if line.startswith("#"):
            continue
        if line.startswith("setup "):
            sc["setup"].append(line[6:].split())
        else:
            sc["steps"].append(line)
    return sc


def build_env(extra: dict, script: Path) -> dict:
    env = dict(os.environ)
    for k in STRIP_ENV:
        env.pop(k, None)
    env["MC2_LOG"] = "1"                      # stdout is live; otherwise it goes to NUL and no marker survives
    env["MC2_NO_CURSOR_GRAB"] = "1"
    tags = env.get("MC2_DIAG_TAGS") or "GPU_CULL,LIGHTBAKE_PROOF,ANIM_GATE,TerrainLOD_prod,CONFIG,BUILD,DEVICE,SHADER_COMPILE"
    if tags != "*" and "FIRST60" not in tags.split(","):
        tags += ",FIRST60"
    env["MC2_DIAG_TAGS"] = tags
    env["MC2_INPUT_SCRIPT"] = str(script)
    env.update(extra)
    return env


def fingerprint_problems(text: str, exe: Path) -> list:
    """Refuse a run whose exe does not carry the build fingerprint of the worktree HEAD (as smoke does): a seed exe proves nothing."""
    if os.environ.get("MC2_SMOKE_REQUIRE_FINGERPRINT", "1") == "0":
        return []
    try:
        lines, bad = fingerprint.check_fingerprint(fingerprint.parse_fingerprint(text), None, repo_root=ROOT,
                                                   reference=fingerprint.resolve_reference(ROOT), exe_path=exe)
    except Exception as exc:  # an unverifiable exe is a failed verification
        return [f"[DEPLOY_FINGERPRINT] check failed: {exc}"]
    return [l for l in lines if "MISMATCH" in l or "absent" in l or "unavailable" in l] or (["[DEPLOY_FINGERPRINT] not OK"] if bad else [])


def run_scenario(exe: Path, sc: dict, out: Path, extra_env: dict, show_window: bool = True) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    script = out / "input.script"
    script.write_text("# player-path input script (appended by run_player_path.py)\n", encoding="utf-8")
    if "MC2_SAVE_DIR" in extra_env:
        raise SystemExit("[PLAYER_PATH] REFUSED: --env MC2_SAVE_DIR is owned by the harness")
    SAVE_DIR = out / "savegame"
    SAVE_DIR.mkdir(parents=True, exist_ok=True)
    real_before = real_saves_snapshot()
    for s in sc["setup"]:
        if s[0] == "save" and len(s) == 3:
            src = ROOT / "tests" / "player_path" / "fixtures" / s[1]
            for f in src.parent.glob(src.name + ".*") if not src.exists() else [src]:
                shutil.copy2(f, SAVE_DIR / (s[2] + f.suffix if src.suffix == "" else s[2] + ".fit"))
    lease = via_menu.lease_check()
    env = build_env(extra_env, script)
    env["MC2_SAVE_DIR"] = str(SAVE_DIR)
    require_save_redirect(env, out)
    t0 = time.perf_counter()
    proc = nofocus.spawn(via_menu.game_argv(exe), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                         errors="replace", cwd=str(exe.resolve().parent), env=env, show_window=show_window)
    lines: list[tuple[float, str]] = []
    lock = threading.Lock()

    def pump():
        for raw in proc.stdout:
            with lock:
                lines.append((time.perf_counter() - t0, raw.rstrip("\r\n")))

    th = threading.Thread(target=pump, daemon=True)
    th.start()
    fail = ""
    cursor = 0
    done = False
    steps = [s.replace("{out}", str(out).replace("\\", "/")) for s in sc["steps"]]
    deadline = t0 + sc["timeout"]

    def find(rx, start, until):
        pat = re.compile(rx)
        while time.perf_counter() < until:
            with lock:
                snap = lines[start:]
            for i, (_, text) in enumerate(snap):
                if pat.search(text):
                    return start + i + 1
            if proc.poll() is not None:
                time.sleep(0.3)   # let the pump drain
                with lock:
                    snap = lines[start:]
                for i, (_, text) in enumerate(snap):
                    if pat.search(text):
                        return start + i + 1
                return -1
            time.sleep(0.2)
        return -2

    for step in steps:
        if proc.poll() is not None:
            fail = f"game exited early rc={proc.returncode} before step '{step}'"
            break
        if step.startswith("await "):
            rest = step[6:]
            m = re.match(r"(.*?)(?:\s+(\d+(?:\.\d+)?))?$", rest)
            rx, secs = m.group(1), float(m.group(2) or 120)
            r = find(rx, cursor, min(deadline, time.perf_counter() + secs))
            if r < 0:
                fail = (f"game exited (rc={proc.returncode}) while awaiting /{rx}/" if r == -1 else f"timeout awaiting /{rx}/ after {secs:.0f}s")
                break
            cursor = r
            continue
        with open(script, "a", encoding="utf-8", newline="\n") as f:
            f.write(step + "\n")
        if step.startswith("mark done"):
            r = find(r"cmd=mark done", cursor, min(deadline, time.perf_counter() + 600))
            if r < 0:
                fail = f"game exited (rc={proc.returncode}) before the script finished" if r == -1 else "timeout before the script finished"
            else:
                done = True
            break
    rc = proc.poll()
    if not fail and not done:
        fail = "script ended without `mark done`"
    time.sleep(1.0)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    th.join(timeout=5)
    with lock:
        all_lines = list(lines)
    (out / "game.log").write_text("\n".join(f"{t:9.3f} {x}" for t, x in all_lines), encoding="utf-8", errors="replace")
    text = "\n".join(x for _, x in all_lines)
    problems = [fail] if fail else []
    problems += fingerprint_problems(text, exe)
    if rc is not None and rc != 0 and not done:
        problems.append(f"exit code {rc} (0x{rc & 0xFFFFFFFF:08X})")
    for m in re.finditer(r"\[SAVELOAD\] op=\S+ file=(\S+)", text):
        fp = Path(m.group(1).replace("//", "/"))
        if SAVE_DIR.resolve() not in fp.resolve().parents:
            problems.append(f"save redirect not active: [SAVELOAD] wrote {fp} outside {SAVE_DIR}")
    if real_saves_snapshot() != real_before:
        problems.append(f"the player's savegame folder {REAL_SAVE_DIR} changed during the run (save redirect broken)")
    for rx in sc["expect"]:
        if not re.search(rx, text, re.M):
            problems.append(f"missing expected /{rx}/")
    for rx in sc["forbid"]:
        mm = re.search(rx, text, re.M)
        if mm:
            problems.append(f"forbidden /{rx}/ matched: {mm.group(0)[:100]}")
    for a, b, frac in sc["differ"]:
        got = diff_fraction(out / f"{a}.tga", out / f"{b}.tga")
        if got < frac:
            problems.append(f"captures {a} vs {b} differ {got:.3f} < {frac} (camera did not move / capture missing)")
    shots = sorted(str(p) for p in out.glob("*.tga")) + sorted(str(p) for p in out.glob("*.png"))
    res = {"scenario": sc["name"], "result": "FAIL" if problems else "PASS", "problems": problems, "wall_s": round(time.perf_counter() - t0, 1),
           "exit_code": rc, "lease": lease, "videos_opened": [m.group(1) for m in re.finditer(r"\[VIDEO\] init: opened (\S+)", text)],
           "captures": shots, "log": str(out / "game.log"), "env": {k: v for k, v in extra_env.items()}}
    (out / "result.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--install", required=True, help="install dir holding mc2.exe (a lane install; never the owner's)")
    ap.add_argument("--scenario", default="all", help="scenario name (tests/player_path/<name>.pp), comma list, or all")
    ap.add_argument("--out", default="", help="evidence root (default E:/mc2-archive-2026-10/proofs/player-path/<stamp>)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V", help="extra engine env (e.g. MC2_SIM_OVERLAP=0)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--scenario-dir", default="", help="directory of .pp scenarios (default tests/player_path; mod-only scripts live in tests/player_path_mods so the stock sweeps never run them)")
    a = ap.parse_args(argv)
    scen_dir = Path(a.scenario_dir) if a.scenario_dir else SCEN_DIR
    names = sorted(p.stem for p in scen_dir.glob("*.pp"))
    if a.list:
        print("\n".join(names))
        return 0
    want = names if a.scenario == "all" else [s.strip() for s in a.scenario.split(",") if s.strip()]
    bad = [n for n in want if n not in names]
    if bad:
        print(f"unknown scenario(s) {bad}; have {names}", file=sys.stderr)
        return 2
    inst = Path(a.install)
    if "owner-test" in str(inst).replace("\\", "/"):
        print("refusing to launch the owner's install", file=sys.stderr)
        return 2
    exe = inst / "mc2.exe"
    if not exe.exists():
        print(f"no mc2.exe in {inst}", file=sys.stderr)
        return 2
    extra = dict(e.split("=", 1) for e in a.env)
    root = Path(a.out) if a.out else DEFAULT_PROOFS / time.strftime("%Y%m%d-%H%M%S")
    results = []
    for n in want:
        r = run_scenario(exe, parse_scenario(scen_dir / f"{n}.pp"), root / n, extra)
        results.append(r)
        print(f"[PLAYER_PATH] {n}: {r['result']} wall={r['wall_s']}s videos={len(r['videos_opened'])} {'; '.join(r['problems'])}", flush=True)
    (root / "summary.json").write_text(json.dumps(results, indent=1), encoding="utf-8")
    ok = all(r["result"] == "PASS" for r in results)
    print(f"[PLAYER_PATH] {'PASS' if ok else 'FAIL'} evidence={root}", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
