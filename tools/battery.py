#!/usr/bin/env python3
"""battery - one ordered driver for a ~2-hour battery session.

WHAT: runs the battery tools in the run-book order and prints one summary table. It shells out to the existing tools and
reimplements none of them. A step whose tool is not in this tree is reported NEEDS_REVIEW, never PASS.
WHY: a battery session is a fixed sequence with a fixed budget; a driver keeps the order, the staging dirs and the verdicts
in one place instead of a hand-typed list of commands.

  py -3 tools/battery.py --owner-present
  py -3 tools/battery.py --dry-run
  py -3 tools/battery.py --only preflight,resave --install A:/Games/mc2-opengl/mc2-win64-v0.4
  py -3 tools/battery.py --skip perf,simtrace
  py -3 tools/battery.py --list

Steps (run-book order): preflight, resave, campaign, sound, determinism, mods, tools, perf, reload, simtrace, triage.
Staging: disposable copies go under G:/mc2-battery/<step>/ (created). Nothing is written inside a game install except by the
runners' own --deploy paths, and mc2-win64-v0.4c is refused outright.
--owner-present: the visible Tracy window and the sound listen need the owner at the desk; without the flag those two items
are recorded NEEDS_REVIEW rather than substituting minimized FPS numbers or a dummy audio driver.
Exit: 0 every selected step passed; 1 any step failed; 2 a step needs review.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import subprocess
import sys
from collections import namedtuple
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "scripts"))
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

DEFAULT_STAGE = "G:/mc2-battery"
DEFAULT_INSTALL = "A:/Games/mc2-opengl/mc2-win64-v0.4"
FORBIDDEN_INSTALL = "mc2-win64-v0.4c"
RESAVE_MISSIONS = ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24")
#: mc2_10 last: it is the slow one under host load (run-book step 5).
DET_MISSIONS = ("mc2_01", "mc2_03", "mc2_16", "mc2_17", "mc2_24", "mc2_10")
TOOL_CAPTURES = ("asset_viewer", "mc2fx_preview", "mc2weapon_gui")

PASS, FAIL, NEEDS_REVIEW, DRY = "PASS", "FAIL", "NEEDS_REVIEW", "DRY"
#: A child killed by our own timeout (a busy host lease, a stuck launch) is not a verdict on the build.
RC_TIMEOUT = 124

Run = namedtuple("Run", "argv rc text out dry")
Result = namedtuple("Result", "verdict detail artifact rc", defaults=(None,))
Step = namedtuple("Step", "id budget title fn")


def first_line(text, limit=160):
    for ln in (text or "").splitlines():
        ln = ln.strip()
        if ln:
            return ln[:limit]
    return ""


def json_tail(text):
    """The JSON object a tool printed after its human-readable lines, or None.

    repo_query.py prints a summary line and then the JSON document; it never calls sys.exit, so its exit code says nothing
    about the verdict and the JSON is the only honest source.
    """
    start = (text or "").find("\n{")
    if start < 0:
        return None
    try:
        return json.loads(text[start + 1:])
    except ValueError:
        return None


def guard(argv):
    """Refuse the install the run-book forbids, whatever a step or a caller asks for."""
    joined = " ".join(str(a) for a in argv).lower().replace("\\", "/")
    if FORBIDDEN_INSTALL in joined:
        raise RuntimeError(f"refusing to touch {FORBIDDEN_INSTALL} (run-book rule)")


def smoke_target(install):
    """run_smoke.py arguments naming the install; reuses tools/sim_determinism.py's lane lookup.

    Imported lazily: sim_determinism pulls in repo_intel, and this module must stay importable without side effects.
    """
    sys.path.insert(0, str(ROOT / "tools"))
    from sim_determinism import smoke_target as _target
    return _target(install)


class Ctx:
    def __init__(self, args):
        self.args = args
        self.dry = args.dry_run
        self.install = args.install
        self.stage = Path(args.stage_root)
        self.results = []

    def stage_dir(self, name):
        d = self.stage / name
        if not self.dry:
            d.mkdir(parents=True, exist_ok=True)
        return d

    def run(self, argv, env=None, cwd=None, out=None, timeout=None):
        """Run one child, keeping its stdout/stderr under `out`. In a dry run it only prints the command line."""
        argv = [str(a) for a in argv]
        guard(argv)
        if self.dry:
            print(f"  [dry-run] {subprocess.list2cmdline(argv)}", flush=True)
            return Run(argv, 0, "", out, True)
        print(f"  $ {subprocess.list2cmdline(argv)}", flush=True)
        try:
            p = subprocess.run(argv, cwd=str(cwd or ROOT), env=dict(os.environ, **(env or {})),
                               capture_output=True, text=True, errors="replace", timeout=timeout,
                               **console_child.child_kwargs(redirected=True))
        except subprocess.TimeoutExpired as exc:
            return Run(argv, RC_TIMEOUT, f"timed out after {timeout}s: {exc}", out, False)
        except OSError as exc:
            return Run(argv, 127, f"{type(exc).__name__}: {exc}", out, False)
        text = (p.stdout or "") + (p.stderr or "")
        if out:
            Path(out).mkdir(parents=True, exist_ok=True)
            (Path(out) / "stdout.txt").write_text(p.stdout or "", encoding="utf-8", errors="replace")
            (Path(out) / "stderr.txt").write_text(p.stderr or "", encoding="utf-8", errors="replace")
        return Run(argv, p.returncode, text, out, False)


# ---------------------------------------------------------------------------
# Steps, in run-book order
# ---------------------------------------------------------------------------

def step_preflight(ctx):
    out = ctx.stage_dir("preflight")
    r = ctx.run([sys.executable, ROOT / "tools/repo_intel/repo_query.py", "preflight",
                 "--expect-branch", "claude/nifty-mendeleev",
                 "--expect-root", "A:/Games/mc2-flash3"], out=out, timeout=300)
    if r.dry:
        return Result(DRY, "", out)
    if r.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, first_line(r.text), out, r.rc)
    doc = json_tail(r.text)
    if doc is None:
        return Result(FAIL, "no JSON verdict in the preflight output: " + first_line(r.text), out, r.rc or 1)
    summary = next((ln for ln in r.text.splitlines() if ln.startswith("PRECHECK:")), "")
    if not doc.get("ok"):
        dirty = doc.get("dirty") or {}
        why = dirty.get("branch_warning") or dirty.get("root_warning") or "dirty tree or undocumented harness"
        return Result(FAIL, f"{why} ({summary})", out, 1)
    return Result(PASS, summary, out, 0)


def step_resave(ctx):
    out = ctx.stage_dir("resave")
    bad, unrecorded, rc = [], [], 0
    for m in RESAVE_MISSIONS:
        r = ctx.run([sys.executable, ROOT / "tools/saveload_roundtrip.py", "check",
                     "--mission", m, "--mask", ROOT / "tools/save_noise_mask.json",
                     "--first-save-only", "--install", ctx.install],
                    out=out / m, timeout=900)
        if r.dry:
            continue
        if r.rc == 0:
            continue
        if r.rc == RC_TIMEOUT:
            unrecorded.append((m, first_line(r.text)))
        elif "no baseline for" in r.text or "malformed baseline" in r.text:
            unrecorded.append((m, first_line(r.text)))
        else:
            bad.append((m, first_line(r.text)))
            rc = rc or r.rc
    if ctx.dry:
        return Result(DRY, "", out)
    if bad:
        return Result(FAIL, f"{bad[0][0]}: {bad[0][1]}", out, rc)
    if unrecorded:
        return Result(NEEDS_REVIEW,
                      f"{len(unrecorded)} mission(s) without a usable baseline: "
                      + ", ".join(m for m, _ in unrecorded), out, 0)
    return Result(PASS, f"{len(RESAVE_MISSIONS)} missions, masked digest matched (not exact byte identity)", out, 0)


def step_campaign(ctx):
    out = ctx.stage_dir("campaign")
    cap = ctx.run([sys.executable, ROOT / "tools/campaign_save_roundtrip.py", "capture",
                   "--out", out, "--deploy", ctx.install, "--save-at", "2400",
                   "--timeout", "180", "--check-screens", "--salvage"],
                  out=out / "capture", timeout=900)
    if cap.dry:
        return Result(DRY, "", out)
    if cap.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, first_line(cap.text), out, cap.rc)
    if cap.rc == 2:
        # The tool's own host-lease refusal: another run holds the host, not a defect in this build.
        return Result(NEEDS_REVIEW, "host game lease busy: " + first_line(cap.text), out, cap.rc)
    if cap.rc != 0:
        return Result(FAIL, first_line(cap.text), out, cap.rc)
    save = out / "save1.ims"
    if not save.is_file():
        return Result(FAIL, "no save1.ims written", out, 1)
    ctx.run([sys.executable, ROOT / "tools/campaign_save_roundtrip.py", "logistics", save],
            out=out / "logistics", timeout=300)
    # The [SOAK] markers are the ENGINE's stdout, which the capture writes to <--out>/stdout.txt.
    log_path = out / "stdout.txt"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else ""
    screens = sorted({ln.split("screen=", 1)[1].split()[0] for ln in log.splitlines()
                      if "[SOAK] screen-check" in ln and "screen=" in ln})
    salvage = [ln for ln in log.splitlines() if "[SOAK] salvage-" in ln]
    missing = [s for s in ("purchase", "loadout") if s not in screens]
    if missing or not salvage:
        return Result(FAIL, f"missing markers: screens={','.join(screens) or 'none'} salvage={len(salvage)}", out, 1)
    return Result(PASS, f"screens={','.join(screens)} salvage lines={len(salvage)}", out, 0)


def step_sound(ctx):
    out = ctx.stage_dir("sound")
    r = ctx.run([sys.executable, ROOT / "scripts/run_smoke.py", "--mission", "mc2_01",
                 "--duration", "30", *smoke_target(ctx.install), "--keep-logs"],
                env={"SDL_AUDIODRIVER": "directsound"}, out=out, timeout=1800)
    if r.dry:
        return Result(DRY, "", out)
    if r.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, first_line(r.text), out, r.rc)
    arts = [ln.split("[runner] artifacts: ", 1)[1].strip() for ln in r.text.splitlines()
            if ln.startswith("[runner] artifacts: ")]
    if not arts:
        return Result(FAIL, "no artifacts dir in the runner output: " + first_line(r.text), out, r.rc or 1)
    src = Path(arts[-1]) / "mc2_01.log"
    text = src.read_text(encoding="utf-8", errors="replace") if src.is_file() else ""
    if src.is_file():
        (out / "mc2_01.log").write_text(text, encoding="utf-8", errors="replace")
    if "Opened audio at" not in text:
        return Result(FAIL, "no 'Opened audio at' in the mission log", out, 1)
    errs = [ln for ln in text.splitlines()
            if "sound" in ln.lower() and ("error" in ln.lower() or "failed" in ln.lower())]
    if errs:
        return Result(FAIL, errs[0].strip(), out, 1)
    if not ctx.args.owner_present:
        return Result(NEEDS_REVIEW, "log clean; owner listen not done (--owner-present)", out, 0)
    return Result(PASS, "Opened audio at; no sound-load errors", out, 0)


def step_determinism(ctx):
    out = ctx.stage_dir("det")
    bad, starved, rc = [], [], 0
    for m in DET_MISSIONS:
        r = ctx.run([sys.executable, ROOT / "tools/sim_determinism.py", "pairs-until-fail",
                     "--mission", m, "--max", "2", "--duration", "60", "--sim-speed", "4",
                     "--out", out / m, "--install", ctx.install],
                    out=out / m / "run", timeout=3600)
        if r.dry:
            continue
        if r.rc == 2:
            starved.append(m)
        elif r.rc == RC_TIMEOUT:
            starved.append(m)
        elif r.rc != 0:
            bad.append((m, first_line(r.text)))
            rc = rc or r.rc
    if ctx.dry:
        return Result(DRY, "", out)
    if bad:
        return Result(FAIL, f"{bad[0][0]}: {bad[0][1]}", out, rc)
    if starved:
        return Result(NEEDS_REVIEW, f"starved or timed out (host too busy): {','.join(starved)}", out, 0)
    return Result(PASS, f"{len(DET_MISSIONS)} missions x 2 pairs identical", out, 0)


def step_mods(ctx):
    out = ctx.stage_dir("mods")
    tool = ROOT / "tools/mod_smoke.py"
    if not tool.is_file():
        return Result(NEEDS_REVIEW, "tools/mod_smoke.py not in this tree (run-book step 6); stage the six mods by hand", out)
    r = ctx.run([sys.executable, tool, "--install", ctx.install, "--out", out],
                out=out / "run", timeout=3600)
    if r.dry:
        return Result(DRY, "", out)
    if r.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, first_line(r.text), out, r.rc)
    return Result(PASS if r.rc == 0 else FAIL, first_line(r.text), out, r.rc)


def step_tools(ctx):
    out = ctx.stage_dir("tools")
    missing, bad, rc = [], [], 0
    for tool in TOOL_CAPTURES:
        r = ctx.run([sys.executable, ROOT / "scripts/run_tool_capture.py", tool,
                     "--out", out / f"{tool}.png", "--validation",
                     "--tree", ctx.args.tools_tree],
                    out=out / tool, timeout=600)
        if r.dry:
            continue
        if r.rc == 2:
            missing.append(tool)
        elif r.rc == RC_TIMEOUT:
            missing.append(tool)
        elif r.rc != 0:
            bad.append((tool, first_line(r.text)))
            rc = rc or r.rc
    if ctx.dry:
        return Result(DRY, "", out)
    if bad:
        return Result(FAIL, f"{bad[0][0]}: {bad[0][1]}", out, rc)
    if missing:
        return Result(NEEDS_REVIEW,
                      f"not built or timed out: {','.join(missing)} "
                      f"(ENABLE_MC2FX_PREVIEW=ON, ENABLE_MC2WEAPON_GUI=ON)", out)
    return Result(PASS, f"{len(TOOL_CAPTURES)} tools captured with --validation", out, 0)


def step_perf(ctx):
    out = ctx.stage_dir("perf")
    name = ctx.args.perf_name or "battery-" + datetime.date.today().isoformat()
    base = ROOT / "release_assets" / "tracy-baselines" / name
    env = {"TRACY_TOOLS_DIR": ctx.args.tracy_tools} if ctx.args.tracy_tools else None
    base_cmd = [sys.executable, ROOT / "tools/tracy_capture_compare.py", "baseline",
                "--install", ctx.install, "--name", name, "--out-dir", base]
    reg_cmd = [sys.executable, ROOT / "tools/tracy_capture_compare.py", "regress",
               "--baseline", base, "--install", ctx.install, "--out-dir", out / "regress"]
    if ctx.dry:
        ctx.run(base_cmd, env=env, out=out / "baseline")
        ctx.run(reg_cmd, env=env, out=out / "regress-run")
        return Result(DRY, "", out)
    if not ctx.args.owner_present:
        return Result(NEEDS_REVIEW, "visible Tracy window needs the owner (--owner-present); not launched", out)
    b = ctx.run(base_cmd, env=env, out=out / "baseline", timeout=1800)
    if b.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, "baseline: " + first_line(b.text), out, b.rc)
    if b.rc != 0:
        return Result(FAIL, "baseline: " + first_line(b.text), out, b.rc)
    g = ctx.run(reg_cmd, env=env, out=out / "regress-run", timeout=1800)
    if g.rc == 0:
        return Result(PASS, f"regress PASS vs {name}", out, 0)
    if g.rc == 1:
        return Result(NEEDS_REVIEW, "regress WARN (5-10% frame drop)", out, 1)
    if g.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, "regress: " + first_line(g.text), out, g.rc)
    return Result(FAIL, "regress FAIL: " + first_line(g.text), out, g.rc)


def step_reload(ctx):
    out = ctx.stage_dir("reload")
    tool = ROOT / "tools/reload_leak.py"
    if not tool.is_file():
        return Result(NEEDS_REVIEW, "tools/reload_leak.py not in this tree (run-book step 9)", out)
    r = ctx.run([sys.executable, tool, "--install", ctx.install, "--out", out],
                out=out / "run", timeout=3600)
    if r.dry:
        return Result(DRY, "", out)
    if r.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, first_line(r.text), out, r.rc)
    return Result(PASS if r.rc == 0 else FAIL, first_line(r.text), out, r.rc)


def step_simtrace(ctx):
    out = ctx.stage_dir("simtrace")
    tool = ROOT / "tools/simtrace_ab.py"
    if not tool.is_file():
        return Result(NEEDS_REVIEW, "tools/simtrace_ab.py not in this tree (run-book step 10)", out)
    r = ctx.run([sys.executable, tool, "--install", ctx.install, "--out", out],
                out=out / "run", timeout=3600)
    if r.dry:
        return Result(DRY, "", out)
    if r.rc == RC_TIMEOUT:
        return Result(NEEDS_REVIEW, first_line(r.text), out, r.rc)
    return Result(PASS if r.rc == 0 else FAIL, first_line(r.text), out, r.rc)


def step_triage(ctx):
    """Write the per-item summary the run-book's handoff asks for. The driver prints the same table."""
    out = ctx.stage_dir("triage")
    if ctx.dry:
        return Result(DRY, "", out)
    rows = [{"step": s.id, "budget": s.budget, "verdict": r.verdict, "rc": r.rc,
             "detail": r.detail, "artifact": str(r.artifact)} for s, r in ctx.results]
    (out / "summary.json").write_text(json.dumps(rows, indent=1) + "\n", encoding="utf-8")
    (out / "summary.md").write_text(render_table(ctx.results) + "\n", encoding="utf-8")
    return Result(PASS, f"{len(rows)} item(s) written", out, 0)


