#!/usr/bin/env python3
"""saveload_roundtrip - does a quicksave followed by a quickload put the simulation back where it was?

WHAT: runs one mission under the determinism gates, quicksaves at fixed tick
SAVE_AT and quickloads at LOAD_AT (the MC2_BOOT_QUICKSAVE_AT / MC2_BOOT_QUICKLOAD_AT fixtures set the same flags as the pause menu buttons),
and compares the `[SIMID v1]` digests the simulation prints AFTER the load with the ones it printed after the save, frame by frame. The load puts
the scenario clock back, so the first post-load frame is matched to the pre-load frame with the same time. A save/load that restores everything
the simulation reads gives identical digests for as long as the run lasts (the mission itself is run-to-run deterministic, tools/sim_determinism.py).
WHY: nothing else exercises Mission::save/load; the enum and ownership steps touch the serialized state. Owner ruling: within a release line a save
must load, and the round trip is the proof.
With MC2_SIM_IDENTITY_TRACE_DETAIL=1 the first divergent frame is explained mover by mover (gesture, path step, position, target).

  py -3 tools/saveload_roundtrip.py run --mission mc2_03 --headless --save-at 300 --load-at 420 --frames 60
  py -3 tools/saveload_roundtrip.py resave [--mission mc2_01] [--save-at 1500] [--load-at 1700]   # save, quickload, save again: byte identity
  py -3 tools/saveload_roundtrip.py --baseline [--mission mc2_01]   # record the digest of the round-trip save (tests/saveload/baselines/)
  py -3 tools/saveload_roundtrip.py --check [--mission mc2_01]      # run again and compare with the recorded digest
  py -3 tools/saveload_roundtrip.py --baseline --mission mc2_01 --mask tools/save_noise_mask.json --first-save-only   # the masked digest of the first save; the lossy re-save is no gate
  py -3 tools/saveload_roundtrip.py analyze path/to/mission.log [--frames 60]
  py -3 tools/saveload_roundtrip.py selftest
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402
import save_inspect as si  # noqa: E402
import savegame_backup  # noqa: E402
import sim_determinism as sd  # noqa: E402

SAVE_AT, LOAD_AT = "1000", "1400"
ENV = dict(sd.GATES, MC2_SMOKE_FREEZE_ANIM="1", MC2_FIXTURE_ORDER_LIVENESS="1", MC2_SIM_IDENTITY_TRACE_DETAIL="1",
           MC2_BOOT_QUICKSAVE_AT=SAVE_AT, MC2_BOOT_QUICKLOAD_AT=LOAD_AT)
_LINE = re.compile(r"\[SIMID v1\] simframe=(\d+) t=([\d.]+) ([^\r\n\[]*)")
_MOVER = re.compile(r"\[SIMID v1 mover\] simframe=(\d+) id=(\d+) ([^\r\n\[]*)")


def parse(text_lines):
    """-> (frames [(simframe, t, digest text)], movers {index into frames: {id: record}}); simframe repeats after a load, so it cannot key the movers"""
    frames, movers = [], {}
    for ln in text_lines:
        for m in _LINE.finditer(ln):
            frames.append((int(m.group(1)), float(m.group(2)), sd.simulation_fields(m.group(3).strip())))
        for m in _MOVER.finditer(ln):
            movers.setdefault(len(frames) - 1, {})[int(m.group(2))] = m.group(3).strip()
    return frames, movers


def analyze(text_lines, frames_needed: int = 300):
    """-> (ok, [report lines])"""
    if frames_needed < 1:
        return False, ["continuation window must contain at least one frame"]
    frames, movers = parse(text_lines)
    jumps = [i for i in range(1, len(frames)) if frames[i][1] < frames[i - 1][1] - 0.01]
    if not jumps:
        return False, ["no load found: the scenario clock never went back (SAVE_AT/LOAD_AT are fixed ticks; an intro camera can defer both fixtures)"]
    j = jumps[0]
    t_load = frames[j][1]
    pre = [i for i in range(j) if abs(frames[i][1] - t_load) < 0.02]
    if not pre:
        return False, [f"no pre-load frame at t={t_load:.3f} to match the first post-load frame"]
    i0 = pre[0]
    after = len(frames) - j
    report = [f"load at sim frame {frames[j][0]} (t={t_load:.3f}) matched to pre-load frame {frames[i0][0]}; {after} frames after the load"]
    first = None
    for k in range(min(after, j - i0, frames_needed)):
        pre_frame, post_frame = frames[i0 + k], frames[j + k]
        if (abs(pre_frame[1] - post_frame[1]) > 0.002 or
                (k and (pre_frame[0] != frames[i0 + k - 1][0] + 1 or
                        post_frame[0] != frames[j + k - 1][0] + 1 or
                        abs(pre_frame[1] - frames[i0 + k - 1][1] - 1 / 30) > 0.002 or
                        abs(post_frame[1] - frames[j + k - 1][1] - 1 / 30) > 0.002))):
            return False, [*report, f"non-contiguous or shifted fixed tick at +{k}: pre={pre_frame[:2]} post={post_frame[:2]}"]
        if frames[i0 + k][2] != frames[j + k][2]:
            first = k
            break
    horizon = min(after, j - i0, frames_needed)
    if first is None:
        ok = horizon >= frames_needed
        report.append(f"digests identical over {horizon} frames" + ("" if ok else f" (need {frames_needed})"))
        return ok, report
    report.append(f"DIGESTS DIVERGE {first} frame(s) after the load (identical before that)")
    report.append("  pre-load : " + frames[i0 + first][2])
    report.append("  post-load: " + frames[j + first][2])
    a, b = movers.get(i0 + first, {}), movers.get(j + first, {})
    diff = [i for i in a if a[i] != b.get(i)]
    if diff:
        report.append(f"{len(diff)} of {len(a)} movers differ at that frame; first three:")
        for i in diff[:3]:
            report.append(f"  id {i}\n    saved : {a[i]}\n    loaded: {b.get(i)}")
    elif not a or not b:
        report.append("(no per-mover detail in the log: rerun with MC2_SIM_IDENTITY_TRACE_DETAIL=1)")
    else:
        report.append("per-mover records match; compare the global SIMID fields above")
    # How far the state stays close: the share of post-load frames whose digest still equals the saved run's.
    same = sum(1 for k in range(horizon) if frames[i0 + k][2] == frames[j + k][2])
    report.append(f"{same} of {horizon} compared frames have an identical digest")
    return first >= frames_needed, report


def selftest() -> int:
    def mk(n, t0=0.0, tag="a"):
        return [f"[SIMID v1] simframe={i} t={t0 + i / 30:.3f} n=2 graded={tag}{i} visual=v strict=s" for i in range(n)]

    fails = []
    nl = chr(10)
    noise = "[runner] [ORDER-LIVENESS] HARD FAIL: 1 order-liveness check(s) failed" + nl + "| mc2_16  | PASS   |"
    if (runner_failed(1, noise) or runner_failed(0, "x") or not runner_failed(1, noise + nl + "[runner] [DEPLOY_FINGERPRINT] HARD FAIL: x")
            or not runner_failed(1, "crash") or not runner_failed(1, "| mc2_16  | FAIL   |" + nl + noise)):
        fails.append("only the order-liveness hard fail may be ignored")
    pre = mk(100)                           # frames 0..99, t up to 3.3
    good = pre + mk(60, t0=60 / 30.0)       # load back to frame 60's time; same digests names -> they differ by index, so build exact copy
    good = pre + [f"[SIMID v1] simframe={100 + i} t={(60 + i) / 30:.3f} n=2 graded=a{60 + i} visual=v strict=s" for i in range(40)]
    ok, rep = analyze(good, frames_needed=30)
    if not ok:
        fails.append("an exact restore must pass: " + "; ".join(rep))
    bad = pre + [f"[SIMID v1] simframe={100 + i} t={(60 + i) / 30:.3f} n=2 graded=a{60 + i if i < 10 else 'X'} visual=v strict=s" for i in range(40)]
    ok, rep = analyze(bad, frames_needed=30)
    if ok or "10 frame(s)" not in " ".join(rep):
        fails.append("a restore that drifts after 10 frames must fail and say so: " + "; ".join(rep))
    ok, rep = analyze(pre, frames_needed=30)
    if ok or "no load found" not in rep[0]:
        fails.append("a log without a load must fail")
    ok, rep = analyze(good[:110], frames_needed=30)
    if ok:
        fails.append("too few post-load frames must fail")
    if analyze(good, frames_needed=0)[0]:
        fails.append("an empty window must not pass")
    if not analyze(bad, frames_needed=10)[0] or analyze(bad, frames_needed=11)[0]:
        fails.append("the bounded window must include exactly the requested frames")
    gapped = [line for index, line in enumerate(good) if index not in (70, 110)]
    if analyze(gapped, frames_needed=30)[0]:
        fails.append("matching dropped ticks in both halves cannot prove a contiguous window")
    with tempfile.TemporaryDirectory() as td:
        def mkfile(name, pk):
            p = os.path.join(td, name)
            si.scs.write(p, pk, False)
            return p

        base_pk = [(0, b"00.06.01.0614\0"), (4, b"mission" * 40), (4, b"M" * 300), (7, b""), (4, b"W" * 128)]
        bad_pk = [(t, (r[:77] + b"\x55" + r[78:]) if t == 4 and len(r) == 300 else r) for t, r in base_pk]
        f1, f2, f3 = mkfile("a.ims", base_pk), mkfile("b.ims", base_pk), mkfile("c.ims", bad_pk)
        ok, rep = resave_verdict(f1, f2)
        if not ok:
            fails.append("two identical saves must pass: " + "; ".join(rep))
        ok, rep = resave_verdict(f1, f3)
        if ok or "packet 2" not in " ".join(rep) or "offset 77" not in " ".join(rep):
            fails.append("a save that differs at packet 2 offset 77 must fail and say where: " + "; ".join(rep))
        if resave_verdict(f1, None)[0] or resave_verdict(None, f2)[0]:
            fails.append("a missing save or re-save must fail")
        empty = mkfile("empty.ims", [])
        if resave_verdict(f1, empty)[0] or resave_verdict(empty, f1)[0]:
            fails.append("an empty save must fail, not compare nothing")
        if resave_verdict(f1, f2, rc=1)[0] or resave_verdict(f1, None, rc=1)[0]:
            fails.append("a crashed child must fail even when both files exist")
        fp = {"sha": "abc", "dirty": 0}
        rec = baseline_record("m", 10, 20, f1, f2, fp)
        ok, rep = check_baseline_record(rec, baseline_record("m", 10, 20, f2, f1, fp))
        if not ok:
            fails.append("a baseline must accept an identical save: " + "; ".join(rep))
        ok, rep = check_baseline_record(rec, baseline_record("m", 10, 20, f3, f1, fp))
        if ok or "differs" not in " ".join(rep):
            fails.append("a baseline must refuse a changed save and name the run: " + "; ".join(rep))
        ok, rep = check_baseline_record(rec, baseline_record("m", 10, 20, f1, f3, fp))
        if ok or "re-save" not in " ".join(rep):
            fails.append("a baseline that was re-save identical must refuse a run that is not")
        other = dict(rec, data_fingerprint={"digest": "d2", "problems": []})
        ok, rep = check_baseline_record(dict(rec, data_fingerprint={"digest": "d1"}), other)
        if ok or "data fingerprint" not in " ".join(rep):
            fails.append("a baseline must refuse a run on other install data before comparing the save")
        for broken in ({}, {"overall": "x"}, {"runs": []}, {"runs": "no", "overall": "x"}, []):
            ok, rep = check_baseline_record(broken, rec)
            if ok or "malformed baseline" not in " ".join(rep):
                fails.append("a malformed baseline must be refused with one line, not raise: " + repr(broken))
        if json.loads(json.dumps(rec))["runs"] != rec["runs"]:
            fails.append("the baseline record must survive a JSON round trip")
        mask_path = os.path.join(td, "mask.json")
        with open(mask_path, "w", encoding="utf-8") as fh:
            json.dump({"classes": {"MechData": {"run": [[77, 77]], "build": [[80, 80]]}}}, fh)
        noise_pk = [(4, bytes(3992))]
        nf = mkfile("noise.ims", noise_pk)
        inside_pk = [(4, bytes(77) + b"\x55" + bytes(3992 - 78))]
        ni = mkfile("inside.ims", inside_pk)
        inside = baseline_record("m", 10, 20, ni, nf, fp, mask_path)
        masked_base = baseline_record("m", 10, 20, nf, nf, fp, mask_path)
        if not check_baseline_record(json.loads(json.dumps(masked_base)), inside)[0] or not inside["resave_matches_policy"] or inside["resave_identical"]:
            fails.append("baseline/check and immediate resave must accept an inside-mask mutation")
        outside_pk = [(4, bytes(76) + b"\x55" + bytes(3992 - 77))]
        f4 = mkfile("outside.ims", outside_pk)
        outside = baseline_record("m", 10, 20, f4, nf, fp, mask_path)
        if check_baseline_record(masked_base, outside)[0] or outside["resave_matches_policy"]:
            fails.append("baseline/check and immediate resave must reject an outside-mask mutation")
        build_pk = [(4, bytes(80) + b"\x55" + bytes(3992 - 81))]
        f5 = mkfile("build.ims", build_pk)
        if not check_baseline_record(masked_base, baseline_record("m", 10, 20, f5, nf, fp, mask_path))[0]:
            fails.append("the declared cross-build mask applies to canonical baseline hashes")
        if check_baseline_record(rec, inside)[0]:
            fails.append("historical unmasked hashes must never silently become masked baselines")
        if fixture_verdict(f1, None, mask_path)[0]:
            fails.append("a mask cannot excuse a missing resave")
        if fixture_verdict(f1, f2, mask_path, rc=1)[0]:
            fails.append("a mask cannot excuse a crashed child")
        if smoke_target("lane3") != ["--deploy", "lane3"] or smoke_target(sd.resolve_install("lane3")) != ["--deploy", "lane3"]:
            fails.append("lane3 names and paths must route to lane3")
        import contextlib
        import io
        from unittest.mock import patch
        baseline_json = os.path.join(td, "baseline.json")
        with open(baseline_json, "w", encoding="utf-8") as fh:
            json.dump(masked_base, fh)
        with patch(__name__ + ".baseline_path", return_value=baseline_json), contextlib.redirect_stdout(io.StringIO()):
            for save, expected in ((ni, 0), (f4, 1)):
                with patch(__name__ + ".run_fixture", return_value=(td, save, nf, fp, 0)) as fixture:
                    verdict = main(["--check", "--mission", "m", "--save-at", "10", "--load-at", "20", "--install", "lane3",
                                    "--save-dir", td, "--mask", mask_path])
                    if verdict != expected or fixture.call_args.args[-4:] != (sd.resolve_install("lane3"), td, None, None):
                        fails.append("--check must apply --mask and route --install/--save-dir end to end")
            with patch(__name__ + ".run_fixture", return_value=(td, ni, nf, fp, 0)) as fixture:
                if main(["--check", "--mission", "m", "--mask", mask_path]) != 0 or fixture.call_args.args[1:3] != (10, 20):
                    fails.append("--check without frame flags must run at the baseline's frames")
            with patch(__name__ + ".run_fixture", return_value=(td, ni, nf, fp, 0)) as fixture:
                main(["--check", "--mission", "m", "--mask", mask_path, "--sim-speed", "4"])
                if fixture.call_args.kwargs.get("sim_speed") != 4 or speed_args(4) != ["--sim-speed", "4"] or speed_args(1):
                    fails.append("--sim-speed must reach run_smoke, and 1x must add no argument")
            with patch(__name__ + ".run_fixture", return_value=(td, ni, nf, fp, 0)) as fixture:
                if main(["--check", "--mission", "m", "--save-at", "11", "--mask", mask_path]) != 1 or fixture.called:
                    fails.append("--check with frames that contradict the baseline must refuse before launching")
            with patch(__name__ + ".run_fixture", return_value=(td, ni, nf, fp, 1)):
                if main(["--check", "--mission", "m", "--mask", mask_path, "--first-save-only"]) != 1:
                    fails.append("--first-save-only must not excuse a crashed child")
    for f in fails:
        print("saveload_roundtrip selftest FAIL: " + f)
    print("saveload_roundtrip selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def smoke_target(install: str, ref_exe: str | None = None, ref_commit: str | None = None) -> list[str]:
    return sd.smoke_target(install, ref_exe, ref_commit)


def speed_args(sim_speed: int) -> list[str]:
    """run_smoke.py arguments for N sim steps per drawn frame; the save fixtures count the same frame numbers at any N."""
    return ["--sim-speed", str(sim_speed)] if sim_speed > 1 else []


def add_speed_arg(p) -> None:
    p.add_argument("--sim-speed", type=int, choices=(1, 2, 3, 4), default=None, metavar="N",
                   help="N fixed sim steps per drawn frame (run_smoke --sim-speed); same saves, about 1/N of the host time past loading. Unset: check uses the speed the baseline was recorded at (the intro hold is render-frame paced, so ActualTime in the camera/world text packet depends on it), else 1")


def runner_failed(returncode: int, output: str) -> bool:
    """Did the smoke runner fail for a reason that matters here? The order-liveness fixture only issues moves (it keeps units walking at the
    save point); its summary needs an autowin window this tool does not want, so the runner's ORDER-LIVENESS hard fail (rc 1) is expected."""
    if not returncode:
        return False
    other = [ln for ln in output.splitlines() if ("HARD FAIL" in ln and "ORDER-LIVENESS" not in ln) or re.match(r"[|]\s*\S+\s*[|]\s*(FAIL|CRASH|TIMEOUT)", ln)
             or ln.startswith("### ")]
    return bool(other) or "ORDER-LIVENESS" not in output


