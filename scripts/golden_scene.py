#!/usr/bin/env python3
"""golden_scene.py -- GOLDEN-SCENE-MANIFEST-1 capture-manifest layer.

Records, for ONE capture of a golden scene, the full metadata needed to make a
later OFF-vs-ON (or GL-vs-future-backend) comparison *rigorous* rather than
eyeballed. It is a thin MANIFEST LAYER over infrastructure that already exists:

  * scripts/run_smoke.py      -- launches the exe, drives one mission fly-through,
                                 passes MC2_* env through to the child.
  * MC2_DEBUG_STATE_DUMP=1     -- engine writes <exe_dir>/debug_state/
                                 latest_render_state.json (frame / mission / build
                                 / features / renderPasses / renderSnapshot /
                                 renderResources[] registry / framePassStats).
  * MC2_SCREENSHOT_AT_FRAME +  -- engine's EXISTING gated one-shot backbuffer
    MC2_SCREENSHOT_PATH           screenshot (gameosmain.cpp [SCREENSHOT v1]):
                                 reads the offscreen scene FBO at a FIXED frame
                                 and writes a TGA. Default-OFF, zero cost unset.
                                 No new engine hook is needed -- this tool reuses
                                 that path and hashes the resulting TGA.

Manifest (JSON, schema in docs/testing/golden-scene-manifest.md) fields:
    scene, mission, frame, backend, gate_set, exe_md5, registry_hash,
    render_health, pass_counters, framePassStats, pixel_hash, pixel_wh.

registry_hash = a stable FNV-1a over the sorted renderResources[] tuples
(id, kind, format, lifetime, debugName) from the dump -- i.e. the registered
RenderResourceId set + owner names/lifetimes, the same data the static
check-gpu-buffer-owners.py / check-render-resource-ids.py gates parse. Two
captures whose GPU-resource ownership graph is identical hash identically;
any add/drop/lifetime-drift changes the hash.

pixel_hash = FNV-1a over the raw TGA pixel bytes. DETERMINISM CAVEAT: the smoke
is a fly-through and is NOT frame-deterministic, so the pixel_hash is captured
at a FIXED EARLY frame (default 1) to minimize fly-through drift. It is a
best-effort visual fingerprint; pixel-EXACT stability across runs requires a
fixed camera (a follow-up slice). All the OTHER fields are deterministic. Run
the tool twice and diff the manifests: stable metadata + (usually) stable
pixel_hash at frame 1; any pixel drift is the noise-floor input for the next
slice.

Pure stdlib. Deploy v0.4 for the capture run (NOT 0.4c / 0.5.0).
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

ROOT = Path(__file__).resolve().parent.parent
RUN_SMOKE = ROOT / "scripts" / "run_smoke.py"
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")

# MC2_* env whose *effective* value we want recorded in the manifest gate_set,
# so an OFF-vs-ON diff is unambiguous about what gates were live for the capture.
# (Anything relevant to a backend/opt slice can be added; recording is cheap.)
GATE_KEYS = [
    "MC2_RENDER_BACKEND_IFACE",
    "MC2_FRAMEGRAPH_EXECUTOR",
    "MC2_FRAMEGRAPH_DRYRUN",
    "MC2_FRAMEGRAPH_REORDER_SPMECH",
    "MC2_MATERIAL_GPU",
    "MC2_STATIC_PROP_PBR_V1",
    "MC2_STATIC_PROP_IBL_SH",
    "MC2_SHADOW_CSM",
    "MC2_TERRAIN_INDIRECT",
    "MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE",
    "MC2_VEGETATION_CARDS",
]


def fnv1a(data: bytes) -> str:
    """64-bit FNV-1a, hex. Stdlib-only stable hash (md5 also fine; FNV is the
    documented pixel/registry hash algorithm)."""
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h


def md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_tga_pixels(path: Path):
    """Return (pixel_bytes, w, h) from an uncompressed true-color TGA
    (the format gos::screenshot::writeTGA emits: header[2]==2, 24bpp)."""
    raw = path.read_bytes()
    if len(raw) < 18:
        raise ValueError("TGA too short: %s" % path)
    w = raw[12] | (raw[13] << 8)
    h = raw[14] | (raw[15] << 8)
    bpp = raw[16]
    idlen = raw[0]
    off = 18 + idlen
    pixels = raw[off:off + w * h * (bpp // 8)]
    return pixels, w, h


def registry_hash(dump: dict) -> str:
    """FNV-1a over the sorted (id,kind,format,lifetime,debugName) tuples of the
    dump's renderResources[]. Mirrors the registered-id + owner-name/lifetime
    set the static owner-gates parse. Empty/absent -> hash of ''."""
    res = dump.get("renderResources", []) or []
    rows = sorted(
        "|".join(str(r.get(k, "")) for k in
                 ("id", "kind", "format", "lifetime", "debugName"))
        for r in res)
    payload = "\n".join(rows).encode("utf-8")
    return fnv1a(payload), len(rows)


def load_dump(dump_path: Path, timeout_s: float = 8.0) -> dict:
    """Read latest_render_state.json, tolerating the writer being mid-flush."""
    deadline = time.time() + timeout_s
    last_err = None
    while time.time() < deadline:
        try:
            return json.loads(dump_path.read_text(encoding="utf-8", errors="replace"))
        except Exception as e:  # partial write / not-yet-written
            last_err = e
            time.sleep(0.25)
    raise RuntimeError("could not read %s: %s" % (dump_path, last_err))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mission", default="mc2_01",
                    help="mission to capture (default mc2_01)")
    ap.add_argument("--scene", default=None,
                    help="scene label for the manifest (default = mission)")
    ap.add_argument("--frame", type=int, default=1,
                    help="fixed frame for the deterministic pixel-hash "
                         "(default 1; early frames minimize fly-through drift)")
    ap.add_argument("--duration", type=int, default=30,
                    help="smoke duration seconds (default 30, smoke cap)")
    ap.add_argument("--exe", default=str(DEFAULT_EXE),
                    help="path to deployed mc2.exe (MUST be v0.4 for a capture)")
    ap.add_argument("--out", default=None,
                    help="output manifest path (default "
                         "<exe_dir>/debug_state/golden_manifest.json)")
    ap.add_argument("--no-run", action="store_true",
                    help="skip the smoke launch; build the manifest from the "
                         "dump + TGA already present (for re-hashing a capture)")
    args = ap.parse_args()

    exe = Path(args.exe).resolve()
    if not exe.exists():
        print("[golden_scene] ERROR: exe not found: %s" % exe, file=sys.stderr)
        return 2
    exe_dir = exe.parent
    dbg_dir = exe_dir / "debug_state"
    dbg_dir.mkdir(parents=True, exist_ok=True)
    scene = args.scene or args.mission
    dump_path = dbg_dir / "latest_render_state.json"
    tga_path = dbg_dir / ("golden_%s.tga" % scene)
    out_path = Path(args.out) if args.out else (dbg_dir / "golden_manifest.json")

    # gate_set = effective value of each recorded gate for this capture (as the
    # child will see it -- os.environ is inherited by run_smoke's Popen).
    gate_set = {k: os.environ.get(k) for k in GATE_KEYS}

    if not args.no_run:
        # The engine's [SCREENSHOT v1] hook + the state dump are both keyed off
        # env vars already in run_smoke.py's Popen allowlist. Set them here so
        # the child inherits them.
        env_add = {
            "MC2_DEBUG_STATE_DUMP": "1",
            "MC2_FRAME_PASS_STATS": "1",
            "MC2_SCREENSHOT_AT_FRAME": str(args.frame),
            "MC2_SCREENSHOT_PATH": str(tga_path),
        }
        for k, v in env_add.items():
            os.environ[k] = v
        # remove any stale artifacts so we prove THIS run produced them.
        for p in (dump_path, tga_path):
            try:
                p.unlink()
            except FileNotFoundError:
                pass

        cmd = [sys.executable, str(RUN_SMOKE),
               "--mission", args.mission,
               "--duration", str(args.duration),
               "--exe", str(exe),
               "--keep-logs"]
        print("[golden_scene] launching capture: %s" % " ".join(cmd), file=sys.stderr)
        rc = subprocess.call(cmd, cwd=str(ROOT), **console_child.child_kwargs())
        print("[golden_scene] smoke exit=%d" % rc, file=sys.stderr)
        # rc!=0 is a smoke FAIL, but we still try to build the manifest from
        # whatever the engine dumped (a manifest of a failed capture is useful).

    if not dump_path.exists():
        print("[golden_scene] ERROR: no state dump at %s "
              "(MC2_DEBUG_STATE_DUMP reach the child?)" % dump_path,
              file=sys.stderr)
        return 3
    dump = load_dump(dump_path)

    reg_hash, reg_count = registry_hash(dump)

    pixel_hash = None
    pixel_wh = None
    if tga_path.exists():
        try:
            px, w, h = read_tga_pixels(tga_path)
            pixel_hash = fnv1a(px)
            pixel_wh = [w, h]
        except Exception as e:
            print("[golden_scene] WARN: could not hash TGA %s: %s"
                  % (tga_path, e), file=sys.stderr)
    else:
        print("[golden_scene] WARN: no screenshot TGA at %s "
              "(frame %d never reached, or window state?) -- pixel_hash null"
              % (tga_path, args.frame), file=sys.stderr)

    manifest = {
        "schema": "GOLDEN_SCENE_MANIFEST_V1",
        "scene": scene,
        "mission": dump.get("mission", {}).get("name", "") or args.mission,
        "frame": dump.get("frame"),
        "captured_frame_request": args.frame,
        "backend": "GL",
        "build_config": dump.get("build", {}).get("config", ""),
        "gate_set": gate_set,
        "exe": str(exe),
        "exe_md5": md5_file(exe),
        "registry_hash": reg_hash,
        "registry_resource_count": reg_count,
        "render_health": {
            "renderSnapshot": dump.get("renderSnapshot", {}),
            "renderPasses": dump.get("renderPasses", {}),
            "frame_graph": dump.get("frame_graph", {}),
        },
        "pass_counters": dump.get("framePassStats", {}),
        "pixel_hash": pixel_hash,
        "pixel_wh": pixel_wh,
        "dump_source": str(dump_path),
        "tga_source": str(tga_path) if pixel_hash else None,
        "generated_at_epoch": int(time.time()),
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print("[golden_scene] wrote %s" % out_path)
    print("[golden_scene] scene=%s frame=%s registry_hash=%s pixel_hash=%s"
          % (manifest["scene"], manifest["frame"],
             manifest["registry_hash"], manifest["pixel_hash"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