STEPS = [
    Step("preflight", "0:00-0:05", "repo preflight + build currency", step_preflight),
    Step("resave", "0:05-0:10", "re-save identity (masked, first save only)", step_resave),
    Step("campaign", "0:10-0:15", "campaign flow: screens + salvage + between-mission save", step_campaign),
    Step("sound", "0:15-0:20", "directsound mission, audio log assertions", step_sound),
    Step("determinism", "0:20-0:35", "determinism soak, 2 pairs per mission at 4x", step_determinism),
    Step("mods", "0:35-0:50", "six-mod smoke", step_mods),
    Step("tools", "0:50-1:00", "viewport tool captures with --validation", step_tools),
    Step("perf", "1:00-1:10", "Tracy baseline then regress", step_perf),
    Step("reload", "1:10-1:20", "10-reload leak", step_reload),
    Step("simtrace", "1:20-1:50", "sim-trace A/B across two trees", step_simtrace),
    Step("triage", "1:50-2:00", "summary and handoff", step_triage),
]


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def render_table(results):
    head = ("step", "budget", "verdict", "rc", "artifact")
    rows = [(s.id, s.budget, r.verdict, "-" if r.rc is None else str(r.rc), str(r.artifact))
            for s, r in results]
    widths = [max([len(head[i])] + [len(row[i]) for row in rows]) for i in range(len(head))]
    def line(vals):
        return "  ".join(v.ljust(widths[i]) if i < 2 else v for i, v in enumerate(vals))
    out = [line(head), line(["-" * w for w in widths])]
    out += [line(row) for row in rows]
    details = [f"  {s.id}: {r.detail}" for s, r in results if r.detail]
    if details:
        out += ["", "details:"] + details
    return "\n".join(out)