def run(mission: str, frames: int, deploy: bool, out: str | None, install: str = sd.INSTALL_DIR,
        save_dir: str | None = None, ref_exe: str | None = None, ref_commit: str | None = None, sim_speed: int = 1,
        save_at: int = int(SAVE_AT), load_at: int = int(LOAD_AT), duration: int = 28, headless: bool = False):
    if frames < 1 or frames > 60:
        return False, ["use a bounded continuation window of 1..60 fixed ticks (up to 2 seconds)"]
    if save_at < 1 or load_at <= save_at:
        return False, ["continuation requires 0 < save-at < load-at"]
    dest, first, second, _, rc = run_fixture(mission, save_at, load_at, deploy, out, duration,
        install=install, save_dir=save_dir, ref_exe=ref_exe, ref_commit=ref_commit,
        sim_speed=sim_speed, headless=headless, continuation=True)
    log = os.path.join(dest, "run", mission + ".log")
    with open(log, encoding="utf-8", errors="replace") as fh:
        ok, report = analyze(fh.read().splitlines(), frames)
    # A clock rewind alone cannot excuse a failed smoke gate; retain its evidence even on failure.
    missing = save_evidence(first, "first save") or save_evidence(second, "re-save")
    if rc or missing:
        with open(os.path.join(dest, "runner.txt"), encoding="utf-8", errors="replace") as fh:
            runner_lines = fh.read().splitlines()
        return False, [f"the smoke run failed (rc={rc}): {missing or 'see run log'}", *runner_lines[-8:], *report]
    return ok, report


