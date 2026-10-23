#!/usr/bin/env python3
"""check-cpu-gpu-cogating.py -- CPU-GPU-COGATING-1.

THE RULE
--------
When one physical quantity is computed TWICE -- once on the CPU where it feeds
gameplay/simulation, once on the GPU where it feeds what is drawn -- the two
implementations are a MIRROR PAIR, and a mirror pair must be controlled by ONE
predicate. Concretely:

  R1  ANTI-BLIND. Every site a pair declares must still be findable. If an
      anchor stops matching, this check fails RED rather than quietly passing.
  R2  CO-GATING. If both halves are live, they must be gated by the SAME env
      token (or both be ungated). One half gated and the other not is the
      defect: flipping the switch desynchronises gameplay from the picture,
      which is exactly the state a person would be in while trying to diagnose
      "objects float".
  R3  ORPHAN. If a pair declares its GPU half ABSENT, the CPU half must be
      INERT BY DEFAULT. A CPU term still feeding gameplay after its GPU
      counterpart was deleted is a silent, permanent divergence.
  R4  ABSENCE MUST BE PROVEN. An ABSENT declaration carries an in-tree anchor
      that demonstrates the absence. If the GPU half is ever reintroduced that
      anchor stops matching, the pair fails R1, and whoever reintroduced it is
      forced to re-declare the pair as live -- which re-arms R2. Absence can
      never rot into a permanent excuse.

WHY THIS EXISTS (the defect it was written from)
------------------------------------------------
mclib/mapdata.cpp's MapData::terrainElevation() carried a Phong-smoothing term
whose own header comment read "matching GPU TES". Its design doc
(docs/plans/2026-04-10-cpu-terrain-displacement-design.md, 2026-04-10) states a
single justification: the GPU TES shaders/gos_terrain.tese Phong-smooths the
drawn surface, so the CPU must match or units float above what is drawn.

On 2026-06-29, commit 75b8ebef (TERRAIN-DEADCODE-DELETE-1) deleted the legacy
tess-solid terrain draw. gos_terrain.tese has had no reachable draw since; the
ground is rasterized by shaders/terrain_lod_chunk.vert + .frag, VS+FS with no
TCS/TES, which Phong-smooths nothing. The GPU half of the mirror was gone. The
CPU half kept running for a month, moving gameplay elevation off the drawn
surface by up to ~19 world units, and NOTHING reported it -- not the registry,
not the env checks, not the split-path contracts, not the comment, which went
on asserting a pairing that no longer existed.

This is the fourth gate-disagreement defect found on 2026-07-30 (GL-vs-VK
skybox fog; a three-way disagreement on the shoreline mask; an unregistered
trace var; this). The first three were each closed with a bespoke assertion.
This one is closed with a rule, because the pattern -- "two implementations of
one quantity, and only one of them heard about the change" -- is the actual
recurring bug.

RELATIONSHIP TO NEIGHBOURING CHECKS (this is not a fifth copy of them)
---------------------------------------------------------------------
  the gate table (Gates/)          every MC2_* read is REGISTERED (a compile error otherwise).
  gate_gen --check (gate_gen_check)  a registered gate's table default equals the registry's.
  check-split-path-contracts.py    two shader paths for one draw stay in step.
  check-material-gpu-mirror.sh     one struct's C++ and GLSL field ORDER match.
None of them can see this class: here BOTH sides were internally consistent and
correctly registered. The failure was that one side had ceased to exist and the
other was never told. What is checked is the RELATIONSHIP, not either endpoint.

USAGE
  py -3 scripts/check-cpu-gpu-cogating.py [--quiet]
  py -3 scripts/check-cpu-gpu-cogating.py --selftest
Exit 0 = every declared pair is in step. Exit 1 = violation (or went blind).
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# --------------------------------------------------------------------------
# Reader-idiom classifier: what does this gate do when the var is UNSET?
# Mirrors the idiom set scripts/gate_default_classifier.py recognises.
# --------------------------------------------------------------------------
DEFAULT_ON_IDIOMS = (
    # !v || v[0] != '0'      /  !(v && v[0] == '0')
    re.compile(r"!\s*\w+\s*\|\|\s*\w+\s*\[\s*0\s*\]\s*!=\s*'0'"),
    re.compile(r"!\s*\(\s*\w+\s*&&\s*\w+\s*\[\s*0\s*\]\s*==\s*'0'"),
)


_UNSET_ON_RULES = ("Lead0Kill", "DefOnLead0", "DefOnAtoiNZ")
_TYPED_READ = r'(!\s*)?(?:\w+::)*get\s*<\s*"%s"\s*(?:,\s*(?:\w+::)*(\w+)\s*)?>'


def _typed_unset_on(root: Path, token: str, window: str) -> bool | None:
    """Does a typed read of `token` in `window` yield ON for an unset variable?

    None when the window holds no typed read. The row supplies the default; a per-site Rule override
    replaces the row's rule; a leading `!` inverts the answer."""
    m = re.search(_TYPED_READ % re.escape(token), window)
    if not m:
        return None
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import gate_table_reader
    row = gate_table_reader.gates(root)[token]
    if row["kind"] != "Bool":
        on = False
    else:
        rule = m.group(2) or row["rule"]
        if rule in _UNSET_ON_RULES:
            on = True
        elif rule in ("Std", "Lead0Def"):
            on = bool(row["default"])
        else:
            on = False
    return (not on) if m.group(1) else on


