#!/usr/bin/env python3
"""V-BASELINE-0 visual baseline capture harness.

Captures PNG + JSON sidecar pairs for named camera presets defined in
`tests/visual/baselines/presets.json`. The captured frames serve as a
controlled reference set for objective before/after comparison of
StaticPropOpaque-lane visual changes.

Design (recon-driven, no engine edits):
  - Reuses the existing OS-level screenshot path from
    `scripts/smoke_with_screenshots.py` (pyautogui).
  - Mission camera is deterministic under MC2_SMOKE_MODE=1 +
    MC2_SMOKE_SEED=0xC0FFEE (RNG seeding in gos_smoke.cpp). A "camera preset"
    is therefore the tuple (mission, warmup_s) at fixed seed; no new in-engine
    camera-preset hook is introduced by this slice.
  - Sidecar JSON schema matches the V-BASELINE-0 brief:
      commit, mission, cameraPreset, resolution, flags{...}
  - Output: `tests/visual/baselines/<preset>_<commit-short>.png` + `.json`.

This script does not modify any rendering behavior. mc2.exe is launched with
the same env it normally sees during a smoke run. Capture mode is opt-in by
running this script; default mc2.exe behavior is unchanged.

Usage:
  py -3 scripts/capture_baseline.py                       # all presets
  py -3 scripts/capture_baseline.py --preset staticprop_baseline_01
  py -3 scripts/capture_baseline.py --verify              # 2x re-run, byte-diff PNG
"""
from __future__ import annotations

import argparse
import atexit
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# BACKGROUND-SMOKE-1: single chokepoint for non-activating launch + guarded focus.
from smoke_lib import nofocus  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe")
BASELINE_DIR = ROOT / "tests" / "visual" / "baselines"
PRESETS_PATH = BASELINE_DIR / "presets.json"

# Render-affecting env vars whose state we record in the sidecar. If unset,
# the engine default applies (recorded as "default").
TRACKED_FLAGS = (
    "MC2_VIEW_UNIFORMS",
    "MC2_SNAPSHOT_STATIC_PROP_BUILD",
    "MC2_MATERIAL_GPU",
    "MC2_MATERIAL_GPU_SAMPLE",
    "MC2_STATIC_PROP_LEGACY_DISPATCH",
    "MC2_GPU_CULL",
    "MC2_GPU_CULL_SUBSTRATE",
    "MC2_STATIC_PROP_AMBIENT_V1",
    "MC2_STATIC_PROP_IBL_SH",
    "MC2_STATIC_PROP_IBL_SH_STRENGTH",
    "MC2_STATIC_PROP_IBL_SH_SET",
    "MC2_STATIC_PROP_PBR_V1",
    "MC2_STATIC_PROP_PBR_V1_STRENGTH",
    "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND",
    # TERRAIN-BASELINE-0: terrain-pass tunables + debug-mode selector. Without
    # these in the sidecar a captured terrain frame is not reproducible if a
    # tuning slider or debug-mode value changes between runs.
    "MC2_TERRAIN_DEBUG_MODE",
    # TERRAIN-RESAMPLE-1: render-grid resample factor (1/2/4).
    "MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR",
    # MECH-BASELINE-0: mech.frag debug-mode selector (0=Final, 2=Albedo,
    # 3=LightingOnly, 4=Normal) + the snapshot-extract gate that populates
    # the debug-state mech inventory the sidecar embeds.
    "MC2_MECH_FRAG_DEBUG",
    "MC2_SNAPSHOT_MECH_EXTRACT",
    # WATER-BASELINE-0: MDI water FS debug-mode selector + the GPU-driven gate
    # that arms the MDI path the uniform lives on, + the fast-path gate. Without
    # these a captured water frame is not reproducible if the path/mode changes.
    "MC2_WATER_DEBUG_MODE",
    "MC2_WATER_REFLECTION",
    "MC2_WATER_REFLECTION_RT",
    "MC2_GPU_DRIVEN_WATER",
    "MC2_RENDER_WATER_FASTPATH",
    # VFX-BASELINE-0: particle debug-mode selector + the master GPU-particle
    # gate. A VFX capture is only meaningful with particles enabled; recording
    # both makes the frame's VFX state reproducible.
    "MC2_VFX_DEBUG_MODE",
    "MC2_GPU_PARTICLES",
    # VFX-ORIGINAL-RECORD-ABI-1: CPU-oracle render gate (PointCloud). Records
    # which particle render path produced the frame (placeholder vs oracle).
    "MC2_VFX_ORACLE_RENDER",
    "MC2_VFX_AGE_SAMPLE",
    # VFX-GPU-SIM-CARDCLOUD-BUFFER-1: compare-only GPU sim gates (observe-only).
    "MC2_VFX_GPU_SIM_CARDCLOUD",
    "MC2_VFX_GPU_SIM_COMPARE",
)


