#!/usr/bin/env python3
"""run_tools_tests.py -- the `tools_tests` check-contracts row.

WHY THIS EXISTS
---------------
scripts/tests/ is run by the `scripts_tests` row, but ~60 more pytest modules
live next to the tools they test (tools/terrain_beautify, tools/unit_converter,
tools/blender_pipeline, tests/smoke, tests/terrain_gen, ...). Nothing ran them,
so they could rot green-by-omission (see the SCRIPTS-TESTS-WIRE-1 note in
check-contracts.sh). Owner ruling 2026-10-09: they are live, so run them.

WHAT IT DOES
------------
* Discovers `test_*.py` / `*_test.py` via `git ls-files` (committed state, so a
  stray untracked file cannot flap the row), minus scripts/tests/ (its own row)
  and EXCLUDE (already driven by another row).
* Runs each module in its OWN pytest process (several modules share a basename
  such as test_manifest.py, which collides in one process without __init__.py),
  up to JOBS at a time. `-B -p no:cacheprovider` keep it read-only.
* Reconciles failures against scripts/tools_tests.known-failures, the same
  discipline as the repo's KNOWN rows:
    - a failure NOT in the ledger            -> FAIL (regression)
    - a ledgered failure that now PASSES      -> FAIL (stale ledger: delete line)
    - a ledger entry whose module is gone     -> FAIL (stale ledger)
  Ledger lines (`#` comments and blanks ignored):
    FAIL <nodeid-or-module> :: <first line of the failure>
  A module-level entry pins a collection error or a failing script-style test.
* Script-style modules (plain `python file.py` tests; pytest exits 5 with
  nothing collected) are run as `py -3 file.py` and must exit 0.
* Exit 0 = all green; 3 = every failure is pinned (the row renders KNOWN, not
  PASS); 1 = regression or stale ledger.

Pure stdlib. Read-only.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
from pathlib import Path
import tempfile
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402

# Every spawn captures its output, so child_kwargs(redirected=True) adds the
# no-window flag only when this process has no console (no conhost per child).

# Every module below runs in a child that inherits this: no test may touch a person's pointer.
os.environ["MC2_CURSOR_GUARD_OFF"] = "1"
# A private host lease file per test process: tests that spawn a fake engine contend with each other, never with the lanes
# (scripts/smoke_lib/host_lease.py raises when a test reaches the real lease without this).
os.environ.setdefault("MC2_HOST_LEASE_PATH", os.path.join(tempfile.gettempdir(), f"mc2-host-lease-test-{os.getpid()}.json"))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEDGER = os.path.join(ROOT, "scripts", "tools_tests.known-failures")

# Modules driven by a different check-contracts row; running them again here
# would only double the cost.
EXCLUDE = {
    "tests/smoke/test_mod_smoke_staging.py",  # row mod_smoke
    "tools/cxx17_ratchet/test_cxx17_ratchet.py",  # row cxx17_ratchet_selftest
    "tools/vfx_oracle/test_compare_combat_traces.py",  # collected by test_vfx_parity_oracle
    # Assembles release zips and launches a game smoke: needs artifacts + a
    # user-present session, never part of an offline contracts row.
    "scripts/test_release_zip_install.py",
}

PYTEST = [sys.executable, "-B", "-m", "pytest", "-q", "-p", "no:cacheprovider",
          "--tb=short", "-rfEs"]
TIMEOUT_S = int(os.environ.get("MC2_TOOLS_TEST_TIMEOUT_S") or 1200)  # box_hygiene: 479 s alone, over 600 s under precheck load; --timeout-s / env override


ART_PREFIXES = ("tools/scifi_kit/", "tools/blender_pipeline/", "tools/kit/", "tools/mc2data/", "tools/content_pipeline/", "tools/ase_to_glb.py", "tools/asset_cook/", "tools/material_cook/", "tools/mc2texcook/", "tools/mc2x_import/", "release_assets_src/", "examples/mods/", "mods/", "gameassets/", "data/")


def art_changed(base: str) -> bool:
    result = subprocess.run(["git", "diff", "--name-only", "-z", base, "HEAD"], cwd=ROOT,
                            capture_output=True, text=True, check=True,
                            **console_child.child_kwargs(redirected=True))
    return any(p.startswith(ART_PREFIXES) or p.endswith((".blend", ".glb", ".ktx2", ".png", ".tga", ".fit", ".pak", ".fst"))
               or p.startswith(("scripts/kit", "scripts/run_tools_tests", "scripts/console_child", "scripts/test_audit_site/", "requirements", "pyproject.toml", "pytest.ini", "conftest.py"))
               for p in (line.replace("\\", "/").lower() for line in result.stdout.split("\0")))


def is_art_test(path):
    return path.startswith(("tools/scifi_kit/", "tools/blender_pipeline/"))


def scoped_modules(mods, paths):
    """Directory-local tests follow their tool; flat tests follow the named source or row's existing input list.

    Unmapped paths are reported for nightly coverage. The module itself always triggers its test.
    """
    import contracts_cache as cc
    from fnmatch import fnmatchcase
    paths = [p for p in paths if p]
    row_inputs = ("tests/unit/", "tests/saveload/baselines/", "tests/smoke/sim_determinism/")
    declarations = cc.load_decls(ROOT)
    selected, covered = [], set()
    # A path mapped to the other aggregate is still mapped, so a scripts-only
    # change need not trigger every tools module (and vice versa).
    mapping = sorted(set(mods) | set(discover("all")) | {
        "scripts/tests/" + name for name in os.listdir(os.path.join(ROOT, "scripts/tests"))
        if name.startswith("test_") and name.endswith(".py")})
    for mod in mapping:
        parent, name = mod.rsplit("/", 1)
        stem = name.removeprefix("test_").removesuffix("_test.py").removesuffix(".py")
        prefixes = [parent + "/"] if parent not in ("tools", "tests", "scripts/tests") else []
        if parent.startswith("tests/"):
            prefixes += ["tools/" + parent[6:] + "/"]
        if "/tests" in parent and parent != "scripts/tests":
            prefixes += [parent.split("/tests")[0] + "/"]
        globs = [f"{d}/{s}*" for d in ("scripts", "tools") for s in (stem, stem.replace("_", "-"))]
        if stem == "merge_speedups":
            globs += ["scripts/deploy_payload.py"]
        # Flat helper tests sometimes carry a prefix/suffix around the tool name.
        globs += [p for p in paths if p.startswith(("scripts/", "tools/")) and p.endswith(".py")
                  and Path(p).stem in stem]
        if stem.startswith(("lane_merge", "laptop_gate", "merge_gate")):
            globs += ["tools/lane_*", "tools/laptop_gate.py", "scripts/merge_gate.py"]
        specs = [s for s in declarations.get(stem, []) if s != "." and not s.startswith(("!", "+"))]
        matches = {p for p in paths if p == mod or p.startswith(tuple(prefixes))
                   or any(fnmatchcase(p, g) for g in globs) or any(cc._match(p, s) for s in specs)}
        if matches:
            covered.update(matches)
            if mod in mods:
                selected.append(mod)
    if not selected and mods and all(is_art_test(m) for m in mods) and any(p.startswith(ART_PREFIXES) for p in paths):
        return mods
    owned = [p for p in paths if p.startswith(row_inputs) or "allowlist" in p]
    if owned:
        print("ROW INPUTS (native/baseline/allowlist): " + ", ".join(owned))
    unmapped = [p for p in paths if p not in covered and p not in owned and not p.startswith(("docs/", ".claude/")) and not p.endswith((".md", ".rst"))]
    if unmapped:
        print("UNMAPPED (nightly): " + ", ".join(unmapped))
    return selected


def discover(suite="all") -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "*test_*.py", "*_test.py"],
        cwd=ROOT, capture_output=True, text=True, check=True,
        **console_child.child_kwargs(redirected=True)).stdout
    mods = []
    for p in out.splitlines():
        base = os.path.basename(p)
        if not (base.startswith("test_") or base.endswith("_test.py")):
            continue
        if p.startswith(("scripts/tests/", "3rdparty/")) or p in EXCLUDE:  # 3rdparty: vendored upstream tests (crashpad), not ours
            continue
        if not os.path.isfile(os.path.join(ROOT, p)):
            continue  # deleted in the working tree, not yet committed
        is_art = is_art_test(p)
        if suite == "all" or is_art == (suite == "art"):
            mods.append(p)
    return sorted(mods)


def load_ledger() -> dict[str, str]:
    fails: dict[str, str] = {}
    if not os.path.isfile(LEDGER):
        return fails
    with open(LEDGER, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            m = re.match(r"FAIL (\S+) :: (.+)$", line)
            if not m:
                raise SystemExit(f"{LEDGER}:{n}: malformed ledger line: {line!r}")
            fails[m.group(1)] = m.group(2)
    return fails


def run_module(mod: str, env=None) -> tuple[str, int, list[str], str]:
    """Run one module; retry ONCE when it failed without naming a test.

    A non-zero exit with no FAILED/ERROR node id (except a timeout) is an infrastructure hiccup
    (measured: a transient WinError 2 during collection while ~16 contract rows
    ran in parallel), not a test verdict. A real test failure is deterministic
    and is never retried.
    """
    res = _run_module_once(mod, env)
    if res[1] not in (0, 124) and not res[2]:
        res = _run_module_once(mod, env)
    return res


def _run_module_once(mod: str, env=None) -> tuple[str, int, list[str], str]:
    """Return (module, rc, failed node ids, tail of output)."""
    try:
        p = subprocess.run(PYTEST + [mod], cwd=ROOT, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=TIMEOUT_S, env=env,
                           **console_child.child_kwargs(redirected=True))
        text, rc = (p.stdout or "") + (p.stderr or ""), p.returncode
        if rc == 5 and re.search(r"\b[1-9]\d* skipped\b", text):
            rc = 0  # module-level pytest.skip: no collected tests, not a script
        if rc == 5:  # nothing collected: script-style test, run it directly
            p = subprocess.run([sys.executable, "-B", mod], cwd=ROOT,
                               capture_output=True, text=True, encoding="utf-8", errors="replace",
                               timeout=TIMEOUT_S, env=env,
                               **console_child.child_kwargs(redirected=True))
            text, rc = (p.stdout or "") + (p.stderr or ""), p.returncode
            if rc != 0:  # report as a module-level failure
                text += f"\nFAILED {mod} - script exited {rc}"
    except subprocess.TimeoutExpired:
        return mod, 124, [], f"timed out after {TIMEOUT_S}s"
    failed = []
    for line in text.splitlines():
        m = re.match(r"(?:FAILED|ERROR) (\S+)", line)
        if m:
            failed.append(m.group(1))
    return mod, rc, failed, "\n".join(text.splitlines()[-8:])


def run_all(mods: list[str], jobs: int, tier: str, no_cache: bool = False, report: bool = False):
    """Run every module (own process each, `jobs` at a time) through the per-module result cache
    (scripts/module_test_cache.py): a module whose recorded inputs are unchanged and that fully passed last time is
    not run. Returns [(module, rc, failed ids, tail)] in module order."""
    import time
    import module_test_cache as mtc
    cache = mtc.ModuleTestCache(ROOT, tier, enabled=not no_cache)
    if cache.enabled:
        cache.tree_key()          # computed once, before anything runs: the pre-run state of the tree
    infos: dict[str, dict] = {}

    def one(mod):
        def run(env):
            env = dict(env)
            env.pop("MC2_GATE_BASE", None)  # selection belongs to this runner, not pytest's fixture repositories
            res = run_module(mod, env)
            return res[1], res[3], res
        res, hit, info = mtc.run_with_cache(cache, mod, "pytest:" + mod, run)
        if hit:
            return (mod, 0, [], "")
        infos[mod] = info
        return res

    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        results = list(ex.map(one, mods))
    cache.save()
    if report or os.environ.get("MC2_TEST_CACHE_REPORT") == "1":
        print(f"[test-cache:{tier}] {cache.stats['hit']} hit, {cache.stats['ran']} ran, "
              f"{cache.stats['uncacheable']} without a record")
        rows = sorted(((i or {}).get("secs", 0), m, i or {}) for m, i in infos.items())
        for secs, m, i in reversed(rows):
            ks = i.get("kinds") or {}
            print(f"[test-cache:{tier}] ran {secs:6.1f}s {m}: {cache.miss.get(m, '?')}")
            if i.get("tree"):
                print(f"[test-cache:{tier}] WHOLE-TREE {secs:6.1f}s {m}: {'; '.join(i.get('opaque', [])[:2])}")
            elif set(ks) - {"foreign", "harmless"}:
                print(f"[test-cache:{tier}] scoped     {secs:6.1f}s {m}: {ks}")
    return results


def default_jobs():
    try:
        return max(1, int(Path("A:/Games/mc2-ops/JOBS").read_text().strip()))
    except (OSError, ValueError):
        return min(6, os.cpu_count() or 2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("MC2_TEST_JOBS", "") or default_jobs()))
    ap.add_argument("--list", action="store_true", help="print modules and exit")
    ap.add_argument("--timeout-s", type=int, help="per-module timeout (default 1200, or MC2_TOOLS_TEST_TIMEOUT_S)")
    ap.add_argument("--suite", choices=("tools", "art", "all"), default="tools")
    ap.add_argument("--base", default=os.environ.get("MC2_GATE_BASE"), help="run only tests mapped to this committed diff; omitted = nightly/full suite")
    ap.add_argument("--no-cache", action="store_true", help="run every module (also CONTRACTS_NO_CACHE=1)")
    ap.add_argument("--report", action="store_true", help="print cache hits and the opaque (whole-tree) modules")
    args = ap.parse_args()
    if args.timeout_s:
        global TIMEOUT_S
        TIMEOUT_S = args.timeout_s

    mods = discover(args.suite)
    if args.base:
        paths = subprocess.check_output(["git", "diff", "--name-only", "-z", args.base, "HEAD"], cwd=ROOT, text=True,
                                        **console_child.child_kwargs(redirected=True)).split("\0")
        mods = scoped_modules(mods, paths)
    if args.list:
        print("\n".join(mods))
        return 0
    if not mods:
        print("tools_tests: no modules mapped to the diff; full suite nightly")
        return 0 if args.base else 1

    led_fail = {node: reason for node, reason in load_ledger().items()
                if (not args.base or node.split("::")[0] in mods)
                and (args.suite == "all" or is_art_test(node.split("::")[0]) == (args.suite == "art"))}
    bad: list[str] = []
    seen_fail: set[str] = set()
    passed = 0
    skipped = 0

    for mod, rc, failed, tail in run_all(mods, args.jobs, "tools-" + args.suite, args.no_cache, args.report):
        if rc == 0:
            if re.search(r"\b[1-9]\d* skipped\b", tail) and not re.search(r"\b[1-9]\d* passed\b", tail):
                print(f"tools_tests: SKIP in {mod}\n{tail}")
                skipped += 1
            else:
                passed += 1
            continue
        if not failed:  # timeout / crash / interrupted with no FAILED line
            bad.append(f"{mod}: pytest rc={rc} with no FAILED/ERROR lines\n{tail}")
            continue
        for node in failed:
            if node in led_fail:
                seen_fail.add(node)
            else:
                bad.append(f"{node}: NEW failure (not in the ledger)\n{tail}")

    all_mods = set(mods)
    for node in led_fail:
        if node not in seen_fail:
            mod = node.split("::")[0]
            why = ("module no longer exists" if mod not in all_mods
                   else "now passes")
            bad.append(f"ledger entry stale ({why}): FAIL {node}")

    for b in bad:
        print("VIOLATION:", b)
    if bad:
        return 1
    print(f"tools_tests: {len(mods)} modules, {passed} fully green, {skipped} skipped, "
          f"{len(seen_fail)} failing node(s) pinned in the ledger")
    if seen_fail:
        for n in sorted(seen_fail):
            print(f"KNOWN: {n} -- {led_fail[n]}")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