def classify_default_on(window: str, token: str = "", root: Path = ROOT) -> bool:
    """True when the reader in `window` yields ON for an unset var.

    A typed read (gate::get<"MC2_X">()) takes its default from the gate table row of `root`, the one place it is
    declared, honoring a per-site Rule override and a leading `!`; a raw getenv is classified by its idiom."""
    if token:
        typed = _typed_unset_on(root, token, window)
        if typed is not None:
            return typed
    return any(p.search(window) for p in DEFAULT_ON_IDIOMS)


# --------------------------------------------------------------------------
# THE PAIR TABLE -- authoritative. Add a row when you add a CPU mirror of a
# GPU computation (or vice versa). Anchors are deliberately short and
# distinctive so they survive reformatting but not deletion.
# --------------------------------------------------------------------------
PAIRS = [
    {
        "id": "terrain_phong",
        "why": "CPU Phong term in gameplay elevation vs GPU TES Phong on the drawn surface.",
        "cpu": {
            "file": "mclib/mapdata.cpp",
            "anchor": r"Phong smoothing \(legacy mirror of gos_terrain\.tese",
            # The CPU half must be inert unless the options.cfg key CpuPhongRetire is explicitly FALSE.
            "inert_by": {"kind": "option", "token": "CpuPhongRetire", "field": "cpuPhongRetire",
                         "reader_file": "mclib/mapdata.cpp"},
            # The guard that actually makes it inert must reference the gate.
            "guard": r"if\s*\(\s*!s_retireCpuPhong\s*&&\s*phongAlpha\s*>\s*0\.0f\s*\)",
        },
        "gpu": {
            "status": "ABSENT",
            # R4: the shader source still exists; what is gone is any reachable
            # DRAW through it. These two anchors prove that, and stop matching
            # the moment somebody re-arms the funnel.
            "absence_proof": [
                ("GameOS/gameos/gameos_graphics_draw.cpp",
                 r"TERRAIN-DEADCODE-DELETE-1: gosRenderer::terrainDrawIndexedPatches deleted"),
                # GL LOD shader setup is retired. Native terrain still has only
                # vertex/fragment stages, so the TES Phong mirror remains absent.
                ("GameOS/gameos/vulkan_terrain_island_island.cpp",
                 r"gp\.stageCount\s*=\s*2;\s*gp\.pStages\s*=\s*stages;"),
            ],
            # The orphaned counterpart, kept named so the pair stays legible.
            "orphan_of": ("shaders/gos_terrain.tese", r"Phong tessellation smoothing"),
        },
    },
    {
        "id": "terrain_dirt_displace",
        "why": "CPU dirt-displacement term in gameplay elevation vs the same GPU TES block.",
        "cpu": {
            "file": "mclib/mapdata.cpp",
            "anchor": r"Dirt texture displacement \(legacy mirror of gos_terrain\.tese",
            # Made inert structurally: the source buffers are freed,
            # buffers, and the null-guard below is what skips the block.
            "inert_by": {"kind": "env", "token": "MC2_COLORMAP_CPU_RETIRE",
                         "reader_file": "GameOS/gameos/gos_terrain_indirect_cement.cpp"},
            "guard": r"displaceScale\s*>\s*0\.0f\s*&&\s*tcm->cpuColorMap\s*&&\s*tcm->cpuDispAlpha",
        },
        "gpu": {
            "status": "ABSENT",
            "absence_proof": [
                ("GameOS/gameos/gameos_graphics_draw.cpp",
                 r"TERRAIN-DEADCODE-DELETE-1: gosRenderer::terrainDrawIndexedPatches deleted"),
            ],
            "orphan_of": ("shaders/gos_terrain.tese",
                          r"Texture-based displacement along normal"),
        },
    },
]