def park_mouse_center() -> None:
    """No-op. capture_baseline is also an MCP tool; the smoke-mode engine ignores the real pointer, and a harness must never move a person's pointer."""


def bring_mc2_foreground() -> bool:
    try:
        import ctypes
        user32 = ctypes.windll.user32
        for title in ("mc2", "MechCommander 2", "mc2 - Remastered"):
            hwnd = user32.FindWindowW(None, title)
            if hwnd:
                # BACKGROUND-SMOKE-1: no-op unless MC2_HARNESS_ALLOW_FOREGROUND=1.
                # The screenshot below uses pyautogui, which grabs the desktop
                # composite -- a non-activated window still renders into it, so
                # raising the window was never required for capture.
                if nofocus.set_foreground_window(hwnd):
                    time.sleep(0.3)
                return True
    except Exception as e:
        print(f"[baseline] WARN foreground: {e}", file=sys.stderr)
    return False


def screenshot_to(path: Path) -> tuple[bool, tuple[int, int] | None]:
    try:
        import pyautogui
        pyautogui.FAILSAFE = False
        img = pyautogui.screenshot()
        img.save(str(path))
        return True, img.size
    except Exception as e:
        print(f"[baseline] ERROR screenshot: {e}", file=sys.stderr)
        return False, None


def git_short_sha() -> str:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL,
            **nofocus.hidden_kwargs()
        ).decode("ascii").strip()
        return out
    except Exception:
        return "unknown"


def captured_flags(env: dict) -> dict:
    """Snapshot the render-affecting env vars actually seen by the subprocess.

    Reads from the supplied `env` dict (the dict passed to Popen(env=...)),
    NOT from os.environ — the parent's environment is irrelevant to what the
    captured mc2.exe process saw.
    """
    out: dict[str, str] = {}
    for k in TRACKED_FLAGS:
        v = env.get(k)
        out[k] = v if v is not None else "default"
    return out


def file_sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _strength_tag(strength: str | None) -> str:
    """Filename suffix for strength sweep. None -> 'off' (env unset)."""
    if strength is None:
        return "ibl_sh_off"
    # 0.25 -> s0p25, 1.0 -> s1p00, 1.5 -> s1p50
    try:
        f = float(strength)
    except ValueError:
        return f"ibl_sh_s{strength}"
    return f"ibl_sh_s{f:.2f}".replace(".", "p")


def read_mech_inventory(exe: Path) -> dict | None:
    """MECH-BASELINE-0: pull the engine's debug-state `mech` section.

    The engine writes debug_state/latest_render_state.json next to mc2.exe
    every 300 frames when MC2_DEBUG_STATE_DUMP=1. This is the read-only mech
    inventory produced by MECH-MATERIAL-INVENTORY-1 (rows + per-mech
    textureName/materialIdx/texHandle + mismatch counters). Returns the
    parsed `mech` object, or None if the file/section is absent. Best-effort:
    never raises (a capture must not fail because the sidecar enrichment did).
    """
    try:
        p = exe.parent / "debug_state" / "latest_render_state.json"
        if not p.exists():
            return None
        doc = json.loads(p.read_text(encoding="utf-8"))
        return doc.get("mech")
    except Exception as e:
        print(f"[baseline] WARN mech inventory read: {e}", file=sys.stderr)
        return None