def main(argv=None):
    ids = [s.id for s in STEPS]
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Steps: " + ", ".join(ids) + ". Staging: <stage-root>/<step>/.")
    ap.add_argument("--only", default="", metavar="IDS",
                    help="comma-separated step ids to run (default: all)")
    ap.add_argument("--skip", default="", metavar="IDS",
                    help="comma-separated step ids to skip")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the commands and launch nothing")
    ap.add_argument("--install", default=DEFAULT_INSTALL,
                    help=f"game install the runners deploy to and launch (default: {DEFAULT_INSTALL})")
    ap.add_argument("--stage-root", default=DEFAULT_STAGE,
                    help=f"disposable staging root, one dir per step (default: {DEFAULT_STAGE})")
    ap.add_argument("--tools-tree", default="build-alltargets",
                    help="build tree holding the viewport tool exes (run_tool_capture --tree)")
    ap.add_argument("--owner-present", action="store_true",
                    help="the owner is at the desk for the visible Tracy window and the sound listen; "
                         "without it those two items are recorded NEEDS_REVIEW")
    ap.add_argument("--perf-name", default="",
                    help="Tracy baseline name (default: battery-<date>)")
    ap.add_argument("--tracy-tools", default="",
                    help="TRACY_TOOLS_DIR for the perf step (default: the tool's own default)")
    ap.add_argument("--list", action="store_true", help="list the steps and exit")
    args = ap.parse_args(argv)

    if args.list:
        for s in STEPS:
            print(f"{s.id:12s} {s.budget:12s} {s.title}")
        return 0

    only = [x.strip() for x in args.only.split(",") if x.strip()]
    skip = [x.strip() for x in args.skip.split(",") if x.strip()]
    unknown = [x for x in only + skip if x not in ids]
    if unknown:
        ap.error(f"unknown step(s): {', '.join(unknown)}; valid: {', '.join(ids)}")
    chosen = [s for s in STEPS if (not only or s.id in only) and s.id not in skip]
    if not chosen:
        ap.error("no step selected")

    ctx = Ctx(args)
    print(f"battery: {len(chosen)} step(s), stage {ctx.stage}" + (" (dry run)" if args.dry_run else ""))
    for step in chosen:
        print(f"\n[{step.id}] {step.budget} {step.title}", flush=True)
        try:
            res = step.fn(ctx)
        except Exception as exc:  # noqa: BLE001 -- one broken step must not lose the rest of the session
            res = Result(FAIL, f"{type(exc).__name__}: {exc}", ctx.stage / step.id)
        ctx.results.append((step, res))
        print(f"  -> {res.verdict}" + (f": {res.detail}" if res.detail else ""), flush=True)

    print("\n" + render_table(ctx.results))
    if args.dry_run:
        return 0
    graded = [r for s, r in ctx.results if s.id != "triage"]
    if any(r.verdict == FAIL for r in graded):
        return 1
    if any(r.verdict == NEEDS_REVIEW for r in graded):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