# --------------------------------------------------------------------------

def read(root: Path, rel: str) -> str | None:
    p = root / rel
    if not p.is_file():
        return None
    return p.read_text(encoding="utf-8", errors="replace")


def find(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text))


def check(root: Path, quiet: bool = False) -> list[str]:
    errs: list[str] = []

    def fail(pair_id: str, rule: str, msg: str):
        errs.append(f"[{pair_id}] {rule}: {msg}")

    for pair in PAIRS:
        pid = pair["id"]
        cpu = pair["cpu"]
        gpu = pair["gpu"]

        # ---- R1 ANTI-BLIND: the CPU anchor must still be findable ----------
        cpu_src = read(root, cpu["file"])
        if cpu_src is None:
            fail(pid, "R1-BLIND", f"CPU file missing: {cpu['file']}")
            continue
        if find(cpu_src, cpu["anchor"]) == 0:
            fail(pid, "R1-BLIND",
                 f"CPU anchor no longer matches in {cpu['file']} -- this check just went "
                 f"blind on the site it exists to watch. Re-point the anchor or delete "
                 f"the pair deliberately. Pattern: {cpu['anchor']}")
            continue

        if gpu.get("status") == "ABSENT":
            # ---- R4 ABSENCE MUST BE PROVEN --------------------------------
            for rel, pat in gpu["absence_proof"]:
                src = read(root, rel)
                if src is None:
                    fail(pid, "R4-ABSENCE", f"absence-proof file missing: {rel}")
                    continue
                if find(src, pat) == 0:
                    fail(pid, "R4-ABSENCE",
                         f"absence proof no longer holds in {rel} (pattern: {pat}). The GPU "
                         f"half may have been reintroduced. Re-declare this pair as LIVE so "
                         f"R2 co-gating applies again -- do not just re-point the anchor.")

            # ---- R3 ORPHAN: CPU half must be inert by default --------------
            inert = cpu.get("inert_by")
            if not inert:
                fail(pid, "R3-ORPHAN",
                     "GPU half is ABSENT but the CPU half declares no inert_by -- an "
                     "ungated CPU term with no GPU counterpart is a permanent divergence.")
                continue

            token = inert["token"]
            reader_src = read(root, inert["reader_file"])
            if reader_src is None:
                fail(pid, "R3-ORPHAN", f"gate reader file missing: {inert['reader_file']}")
                continue
            if inert.get("kind") == "option":
                # An options.cfg switch: the reader must consult the published option, and the option must default ON.
                field = inert["field"]
                if f"options::current().{field}" not in reader_src:
                    fail(pid, "R3-ORPHAN",
                         f"option {token} is not read in {inert['reader_file']} -- the CPU half "
                         f"is NOT actually gated, so it still runs unconditionally.")
                    continue
                opts_src = read(root, "Gates/render_options.h") or ""
                if not re.search(r"(?<![A-Za-z0-9_])%s\s*=\s*true(?![A-Za-z0-9_])" % re.escape(field), opts_src):
                    fail(pid, "R3-ORPHAN",
                         f"option {token} does not default ON in Gates/render_options.h; the "
                         f"orphaned CPU term would still be live by default.")
            else:
                if token not in reader_src:
                    fail(pid, "R3-ORPHAN",
                         f"gate {token} is not read in {inert['reader_file']} -- the CPU half "
                         f"is NOT actually gated, so it still runs unconditionally.")
                    continue

                # The reader's unset-case must leave the CPU term OFF.
                idx = reader_src.index(token)
                window = reader_src[max(0, idx - 400): idx + 400]
                if not classify_default_on(window, token, root):
                    fail(pid, "R3-ORPHAN",
                         f"{token} does not read as a default-ON killswitch near its reader in "
                         f"{inert['reader_file']}; the orphaned CPU term would still be live by "
                         f"default. Expected the `!v || v[0] != '0'` idiom.")

            # The guard that skips the CPU block must exist AND mention the gate.
            guard = cpu.get("guard")
            if guard:
                if find(cpu_src, guard) == 0:
                    fail(pid, "R3-ORPHAN",
                         f"the guard that makes the CPU half inert is gone from "
                         f"{cpu['file']} (pattern: {guard}) -- the term is running again.")
        else:
            # ---- R2 CO-GATING: both halves live, gates must match ----------
            gpu_src = read(root, gpu["file"])
            if gpu_src is None:
                fail(pid, "R1-BLIND", f"GPU file missing: {gpu['file']}")
                continue
            if find(gpu_src, gpu["anchor"]) == 0:
                fail(pid, "R1-BLIND",
                     f"GPU anchor no longer matches in {gpu['file']}: {gpu['anchor']}")
                continue
            cpu_gate = (cpu.get("inert_by") or {}).get("token")
            gpu_gate = (gpu.get("inert_by") or {}).get("token")
            if cpu_gate != gpu_gate:
                fail(pid, "R2-COGATING",
                     f"CPU half gated by {cpu_gate!r} but GPU half by {gpu_gate!r}. A "
                     f"killswitch whose two halves disagree desynchronises gameplay from "
                     f"the picture in one of its two states.")

    if not quiet:
        print(f"[cpu-gpu-cogating] {len(PAIRS)} mirror pair(s) checked "
              f"({sum(1 for p in PAIRS if p['gpu'].get('status') == 'ABSENT')} with an "
              f"ABSENT GPU half).")
        for e in errs:
            print(f"  FAIL {e}")
        if not errs:
            print("[cpu-gpu-cogating] OK -- every pair is in step.")
    return errs