#: Where recorded round-trip digests live, one JSON file per mission.
BASELINE_DIR = os.path.join(ROOT, "tests", "saveload", "baselines")
_FINGERPRINT = re.compile(r"\[BUILD_FINGERPRINT v1\] sha=(\w+) dirty=([01])")
DEFAULT_SAVE_AT, DEFAULT_LOAD_AT = 1000, 1100


def resave_env(save_at: int, load_at: int) -> dict:
    """Environment of a resave run: the determinism gates, frozen animation, and the quicksave and quickload fixtures. With both fixtures armed the
    game saves a second time right after the load (testgame_resave.ims)."""
    return dict(sd.GATES, MC2_SMOKE_FREEZE_ANIM="1", MC2_BOOT_QUICKSAVE_AT=str(save_at), MC2_BOOT_QUICKLOAD_AT=str(load_at))


def run_fixture(mission: str, save_at: int, load_at: int, deploy: bool, out: str | None, duration: int = 28, plant: str | None = None,
                install: str = sd.INSTALL_DIR, save_dir: str | None = None, ref_exe: str | None = None, ref_commit: str | None = None,
                sim_speed: int = 1, headless: bool = False, continuation: bool = False):
    """Run `mission` with the save, load, re-save fixtures. -> (out dir, first save path, second save path or None, fingerprint dict or None).
    With `plant` (a save written by another exe, e.g. by tools/campaign_save_roundtrip.py) that file is put where the quickload reads it;
    `save_at` must be 0 (a quicksave would overwrite it), so the quickload loads it and it comes back as the first save, but the engine writes
    no re-save after a planted load.
    The owner's quicksave is backed up first and put back afterwards."""
    if deploy:
        sd.build_and_deploy(install)
    dest = out or tempfile.mkdtemp(prefix="resave_")
    os.makedirs(dest, exist_ok=True)
    sdir = save_dir or savegame_backup.default_dir()
    second_src = os.path.join(sdir, "testgame_resave.ims")
    env = dict(os.environ, **resave_env(save_at, load_at))
    # A movie defers BOTH fixtures, preserving their gap. mc2_03 saves about
    # 449 ticks late: load_at + 120 used to terminate before its quickload.
    # Keep fixed headroom; duration is a wall timeout, not a simulation budget.
    # Slow content reloads may need more wall time without adding observed ticks.
    headless_ticks = load_at + 600
    if continuation:
        env["MC2_SIM_IDENTITY_TRACE_DETAIL"] = "1"
    with savegame_backup.guarded(sdir):
        if os.path.exists(second_src):
            os.remove(second_src)
        if not plant and os.path.exists(os.path.join(sdir, "testgame.ims")):
            # A mission whose quicksave fixture never fires (the fixture waits out a movie or cut-scene camera) would otherwise hand back the previous
            # run's file as "its" save: two baselines of 2026-10-14 held mc2_01 saves.
            os.remove(os.path.join(sdir, "testgame.ims"))
        if plant:
            shutil.copy2(plant, os.path.join(sdir, "testgame.ims"))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", mission, "--duration", str(duration),
                            *smoke_target(install, ref_exe, ref_commit), "--keep-logs", *speed_args(sim_speed),
                            # headless: no window or GPU; the run ends on a tick budget past the quickload and its re-save
                            *(["--headless", "--ticks", str(headless_ticks)] if headless else [])], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                           **console_child.child_kwargs(redirected=True))
        with open(os.path.join(dest, "runner.txt"), "w", encoding="utf-8") as fh:
            fh.write(r.stdout)
        arts = [line.removeprefix("[runner] artifacts: ").strip() for line in r.stdout.splitlines() if line.startswith("[runner] artifacts: ")]
        if not arts or not os.path.exists(os.path.join(arts[-1], mission + ".log")):
            sys.exit("saveload_roundtrip: the smoke run produced no log:\n" + "\n".join(r.stdout.splitlines()[-8:]))
        shutil.copytree(arts[-1], os.path.join(dest, "run"), dirs_exist_ok=True)
        first = second = None
        first_src = os.path.join(sdir, "testgame.ims")
        if os.path.exists(first_src):
            first = os.path.join(dest, "save1.ims")
            shutil.copy2(first_src, first)
        if os.path.exists(second_src):
            second = os.path.join(dest, "save2.ims")
            shutil.move(second_src, second)
    with open(os.path.join(dest, "run", mission + ".log"), encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    m = _FINGERPRINT.search(text)
    rc = r.returncode
    # A headless run has no frames, so the runner's render gates (vegetation, heartbeat) fail it by design; it is clean when the engine hit the
    # tick budget and exited through its summary without a crash.
    if headless and rc == 1 and "[SMOKE v1] event=tick_budget" in text and "[SMOKE v1] event=summary result=pass" in text and "Crash Report" not in text:
        rc = 0
    return dest, first, second, ({"sha": m.group(1), "dirty": int(m.group(2))} if m else None), rc


def save_evidence(path: str | None, what: str) -> str | None:
    """-> None when `path` is a readable, non-empty save; otherwise the reason the comparison has no evidence to judge."""
    if not path:
        return f"no {what} was written: the fixture did not fire"
    if not os.path.isfile(path):
        return f"the {what} is missing ({path})"
    if os.path.getsize(path) == 0:
        return f"the {what} is empty ({path})"
    try:
        si.load(path)
    except Exception as err:  # noqa: BLE001  a truncated or corrupt file is a missing save, not a crash of the checker
        return f"the {what} is unreadable ({path}: {err})"
    return None


def resave_verdict(first: str | None, second: str | None, rc: int | None = None):
    """-> (ok, report lines): are the save and the save made right after loading it the same bytes? Names the first differing packet and offset.
    A missing, empty or unreadable save, or a child that exited non-zero, is a FAIL: a comparison of nothing is not a pass."""
    if rc:
        return False, [f"the smoke run exited {rc}: the quickload did not complete, so the re-save cannot be trusted (see the run log)"]
    if not first:
        return False, ["no quicksave was written: the quicksave fixture did not fire (SAVE_AT is a fixed tick; the run may have ended first)"]
    if not second:
        return False, ["no re-save was written: the quickload did not run or failed (see the run log)"]
    reason = save_evidence(first, "quicksave") or save_evidence(second, "re-save")
    if reason:
        return False, [reason]
    try:
        res = si.compare(si.load(first)[1], si.load(second)[1])
    except Exception as err:  # noqa: BLE001  a truncated or corrupt file is a missing save, not a crash of the checker
        return False, [f"a save could not be read ({err}): nothing was compared"]
    with open(first, "rb") as fa, open(second, "rb") as fb:
        same_file = fa.read() == fb.read()
    lines = si.describe(res)
    if res["equal"] and not res["packets_a"]:
        return False, ["both saves are empty (0 packets): nothing was compared"]
    if res["equal"] and not same_file:
        lines.append("(every packet is equal, the container bytes differ: compressor or seek table)")
    return res["equal"], lines


def mask_policy(mask_path: str | None) -> dict:
    if not mask_path:
        return {"mode": "exact"}
    mask = si.load_mask(mask_path)
    sizes = {kind: size for size, kind in si.RECORD_SIZES.items()}
    for kind, scopes in mask.items():
        if kind not in sizes:
            raise ValueError("save-noise masks require a known record class: " + kind)
        for lo, hi in scopes["run"] + scopes["build"]:
            if lo < 0 or hi < lo or hi >= sizes[kind]:
                raise ValueError("invalid save-noise mask range for " + kind)
    return {"mode": "zero-masked-bytes-v1", "across_builds": True, "classes": mask}


def masked_packets(pk, policy: dict):
    """Canonical legacy payloads retain packet kinds and sizes; only declared noise bytes become zero."""
    if policy["mode"] == "exact":
        return pk
    result = []
    for typ, raw in pk:
        kind = si.packet_kind(typ, raw)
        data = bytearray(raw)
        spans = policy["classes"].get(kind, {})
        for lo, hi in spans.get("run", []) + spans.get("build", []):
            if lo < 0 or hi < lo:
                raise ValueError("invalid save-noise mask range")
            end = min(hi + 1, len(data))
            if lo < end:
                data[lo:end] = bytes(end - lo)
        result.append((typ, bytes(data)))
    return result


def _mask_rel(mask_path):
    try:
        return os.path.relpath(mask_path, ROOT).replace(os.sep, "/")
    except ValueError:   # mask on another drive (temp dir): record it absolute
        return str(mask_path).replace(os.sep, "/")


def baseline_record(mission: str, save_at: int, load_at: int, first: str, second: str | None, fingerprint: dict | None,
                    mask_path: str | None = None, first_save_only: bool = False, rc: int | None = None) -> dict:
    pk = si.load(first)[1]
    policy = mask_policy(mask_path)
    identical, _ = resave_verdict(first, second, rc)
    ok, _ = fixture_verdict(first, second, mask_path, rc)
    pk = masked_packets(pk, policy)
    return {"mission": mission, "save_at": save_at, "load_at": load_at, "gates": resave_env(save_at, load_at), "exe": fingerprint,
            "comparison": policy,
            # WHY: the classes alone cannot tell a later re-pin which mask file produced them.
            "mask": (_mask_rel(mask_path) if mask_path else None),
            # WHY recorded: a first-save-only baseline never proved the re-save, so a reader must be able to tell it from a strong one.
            "judged": "first-save-only" if first_save_only else "first-save-and-resave",
            # WHY recorded: --first-save-only waives the re-save's CONTENT, not its existence: a run with no re-save proved nothing.
            "resave_present": save_evidence(second, "re-save") is None,
            "packets": len(pk), "overall": si.overall(pk), "resave_identical": identical, "resave_matches_policy": ok,
            "runs": [list(r) for r in si.runs(pk)],
            "about": "digest of the quicksave a round-trip fixture writes; tools/saveload_roundtrip.py --baseline / --check"}


def check_baseline_record(base: dict, cur: dict):
    """-> (ok, report lines): does a fresh round-trip save equal the recorded one, run for run?"""
    lines = []
    if not isinstance(base, dict) or not isinstance(base.get("runs"), list) or not isinstance(base.get("overall"), str):
        return False, ["malformed baseline: it needs a 'runs' list and an 'overall' digest; record it again with --baseline"]
    if not base["runs"] or not isinstance(cur.get("runs"), list) or not cur["runs"] or not cur.get("packets"):
        return False, ["nothing to compare: the baseline or this run has no packets"]
    if not cur.get("resave_present"):
        return False, ["no valid re-save this run: a missing, empty or crashed re-save is not a pass"]
    if json.dumps(base.get("comparison", {"mode": "exact"}), sort_keys=True) != json.dumps(cur.get("comparison", {"mode": "exact"}), sort_keys=True):
        return False, ["baseline comparison policy differs; historical hashes cannot be remasked: record --baseline with the same --mask"]
    # A save taken at another frame is another scenario moment: its digest cannot match, and a silent mismatch once hid a re-record at the CLI default.
    if (base.get("save_at"), base.get("load_at")) != (cur.get("save_at"), cur.get("load_at")):
        return False, [f"baseline recorded at --save-at {base.get('save_at')} --load-at {base.get('load_at')}; this run used "
                       f"{cur.get('save_at')} / {cur.get('load_at')}: pass the baseline's frames"]
    bd, cd = base.get("data_fingerprint"), cur.get("data_fingerprint")
    if bd and cd and bd.get("digest") != cd.get("digest"):
        return False, [f"the install data fingerprint {cd.get('digest')} is not the baseline's {bd.get('digest')}: the baseline pins other data "
                       "(re-record it after a data change, with the reason in the commit)" + "".join("; " + pr for pr in cd.get("problems") or [])]
    be, ce = base.get("exe") or {}, cur.get("exe") or {}
    if base.get("judged", "unrecorded") != "first-save-and-resave":
        lines.append(f"WEAK baseline: judged {base.get('judged', 'unrecorded (recorded before the field existed)')}; the re-save was not part of the verdict")
    lines.append(f"baseline exe {be.get('sha', '?')} dirty={be.get('dirty', '?')}; this run's exe {ce.get('sha', '?')} dirty={ce.get('dirty', '?')}")
    ok, msg = si.compare_runs(base["runs"], cur["runs"])
    if not ok:
        lines.append(msg)
        return False, lines
    if base["overall"] != cur["overall"]:
        lines.append(f"runs match but the overall digest differs ({base['overall']} vs {cur['overall']})")
        return False, lines
    lines.append(f"save matches the {cur.get('comparison', {'mode': 'exact'})['mode']} baseline: {cur['packets']} packets, digest {cur['overall']}")
    if not base.get("judged") or base.get("judged") == "first-save-and-resave":
        if base.get("resave_matches_policy", base.get("resave_identical")) and not cur.get("resave_matches_policy", cur.get("resave_identical")):
            lines.append("the re-save no longer matches the save's comparison policy")
            return False, lines
    return True, lines


def text_ref_path(mission: str) -> str:
    return os.path.join(BASELINE_DIR, mission + ".text.txt")


def decoded_text(first: str) -> str:
    """The FIT text packets of a save as one readable text, each under a '=== packet N ===' header (stored next to the pin, diffed on a mismatch)."""
    out = []
    for i, (typ, raw) in enumerate(si.load(first)[1]):
        if si.packet_kind(typ, raw) == "text":
            out.append(f"=== packet {i} ===\n" + raw.decode("latin-1").replace("\r\n", "\n"))
    return "\n".join(out)


def text_diff(mission: str, first: str) -> list[str]:
    ref = text_ref_path(mission)
    if not os.path.exists(ref):
        return [f"no stored text reference ({os.path.relpath(ref, ROOT)}); re-pin with --baseline to store one"]
    import difflib
    with open(ref, encoding="utf-8", newline="") as fh:
        old = fh.read().splitlines()
    d = list(difflib.unified_diff(old, decoded_text(first).splitlines(), "pinned", "this run", lineterm="", n=2))
    return d[:200] or ["decoded FIT text identical to the stored reference (drift is in binary packets)"]


def baseline_path(mission: str) -> str:
    return os.path.join(BASELINE_DIR, mission + ".json")


def masked_resave_verdict(first: str | None, second: str | None, mask_path: str, rc: int | None = None):
    """-> (ok, report lines) judging a re-save against the byte-offset noise mask instead of byte identity: ok when every differing byte lies inside it.
    WHY across builds: a planted save is written by ANOTHER exe by definition (that is what the leg proves), so the mask's cross-build ranges apply."""
    if rc:
        return False, [f"the smoke run exited {rc}: the quickload did not complete, so the re-save cannot be trusted (see the run log)"]
    reason = save_evidence(first, "quicksave") or save_evidence(second, "re-save")
    if reason:
        return False, [reason]
    res = si.compare(si.load(first)[1], si.load(second)[1], si.load_mask(mask_path), across_builds=True)
    if not res["packets_a"]:
        return False, ["both saves are empty (0 packets): nothing was compared"]
    return res["first_outside"] is None, si.describe(res)


def brain_resave_verdict(first: str | None, second: str | None, rc: int | None = None):
    """TechBrain's optional BRN1 state must survive save -> load -> re-save byte for byte."""
    if rc:
        return False, [f"the smoke run exited {rc}"]
    reason = save_evidence(first, "quicksave") or save_evidence(second, "re-save")
    if reason:
        return False, [reason]

    def brain(path):
        sections = [raw for _, raw in si.load(path)[1] if raw.startswith(si.SECTION_MAGIC)]
        return [s for raw in sections for s in si.decode_sections(raw) if s[0] == int.from_bytes(b"BRN1", "little")]

    a, b = brain(first), brain(second)
    if len(a) != 1 or len(b) != 1:
        return False, ["BRN1 missing or duplicated in quicksave/re-save"]
    return a == b, ["BRN1 save -> load -> re-save " + ("identical" if a == b else "DIFFERS")]


def fixture_verdict(first: str | None, second: str | None, mask_path: str | None = None, rc: int | None = None):
    # WHY: the mask is the policy under which a differing re-save can still pass; masked_resave_verdict also refuses a missing/empty/crashed run.
    if mask_path:
        ok, lines = masked_resave_verdict(first, second, mask_path, rc)
    else:
        ok, lines = resave_verdict(first, second, rc)
    if os.environ.get("MC2_BRAIN_POLICY") == "techbrain":
        brain_ok, brain_lines = brain_resave_verdict(first, second, rc)
        return ok and brain_ok, brain_lines + lines
    return ok, lines


def frames_from_baseline(args, base: dict | None) -> str | None:
    """Fill unset --save-at / --load-at (check: from the baseline; otherwise the CLI defaults). -> a refusal line when explicit flags
    contradict the baseline. WHY before the launch: a check at other frames cannot pass, so it must not spend a host lease and a run."""
    recorded = (base.get("save_at"), base.get("load_at")) if isinstance(base, dict) else (None, None)
    for attr, idx, default in (("save_at", 0, DEFAULT_SAVE_AT), ("load_at", 1, DEFAULT_LOAD_AT)):
        if getattr(args, attr) is None:
            setattr(args, attr, recorded[idx] if isinstance(recorded[idx], int) else default)
    if base is not None:
        missing = [key for key, value in zip(("save_at", "load_at"), recorded) if not isinstance(value, int)]
        if missing:
            return (f"baseline has no {' / '.join(missing)}; re-record it with --baseline")
    if base is not None and recorded != (args.save_at, args.load_at):
        return (f"baseline recorded at --save-at {recorded[0]} --load-at {recorded[1]}; this run asks for "
                f"{args.save_at} / {args.load_at}: omit the flags or pass the baseline's frames")
    return None


def plant_refusal(args) -> str | None:
    """-> a refusal line when --plant cannot load the planted file. WHY before the launch: the quickload fixture waits for the quicksave
    fixture, so with any nonzero --save-at the run either saves over the planted file or (when that frame is past the run) never quickloads."""
    if args.plant and args.save_at:
        return ("--plant needs --save-at 0: with a quicksave frame the run saves over the planted file, or never quickloads it when that "
                "frame is past the run; pass --save-at 0 --load-at <frame>")
    return None


def previous_baseline(path: str) -> dict | None:
    try:
        with open(path, encoding="utf-8") as fh:
            prev = json.load(fh)
    except (OSError, ValueError):
        return None
    return prev if isinstance(prev, dict) else None


def fill_from_previous(args, prev: dict | None) -> None:
    """--baseline over an existing baseline: unset save_at / load_at / sim_speed / mask / first-save-only come from it, as check does."""
    if not prev:
        return
    for attr in ("save_at", "load_at"):
        if getattr(args, attr) is None and isinstance(prev.get(attr), int):
            setattr(args, attr, prev[attr])
    if not args.sim_speed and isinstance(prev.get("sim_speed"), int):
        args.sim_speed = prev["sim_speed"]
    comp = prev.get("comparison") or {}
    if not args.mask and comp.get("mode") == "zero-masked-bytes-v1":
        recorded = prev.get("mask")
        if not isinstance(recorded, str) or not recorded:
            raise ValueError("previous baseline records a noise mask but no mask path; pass --mask <path> "
                             "(and --first-save-only if it was judged first-save-only)")
        candidate = os.path.normpath(recorded if os.path.isabs(recorded) else os.path.join(ROOT, recorded))
        if not os.path.exists(candidate):
            raise ValueError(f"previous baseline's mask {recorded} is not readable; pass --mask <path> "
                             "(and --first-save-only if it was judged first-save-only)")
        args.mask = candidate
    if comp.get("mode") == "zero-masked-bytes-v1" and "judged" not in prev:
        raise ValueError("previous baseline does not record whether it was first-save-only; pass --mask <path> "
                         "and --first-save-only if that was the original judgement")
    if prev.get("judged") == "first-save-only":
        args.first_save_only = True


def cmd_fixture(args) -> int:
    mask_policy(args.mask)
    path = baseline_path(args.mission)
    base = None
    if args.cmd == "check":
        if not os.path.exists(path):
            print(f"saveload_roundtrip check: no baseline for {args.mission} ({os.path.relpath(path, ROOT)}); record one with --baseline")
            return 1
        try:
            with open(path, encoding="utf-8") as fh:
                base = json.load(fh)
        except (OSError, ValueError) as err:
            print(f"saveload_roundtrip check: malformed baseline {os.path.relpath(path, ROOT)} ({err}); record it again with --baseline")
            return 1
    if args.cmd == "baseline":
        base = previous_baseline(path)
        try:
            fill_from_previous(args, base)
        except ValueError as exc:
            print(f"saveload_roundtrip baseline: FAIL\n{exc}")
            return 1
        base = None
    refusal = frames_from_baseline(args, base)
    if refusal:
        print(f"saveload_roundtrip {args.cmd}: FAIL\n" + refusal)
        return 1
    refusal = plant_refusal(args)
    if refusal:
        print(f"saveload_roundtrip {args.cmd}: FAIL\n" + refusal)
        return 1
    speed = args.sim_speed or (base or {}).get("sim_speed", 1)
    dest, first, second, fp, rc = run_fixture(args.mission, args.save_at, args.load_at, args.deploy, args.out, args.duration, args.plant,
                                             args.install, args.save_dir, args.ref_exe, args.ref_commit, sim_speed=speed, headless=args.headless)
    ok, lines = fixture_verdict(first, second, args.mask, rc)
    print(f"files kept in {dest}")
    if args.cmd == "resave":
        print(("saveload_roundtrip resave: PASS" if ok else "saveload_roundtrip resave: FAIL") + "\n" + "\n".join(lines))
        return 0 if ok else 1
    # WHY: --first-save-only waives the (known lossy) re-save comparison, never a crashed child or a missing first save: that is no evidence at all.
    if rc or not first or save_evidence(first, "quicksave") or save_evidence(second, "re-save"):
        print(f"saveload_roundtrip {args.cmd}: FAIL\n" + "\n".join(lines))
        return 1
    if args.mask and not ok and not args.first_save_only:
        print(f"saveload_roundtrip {args.cmd}: FAIL\n" + "\n".join(lines))
        return 1
    if args.mask and not ok:
        print("the re-save leaves the mask (information only, --first-save-only):\n" + "\n".join(lines[:3]))
    rec = baseline_record(args.mission, args.save_at, args.load_at, first, second, fp, args.mask, args.first_save_only, rc)
    rec["sim_speed"] = speed
    # WHY: a baseline pins the declared stock-plus-overlay data (tools/sim_determinism.py), not whatever a lane's install carries.
    rec["data_fingerprint"] = sd.data_fingerprint(args.install)
    if args.cmd == "baseline":
        if rec["data_fingerprint"]["problems"]:
            print("saveload_roundtrip baseline: the install is not the declared stock-plus-overlay data set; nothing written:\n  "
                  + "\n  ".join(rec["data_fingerprint"]["problems"]))
            return 1
        os.makedirs(BASELINE_DIR, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(rec, fh, indent=1)
            fh.write("\n")
        with open(text_ref_path(args.mission), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(decoded_text(first))
        print(f"saveload_roundtrip: wrote {os.path.relpath(path, ROOT)} ({rec['packets']} packets, digest {rec['overall']}, re-save exact: {rec['resave_identical']}, policy match: {rec['resave_matches_policy']})")
        return 0
    ok, lines = check_baseline_record(base, rec)
    if not ok:
        lines += ["FIT text diff against the stored reference:"] + text_diff(args.mission, first)
    print(("saveload_roundtrip check: PASS" if ok else "saveload_roundtrip check: FAIL") + "\n" + "\n".join(lines))
    return 0 if ok else 1


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ("--baseline", "--check"):
        argv[0] = argv[0][2:]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, helptext in (("resave", "save, quickload, save again; the two files must be identical"),
                           ("baseline", "record the digest of the round-trip save for a mission"),
                           ("check", "compare a fresh round-trip save with the recorded digest")):
        p = sub.add_parser(name, help=helptext)
        p.add_argument("--mission", default="mc2_01")
        p.add_argument("--save-at", type=int, help=f"fixed tick of the quicksave (check: the baseline's; else {DEFAULT_SAVE_AT})")
        p.add_argument("--load-at", type=int, help=f"fixed tick of the quickload (check: the baseline's; else {DEFAULT_LOAD_AT})")
        p.add_argument("--duration", type=int, default=28)
        p.add_argument("--deploy", action="store_true")
        p.add_argument("--out", help="keep save1.ims, save2.ims and the run here (default: a temporary directory)")
        p.add_argument("--plant", help="resave: put this save where the quickload reads it instead of writing one; requires --save-at 0 "
                                        "(the engine writes no re-save after a planted load)")
        p.add_argument("--mask", help="legacy noise mask: resave ignores declared run/build bytes; baseline/check hash zeroed noise bytes and require the same mask policy (old hashes must be re-recorded)")
        p.add_argument("--install", type=sd.resolve_install, default=sd.INSTALL_DIR, metavar="NAME_OR_PATH", help="launch/deploy install, e.g. lane3")
        p.add_argument("--save-dir", help="backup/capture directory matching the engine's savePath (Windows installs share ~/.mechcommander2/savegame)")
        sd.add_ref_args(p)
        add_speed_arg(p)
        p.add_argument("--headless", action="store_true", help="run the fixture headless (no window, GPU or audio; --ticks past the load): the save bytes must match the windowed baseline")
        p.add_argument("--first-save-only", action="store_true",
                       help="baseline/check with --mask: judge only the first save against the baseline and do not fail because the re-save leaves the mask "
                            "(the quickload is lossy: any save and its re-save differ in about 1159 of 20000 packets, so the re-save gate cannot pass); "
                            "the re-save verdict is still printed and recorded as information")
    r = sub.add_parser("run")
    r.add_argument("--mission", default="mc2_03")
    r.add_argument("--frames", type=int, default=60, help="post-load fixed ticks that must match (1..60)")
    r.add_argument("--save-at", type=int, default=int(SAVE_AT), help="fixed tick of the quicksave (may wait for the intro camera)")
    r.add_argument("--load-at", type=int, default=int(LOAD_AT), help="fixed tick of the quickload")
    r.add_argument("--headless", action="store_true", help="reach the fixtures on a fixed tick budget without rendering")
    r.add_argument("--duration", type=int, default=28, help="smoke timeout in seconds")
    r.add_argument("--deploy", action="store_true")
    r.add_argument("--out")
    r.add_argument("--install", type=sd.resolve_install, default=sd.INSTALL_DIR, metavar="NAME_OR_PATH")
    r.add_argument("--save-dir", help="directory matching the engine's savePath")
    sd.add_ref_args(r)
    add_speed_arg(r)
    a = sub.add_parser("analyze")
    a.add_argument("log")
    a.add_argument("--frames", type=int, default=60)
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    if args.cmd in ("resave", "baseline", "check"):
        return cmd_fixture(args)
    if args.cmd == "analyze":
        with open(args.log, encoding="utf-8", errors="replace") as fh:
            ok, rep = analyze(fh.read().splitlines(), args.frames)
    else:
        ok, rep = run(args.mission, args.frames, args.deploy, args.out, args.install, args.save_dir, args.ref_exe, args.ref_commit, args.sim_speed or 1,
                      args.save_at, args.load_at, args.duration, args.headless)
    print(("saveload_roundtrip: PASS" if ok else "saveload_roundtrip: FAIL") + "\n" + "\n".join(rep))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
