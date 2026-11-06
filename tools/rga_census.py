#!/usr/bin/env python3
"""Report-only shader census: Radeon GPU Analyzer (offline, gfx1100) over our SPIR-V.

    py -3 tools/rga_census.py [--spv-dir build64-ninja/shaders/vulkan] [--csv out.csv] [-j4]

Prints one row per shader: stage, VGPR, SGPR, spills, LDS, ISA bytes and an occupancy ESTIMATE (waves per SIMD, wave32,
1536 VGPR per SIMD, 8-VGPR allocation granule). Offline mode uses amdllpc, not the live driver: treat numbers as a proxy.
Never fails on a number (report only); exits 0 unless rga is missing or no SPIR-V is found (rc 2).
rga.exe is an AMD EULA binary: found via $RGA_EXE, $AMD_DEVTOOLS_ROOT, or the Radeon Developer Tool Suite in Downloads.
"""
import argparse, csv, glob, os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)

DEFAULT_ROOT = "C:/Users/Joe/Downloads/RadeonDeveloperToolSuite-2026-02-02-1757"
STAGES = {"frag": "--frag", "vert": "--vert", "comp": "--comp"}
VGPR_FILE, VGPR_GRANULE, MAX_WAVES = 1536, 8, 16


def find_rga():
    for c in (os.environ.get("RGA_EXE"), *(os.path.join(r, "rga.exe") for r in (os.environ.get("AMD_DEVTOOLS_ROOT"), DEFAULT_ROOT) if r)):
        if c and os.path.isfile(c):
            return c
    return None


def occupancy(vgprs):
    alloc = max(VGPR_GRANULE, -(-int(vgprs) // VGPR_GRANULE) * VGPR_GRANULE)
    return min(MAX_WAVES, VGPR_FILE // alloc)


def analyze(rga, spv, workdir):
    name = Path(spv).name                       # e.g. aa_fxaa.frag.spv
    stem, stage = Path(name[:-4]).stem, Path(name[:-4]).suffix.lstrip(".")
    if stage not in STAGES:
        return name, None, f"unknown stage '{stage}'"
    tag = name.replace(".", "_")
    r = subprocess.run([rga, "-s", "vk-spv-offline", "-c", "gfx1100", "-a", f"{tag}.csv", STAGES[stage], str(spv)],
                       cwd=workdir, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    out = Path(workdir) / f"gfx1100_{tag}_{stage}.csv"
    if not out.exists():
        return name, None, ((r.stdout + r.stderr).strip().splitlines() or ["no csv"])[-1][:100]
    row = next(csv.DictReader(out.open()))
    v, s = int(row["USED_VGPRs"]), int(row["USED_SGPRs"])
    return name, dict(stage=stage, vgpr=v, sgpr=s, spill=int(row["VGPR_SPILLS"]) + int(row["SGPR_SPILLS"]), lds=int(row["USED_LDS_BYTES"]),
                      isa=int(row["ISA_SIZE"]), occ=occupancy(v)), ""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--spv-dir", default="build64-ninja/shaders/vulkan")
    ap.add_argument("--csv")
    ap.add_argument("-j", type=int, default=4)
    a = ap.parse_args(argv)
    rga = find_rga()
    if not rga:
        print("rga_census: rga.exe not found (set RGA_EXE or AMD_DEVTOOLS_ROOT)")
        return 2
    spvs = sorted(glob.glob(os.path.join(a.spv_dir, "*.spv")))
    if not spvs:
        print(f"rga_census: no .spv under {a.spv_dir}")
        return 2
    with tempfile.TemporaryDirectory(prefix="rga_census_", dir=os.environ.get("TMPDIR") or None) as wd, ThreadPoolExecutor(a.j) as ex:
        res = list(ex.map(lambda s: analyze(rga, s, wd), spvs))
    ok = sorted(((n, d) for n, d, _ in res if d), key=lambda x: -x[1]["vgpr"])
    print(f"{'shader':58} {'stg':4} {'VGPR':>5} {'SGPR':>5} {'spill':>5} {'LDS':>6} {'ISA':>7} {'occ~':>4}")
    for n, d in ok:
        print(f"{n:58} {d['stage']:4} {d['vgpr']:5} {d['sgpr']:5} {d['spill']:5} {d['lds']:6} {d['isa']:7} {d['occ']:4}")
    bad = [(n, why) for n, d, why in res if not d]
    print(f"rga_census: {len(ok)} analysed, {len(bad)} no result, {sum(1 for _, d in ok if d['spill'])} with spills, max VGPR {ok[0][1]['vgpr'] if ok else 0} (report only)")
    for n, why in bad:
        print(f"  no result: {n}: {why}")
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["shader", "stage", "vgpr", "sgpr", "spill", "lds", "isa", "occupancy_est"])
            w.writerows([n, d["stage"], d["vgpr"], d["sgpr"], d["spill"], d["lds"], d["isa"], d["occ"]] for n, d in ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