# --------------------------------------------------------------------------
# Self-test: inject each regression class into a scratch copy of the tree and
# require a non-zero exit. A check nobody has seen fail is a check nobody
# should trust.
# --------------------------------------------------------------------------
INJECTIONS = [
    ("R4: native terrain gains a tessellation stage",
     "GameOS/gameos/vulkan_terrain_island_island.cpp",
     "gp.stageCount = 2; gp.pStages = stages;",
     "gp.stageCount = 3; gp.pStages = stages;"),
    ("R3: un-gate the CPU Phong term (the original defect)",
     "mclib/mapdata.cpp",
     "if (!s_retireCpuPhong && phongAlpha > 0.0f) {",
     "if (phongAlpha > 0.0f) {"),
    ("R3: the killswitch reader stops reading the gate",
     "mclib/mapdata.cpp",
     'mc2::options::current().cpuPhongRetire;',
     "false;    // injected: gate no longer read"),
    ("R1: anchor vanishes (check goes blind)",
     "mclib/mapdata.cpp",
     "// --- Phong smoothing (legacy mirror of gos_terrain.tese:101-108) ---",
     "// --- Phong smoothing ---"),
    ("R4: absence proof stops holding (GPU half reintroduced)",
     "GameOS/gameos/gameos_graphics_draw.cpp",
     "// TERRAIN-DEADCODE-DELETE-1: gosRenderer::terrainDrawIndexedPatches deleted.",
     "// (re-armed)"),
]