def run_capture(exe: Path, preset_name: str, preset: dict,
                commit_sha: str, out_png: Path,
                strength: str | None = None,
                terrain_debug_mode: str | None = None,
                mech_debug_mode: str | None = None,
                water_debug_mode: str | None = None,
                vfx_debug_mode: str | None = None) -> dict:
    mission = preset["mission"]
    warmup = int(preset["warmup_s"])
    duration = int(preset.get("duration_s", warmup + 2))

    park_mouse_center()

    log_path = out_png.with_suffix(".log")
    log_fp = open(log_path, "w", encoding="utf-8", errors="replace")

    env = os.environ.copy()
    env["MC2_SMOKE_MODE"] = "1"
    env["MC2_SMOKE_SEED"] = "0xC0FFEE"
    env["MC2_HEARTBEAT"] = "1"
    # V-IBL-STATIC-1-SOAK: per-capture strength override drives the IBL-SH
    # gate + default slider value. strength=None means "off" baseline:
    # explicitly unset both env vars so this subprocess sees the legacy
    # gate-OFF path (byte-identical to pre-slice).
    if strength is None:
        env.pop("MC2_STATIC_PROP_IBL_SH", None)
        env.pop("MC2_STATIC_PROP_IBL_SH_STRENGTH", None)
    else:
        env["MC2_STATIC_PROP_IBL_SH"] = "1"
        env["MC2_STATIC_PROP_IBL_SH_STRENGTH"] = str(strength)

    # TERRAIN-BASELINE-0: terrain fragment debug-mode override (env var read
    # at all three GL upload sites in gameos_graphics.cpp). None = leave the
    # var unset (default mode 0 / OFF, byte-identical to legacy). "0" is also
    # valid (explicit OFF). Strings 1..9 / -1 select modes per kTerrainModes.
    if terrain_debug_mode is None:
        env.pop("MC2_TERRAIN_DEBUG_MODE", None)
    else:
        env["MC2_TERRAIN_DEBUG_MODE"] = str(terrain_debug_mode)

    # MECH-BASELINE-0: mech.frag debug-mode override + inventory enablement.
    # None = leave MC2_MECH_FRAG_DEBUG unset (default mode 0 / Final,
    # byte-identical to legacy). A value (0/2/3/4) selects a mech debug view.
    # Whenever a mech capture runs we also enable MC2_SNAPSHOT_MECH_EXTRACT
    # and MC2_DEBUG_STATE_DUMP so read_mech_inventory() can enrich the sidecar.
    # Neither alters the rendered pixels (extract is observational; the JSON
    # dump is a side file) — the captured frame is the same as without them.
    mech_capture = mech_debug_mode is not None
    if mech_debug_mode is None:
        env.pop("MC2_MECH_FRAG_DEBUG", None)
    else:
        env["MC2_MECH_FRAG_DEBUG"] = str(mech_debug_mode)
        env["MC2_SNAPSHOT_MECH_EXTRACT"] = "1"
        env["MC2_DEBUG_STATE_DUMP"] = "1"

    # WATER-BASELINE-0: MDI water FS debug-mode override. The uniform lives on
    # the MDI path (gos_terrain_water_mdi.frag), which only renders when the
    # GPU-driven water gate is armed — so a water debug capture also forces
    # MC2_GPU_DRIVEN_WATER=1. None = leave both unset (default; mode 0 is
    # byte-identical to legacy, and the active path is config-default).
    water_capture = water_debug_mode is not None
    if water_debug_mode is None:
        env.pop("MC2_WATER_DEBUG_MODE", None)
    else:
        env["MC2_WATER_DEBUG_MODE"] = str(water_debug_mode)
        env["MC2_GPU_DRIVEN_WATER"] = "1"

    # VFX-BASELINE-0: particle_billboard.frag debug-mode override
    # (MC2_VFX_DEBUG_MODE). None = leave unset (default mode 0 / Final,
    # byte-identical to legacy). Values 1..4 select Albedo/Alpha/ParticleKind/
    # Overdraw. Does not alter emission/lifetime/sorting; gated default-OFF.
    # MC2_GPU_PARTICLES is left at its default (ON) so particles actually draw.
    vfx_capture = vfx_debug_mode is not None
    if vfx_debug_mode is None:
        env.pop("MC2_VFX_DEBUG_MODE", None)
    else:
        env["MC2_VFX_DEBUG_MODE"] = str(vfx_debug_mode)

    proc_args = [str(exe), "--profile", "stock", "--mission", mission,
                 "--duration", str(duration)]
    # Launch minimized so the game does not steal foreground from the user's
    # desktop while the capture warm-up runs.
    # BACKGROUND-SMOKE-1: the STARTUPINFO (SW_SHOWMINNOACTIVE) now comes from
    # smoke_lib.nofocus, which ALSO sets MC2_NO_CURSOR_GRAB=1 -- STARTUPINFO
    # alone does not stop an SDL app from activating its window.
    print(f"[baseline] preset={preset_name} mission={mission} warmup={warmup}s")
    t0 = time.time()
    # NOFOCUS-FINISH-1: screenshot_to() above grades the DESKTOP COMPOSITE
    # (pyautogui), which sees nothing of a window parked at (-32000,-32000).
    # This is the highest-consequence instance of that hazard in the tree -- the
    # output here becomes a CHECKED-IN VISUAL BASELINE, so a wallpaper frame
    # would silently become the reference every later comparison is graded
    # against. Declaring offscreen=False makes nofocus REFUSE the launch unless
    # MC2_HARNESS_ALLOW_FOREGROUND=1. Listed in
    # check-background-launch.COMPOSITE_CAPTURE_FILES; porting this to the
    # engine readback path (MC2_VISUAL_CAPTURE, as run_visual_capture.py does)
    # retires the exception and makes the harness agent-runnable again.
    proc = nofocus.spawn(proc_args, stdout=log_fp, stderr=subprocess.STDOUT,
                         cwd=str(exe.parent), env=env, offscreen=False)

    time.sleep(max(0, warmup - 5))
    park_mouse_center()
    time.sleep(min(5, warmup))

    bring_mc2_foreground()
    park_mouse_center()
    time.sleep(0.3)
    shot_ok, size = screenshot_to(out_png)

    remaining = max(1, (duration + 10) - (time.time() - t0))
    try:
        proc.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    log_fp.close()

    if not shot_ok:
        return {"ok": False, "reason": "screenshot_failed"}

    resolution = f"{size[0]}x{size[1]}" if size else "unknown"
    sidecar = {
        "commit": commit_sha,
        "mission": mission,
        "cameraPreset": preset_name,
        "resolution": resolution,
        "flags": captured_flags(env),
        "capture": {
            "warmup_s": warmup,
            "duration_s": duration,
            "smoke_seed": env["MC2_SMOKE_SEED"],
            "captured_at": dt.datetime.now().isoformat(timespec="seconds"),
            "png_sha256": file_sha256(out_png),
            "png_bytes": out_png.stat().st_size,
        },
        "preset_description": preset.get("description", ""),
    }
    # MECH-BASELINE-0: record the selected mech debug mode and (best-effort)
    # embed the live mech inventory from the debug-state dump, so a mech
    # capture is self-describing (which texture/materialIdx was on screen).
    if mech_capture:
        sidecar["mechDebugMode"] = str(mech_debug_mode)
        inv = read_mech_inventory(exe)
        sidecar["mechInventory"] = inv if inv is not None else {
            "note": "debug-state mech section unavailable (no dump or no mechs)"
        }
    # WATER-BASELINE-0: record the selected water FS debug mode so a water
    # capture is self-describing. (Debug-state JSON has no water section yet —
    # recon gap — so there is no inventory to embed, only the mode + flags.)
    if water_capture:
        sidecar["waterDebugMode"] = str(water_debug_mode)
    # VFX-BASELINE-0: record the selected particle debug mode. VFX is transient
    # (combat-only); note that visibility at the captured frame is opportunistic.
    if vfx_capture:
        sidecar["vfxDebugMode"] = str(vfx_debug_mode)
        sidecar["vfxNote"] = ("Particles are transient combat effects; the "
                              "captured frame may or may not contain active "
                              "particles. Confirm via the .log "
                              "(GOSFX_GPU 'enabled=1 sprites=N' / TRAIL_PROBE).")
    sidecar_path = out_png.with_suffix(".json")
    sidecar_path.write_text(json.dumps(sidecar, indent=2) + "\n",
                            encoding="utf-8")
    print(f"[baseline]   PNG    {out_png}")
    print(f"[baseline]   JSON   {sidecar_path}")
    print(f"[baseline]   sha256 {sidecar['capture']['png_sha256'][:16]}...")
    return {"ok": True, "png": str(out_png), "json": str(sidecar_path),
            "sha256": sidecar["capture"]["png_sha256"]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", action="append", default=[],
                    help="Preset name to capture. Repeat for multiple. "
                         "Omit for all presets.")
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--verify", action="store_true",
                    help="Capture each preset TWICE and compare PNG sha256.")
    ap.add_argument("--out-dir", default=str(BASELINE_DIR))
    ap.add_argument("--strength", default=None,
                    help="V-IBL-STATIC-1-SOAK: single strength override "
                         "(sets MC2_STATIC_PROP_IBL_SH=1 + STRENGTH=<f>). "
                         "Omit for legacy gate-OFF baseline.")
    ap.add_argument("--strength-sweep", default=None,
                    help="V-IBL-STATIC-1-SOAK: comma-separated strengths to "
                         "sweep per preset, e.g. 'off,0.25,0.5,1.0,1.5'. "
                         "'off' = env unset (legacy gate-OFF baseline).")
    ap.add_argument("--terrain-debug-mode", default=None,
                    help="TERRAIN-BASELINE-0: terrain fragment debug-mode "
                         "override (MC2_TERRAIN_DEBUG_MODE). Modes: -1 tess-"
                         "alive probe, 0 OFF (default), 1 depth-cmp, 2 raw "
                         "colormap, 3 blurred colormap, 4 material weights, "
                         "5 normal lighting, 6 shadow, 7 cloud shadow, 8 "
                         "cement diag, 9 thin-record. Omit for default (mode "
                         "0). Filename suffix: _tdmN.")
    ap.add_argument("--mech-debug-mode", default=None,
                    help="MECH-BASELINE-0: mech.frag debug-mode override "
                         "(MC2_MECH_FRAG_DEBUG). Registry-backed modes: 0 "
                         "Final (default), 2 Albedo (texture-only), 3 "
                         "LightingOnly, 4 Normal. (mech.frag also has raw "
                         "diag modes 1/5/6/7/8/9 — not registry views.) "
                         "Also enables MC2_SNAPSHOT_MECH_EXTRACT + "
                         "MC2_DEBUG_STATE_DUMP so the sidecar embeds the mech "
                         "inventory. Omit for default. Filename suffix: _mdmN.")
    ap.add_argument("--water-debug-mode", default=None,
                    help="WATER-BASELINE-0: MDI water FS debug-mode override "
                         "(MC2_WATER_DEBUG_MODE). Modes: 1 Tint, 2 Alpha, "
                         "3 Normal, 4 Depth, 5 Shore, 6 Lighting. Also forces "
                         "MC2_GPU_DRIVEN_WATER=1 so the MDI water path (which "
                         "owns the uniform) is armed. Omit for default. "
                         "Filename suffix: _wdmN.")
    ap.add_argument("--vfx-debug-mode", default=None,
                    help="VFX-BASELINE-0: particle_billboard.frag debug-mode "
                         "override (MC2_VFX_DEBUG_MODE). Modes: 0 Final "
                         "(default), 1 Albedo, 2 Alpha, 3 ParticleKind, 4 "
                         "Overdraw. Omit for default (mode 0, byte-identical). "
                         "MC2_GPU_PARTICLES stays default-ON. VFX is transient "
                         "(combat) — particle visibility at the frame is "
                         "opportunistic. Filename suffix: _vdmN.")
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print(f"[baseline] ERROR exe missing: {exe}", file=sys.stderr)
        return 4

    if not PRESETS_PATH.exists():
        print(f"[baseline] ERROR presets.json missing: {PRESETS_PATH}",
              file=sys.stderr)
        return 4

    presets_doc = json.loads(PRESETS_PATH.read_text(encoding="utf-8"))
    all_presets = presets_doc["presets"]
    selected = args.preset or list(all_presets.keys())
    bad = [p for p in selected if p not in all_presets]
    if bad:
        print(f"[baseline] ERROR unknown preset(s): {bad}", file=sys.stderr)
        print(f"[baseline] known: {list(all_presets.keys())}", file=sys.stderr)
        return 4

    commit_sha = git_short_sha()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[baseline] commit={commit_sha} out={out_dir}")
    print(f"[baseline] presets: {selected}")

    # V-IBL-STATIC-1-SOAK: build list of strength values for the sweep.
    # 'off' / None means env unset (legacy gate-OFF baseline). A numeric
    # value sets MC2_STATIC_PROP_IBL_SH=1 + STRENGTH=<v>.
    if args.strength_sweep:
        sweep: list[str | None] = []
        for tok in args.strength_sweep.split(","):
            tok = tok.strip()
            if not tok:
                continue
            sweep.append(None if tok.lower() == "off" else tok)
    elif args.strength is not None:
        sweep = [args.strength]
    else:
        sweep = [None]

    overall_ok = True
    for name in selected:
        preset = all_presets[name]
        for strength in sweep:
            tag = _strength_tag(strength)
            # Sweep mode encodes strength in filename; legacy single-shot
            # mode (sweep == [None] and no --strength and no --terrain-debug-mode)
            # preserves the original `<name>_<sha>.png` filename for back-compat.
            # terrain-debug-mode appends a _tdmN suffix so multiple debug-mode
            # captures of the same preset coexist on disk.
            tdm = args.terrain_debug_mode
            tdm_suffix = f"_tdm{tdm}" if tdm is not None else ""
            mdm = args.mech_debug_mode
            mdm_suffix = f"_mdm{mdm}" if mdm is not None else ""
            wdm = args.water_debug_mode
            wdm_suffix = f"_wdm{wdm}" if wdm is not None else ""
            vdm = args.vfx_debug_mode
            vdm_suffix = f"_vdm{vdm}" if vdm is not None else ""
            if (args.strength_sweep is None and args.strength is None
                    and tdm is None and mdm is None and wdm is None
                    and vdm is None):
                out_png = out_dir / f"{name}_{commit_sha}.png"
            else:
                out_png = out_dir / f"{name}_{commit_sha}_{tag}{tdm_suffix}{mdm_suffix}{wdm_suffix}{vdm_suffix}.png"
            r = run_capture(exe, name, preset, commit_sha, out_png,
                            strength=strength,
                            terrain_debug_mode=tdm,
                            mech_debug_mode=mdm,
                            water_debug_mode=wdm,
                            vfx_debug_mode=vdm)
            if not r.get("ok"):
                overall_ok = False
                print(f"[baseline] FAIL preset={name} strength={tag}: {r}",
                      file=sys.stderr)
                continue
            time.sleep(2)
        # --verify only meaningful for the original single-shot path
        # (one capture per preset). Sweep mode produces N captures and is
        # not bit-comparison anyway.
        if (args.verify and args.strength_sweep is None
                and args.strength is None and args.terrain_debug_mode is None
                and args.mech_debug_mode is None
                and args.water_debug_mode is None
                and args.vfx_debug_mode is None):
            single_png = out_dir / f"{name}_{commit_sha}.png"
            sha_first = file_sha256(single_png) if single_png.exists() else ""
            verify_png = out_dir / f"{name}_{commit_sha}.verify.png"
            time.sleep(2)
            r2 = run_capture(exe, name, preset, commit_sha, verify_png)
            if not r2.get("ok"):
                overall_ok = False
                continue
            same = (sha_first == r2["sha256"])
            print(f"[baseline] VERIFY preset={name} "
                  f"identical={'yes' if same else 'NO'} "
                  f"({sha_first[:16]} vs {r2['sha256'][:16]})")
            if not same:
                print("[baseline]   (OS-screenshot path; non-byte-identical "
                      "across runs is expected — see howto doc.)")

    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