def _selftest_typed_readers() -> int:
    """classify_default_on on typed reads: the row's default, a per-site Rule override, `!`, and the root argument."""
    rc = 0
    kill = "MC2_WATER_UPLOAD_NARROW"   # Lead0Kill, unset -> on
    pres = "MC2_WINDOWED"           # Presence, unset -> off
    cases = [
        ("row default (Lead0Kill)", 'x = mc2::gate::get<"%s">();' % kill, kill, True),
        ("row default (Presence)", 'x = mc2::gate::get<"%s">();' % pres, pres, False),
        ("inverted read", 'x = !mc2::gate::get<"%s">();' % kill, kill, False),
        ("override to Presence", 'x = gate::get<"%s", gate::Rule::Presence>();' % kill, kill, False),
        ("override to Lead0Kill", 'x = gate::get<"%s", gate::Rule::Lead0Kill>();' % pres, pres, True),
        ("raw idiom still classified", "return !v || v[0] != '0';", kill, True),
    ]
    for name, window, token, want in cases:
        got = classify_default_on(window, token, ROOT)
        print("  [%s] typed reader: %s" % ("OK  " if got == want else "FAIL", name))
        rc |= got != want
    # the table comes from the root argument, not from this script's own repo
    import json
    with tempfile.TemporaryDirectory() as td:
        (Path(td) / "Gates").mkdir()
        doc = json.loads((ROOT / "Gates" / "gate_table.json").read_text(encoding="utf-8"))
        for g in doc["gates"]:
            if g["name"] == kill:
                g["rule"] = "Presence"
                g["default"] = False
        (Path(td) / "Gates" / "gate_table.json").write_text(json.dumps(doc), encoding="utf-8")
        got = classify_default_on('x = gate::get<"%s">();' % kill, kill, Path(td))
        print("  [%s] typed reader: table read from the root argument" % ("OK  " if got is False else "FAIL"))
        rc |= got is not False
    return int(rc)


def selftest() -> int:
    rc = 0
    print("[cpu-gpu-cogating] self-test: injecting real regressions into a scratch tree\n")
    baseline = check(ROOT, quiet=True)
    if baseline:
        print("  SELF-TEST ABORTED: the live tree is already failing:")
        for e in baseline:
            print(f"    {e}")
        return 1
    print("  live tree: clean (0 violations) -- baseline established\n")

    for name, rel, old, new in INJECTIONS:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td) / "tree"
            # Copy only the files the pair table touches -- fast and sufficient.
            needed = set()
            for p in PAIRS:
                needed.add(p["cpu"]["file"])
                for r, _ in p["gpu"].get("absence_proof", []):
                    needed.add(r)
                ib = p["cpu"].get("inert_by")
                if ib:
                    needed.add(ib["reader_file"])
                if p["gpu"].get("file"):
                    needed.add(p["gpu"]["file"])
            needed.add("Gates/gate_table.json")
            for r in needed:
                dst = tmp / r
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / r, dst)

            target = tmp / rel
            text = target.read_text(encoding="utf-8", errors="replace")
            if old not in text:
                print(f"  [SKIP] {name}\n         injection string not found in {rel} "
                      f"-- the self-test itself has gone stale.")
                rc = 1
                continue
            target.write_text(text.replace(old, new, 1), encoding="utf-8")

            errs = check(tmp, quiet=True)
            if errs:
                print(f"  [OK]   {name}\n         -> caught: {errs[0][:150]}")
            else:
                print(f"  [FAIL] {name}\n         -> injected regression NOT caught")
                rc = 1
    rc |= _selftest_typed_readers()
    print("\n[cpu-gpu-cogating] self-test " + ("PASSED" if rc == 0 else "FAILED"))
    return rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    return 1 if check(ROOT, quiet=a.quiet) else 0


if __name__ == "__main__":
    sys.exit(main())
