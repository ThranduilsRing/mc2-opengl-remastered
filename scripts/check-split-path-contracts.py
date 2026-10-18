#!/usr/bin/env python3
"""check-split-path-contracts.py — META-FIX design C (split rendering paths).

Recurrence guard for the "two sibling render paths silently diverge" class
(issue #24 R/B swap on the untextured quad path, commit 509f8e-era; issue #34
tess/non-tess near-plane split; future GL/VK islands). Four static rule
groups, pure regex over comment-stripped / whitespace-normalized text.

RULE 1 — color convention table + family parity (#24 class). Per-PROGRAM
  convention table (the repo legitimately has three conventions). FAIL when
  (a) an FS's observed Color-read swizzle state disagrees with its table row,
  (b) a VS writes Color from computed RGBA data without .bgra, a known-write
  table entry, or a `// COLOR-CONTRACT-WAIVER:` line, or (c) members of a
  declared sibling family diverge — (c) is the check that turns the exact
  historical #24 failure (one FS of the family losing .bgra) into a red check.
  A NEW shader reading interface var `Color` MUST add a table row (unknown
  Color-reading files FAIL) — the self-registration forcing function.

RULE 2 — depth-fudge single-source + form. (a) local `const float *_DEPTH_
  (FUDGE|BIAS)` mirrors outside shaders/include/terrain_depth_bias.hglsl need
  a LOCKSTEP-MIRROR marker AND a value equal to the canonical header constant;
  (b) hglsl <-> mclib/terrain_depth_bias.h lockstep on the shared-name
  intersection (the C++-only alias WATER_DEPTH_FUDGE is outside the
  intersection by construction); (c) the banned post-divide form
  (screen.z = ... rhw ... +/- FUDGE/BIAS) never reappears IN SHADERS (the CPU
  raster path legitimately biases post-divide, terrain_depth_bias.h:9-10 —
  out of scope); (d) tess/non-tess symmetry: the identical normalized line
  `clip.z += TERRAIN_DEPTH_FUDGE * clip.w;` must exist in gos_terrain.tese,
  gos_terrain_surface.vert AND gos_terrain_thin.vert (forward-hardening; a
  future non-zero fudge moves both paths together).

RULE 3 — tess predicate chokepoint (the actual #34 recurrence guard). The raw
  tokens RendererFeature::TerrainTessellation / MC2_TERRAIN_TESS may appear
  ONLY in the allowlist below; all other code must consume
  gos_IsModernShadowPathActive() / gos_IsTerrainTessellationActive().
  (Freezes the TOKEN chokepoint; a re-derived synonym predicate evades it —
  the nightly MC2_TERRAIN_TESS=0/1 A/B leg is the behavioral guard.)

RULE 4 — pair manifest + content-hash stamps. Every declared path-pair's
  members are sha1-stamped in scripts/split-path-stamps.json at last verified
  parity; editing either side without re-stamping FAILS. Every file under
  shaders/vulkan/ must appear as a pair member or an explicit island-only
  entry with a stated reason. Re-stamp via `--stamp <pair>` (or --stamp-all)
  ONLY after re-running the pair's parity evidence.

Pair ledger (human context + parity evidence pointers):
  docs/render-backend-seams/split-path-pairs.md

Precedents: scripts/check-light-abi-lockstep.py (lockstep tripwire shape),
scripts/check-static-prop-family.py (family checker shape).

Usage:
  py -3 scripts/check-split-path-contracts.py [--quiet]
  py -3 scripts/check-split-path-contracts.py --self-test
  py -3 scripts/check-split-path-contracts.py --stamp <pair> | --stamp-all

Exit: 0 pass / 1 violation / 2 harness error (checklib convention).
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402
from checklib import (EXIT_HARNESS_ERROR, EXIT_OK, EXIT_VIOLATION, ROOT,
                      normalize_ws, strip_comments)  # noqa: E402

STAMPS_PATH = "scripts/split-path-stamps.json"

# ---------------------------------------------------------------------------
# RULE 1 tables
# ---------------------------------------------------------------------------

# FS conventions for every shader declaring interface var `in ... vec4 Color;`.
#   fs-swizzle: every active read applies exactly one .bgra
#   fs-raw:     active reads are raw (the paired VS pre-swizzles)
#   fs-dead:    declares Color but has no active read
FS_CONVENTIONS = {
    "shaders/gos_vertex.frag":            "fs-swizzle",
    "shaders/gos_tex_vertex.frag":        "fs-swizzle",
    "shaders/gos_vertex_lighted.frag":    "fs-swizzle",
    "shaders/gos_terrain.frag":           "fs-swizzle",
    "shaders/gos_terrain_water_mdi.frag": "fs-swizzle",  # two active reads, EACH one .bgra
    # VK native water FS (rung 6.5 port of the MDI water reflection block):
    # vertex light read is Color.bgra — same swizzle convention as its GL source.
    "shaders/vulkan/water_geom.frag":     "fs-swizzle",
    # decal program = terrain_overlay.vert + decal.frag (gameos_graphics.cpp
    # makeProgram): the VS pre-swizzles (Color = colorIn.bgra), FS reads raw.
    "shaders/decal.frag":                 "fs-raw",
    # terrain_overlay.frag declares Color but no active read remains.
    "shaders/terrain_overlay.frag":       "fs-dead",
    # VK ports of terrain_overlay.frag / decal.frag. The shared VK VS
    # (include/terrain_overlay_vs.glsl) pre-swizzles (Color = a_color.bgra); decal FS reads raw,
    # overlay FS declares Color but (like its GL source) has no active read.
    "shaders/vulkan/terrain_decal_tier1.frag":   "fs-raw",
    "shaders/vulkan/terrain_overlay_tier1.frag": "fs-dead",
    # gos_text.frag: active read is Foreground; the Color read is commented out.
    "shaders/gos_text.frag":              "fs-dead",
}

# Sibling families that must share ONE convention (the parity check: the
# untextured member losing .bgra while the textured member keeps it).
FS_FAMILIES = {
    "gos_quad_fs": ["shaders/gos_vertex.frag",
                    "shaders/gos_tex_vertex.frag",
                    "shaders/gos_vertex_lighted.frag"],
}

# Known computed VS writes of `Color` WITHOUT .bgra that are certified
# correct/neutral in current state: (file, normalized RHS, reason).
KNOWN_VS_WRITES = [
    ("shaders/gos_terrain_mask_solid.vert", "unpackARGB(lighting[vn].lightRGB)",
     "mask pass: glColorMask(GL_FALSE) suppresses color output"),
    ("shaders/gos_terrain_thin.vert", "unpackARGB(lrgb)",
     "FROZEN current state: raw ARGB decode feeds gos_terrain.frag's .bgra "
     "read; near-greyscale vertex light + terrainTextures2 alphaOverride "
     "(0xffffffff) make the net R/B swap sub-visible. Flagged as follow-up "
     "in split-path-pairs.md — do not certify for NEW writes."),
]

# Splat constants vec4(K) are swizzle-invariant (all lanes equal) — auto-pass.
_NEUTRAL_SPLAT_RE = re.compile(r"vec4\(\s*-?[\d.]+\s*\)")

# VS/TES/water-family members whose computed writes must ALL be pre-swizzled
# (the double-swizzle convention family).
VS_DOUBLE_SWIZZLE_FAMILY = ["shaders/gos_terrain_water_fast.vert",
                            "shaders/gos_terrain_water_fast_mdi.vert"]

# ---------------------------------------------------------------------------
# RULE 2 tables
# ---------------------------------------------------------------------------

HGLSL_HEADER = "shaders/include/terrain_depth_bias.hglsl"
CPP_HEADER = "mclib/terrain_depth_bias.h"

# Identical-normalized-line symmetry set (RULE 2d).
TESS_SYMMETRY_LINE = "clip.z += TERRAIN_DEPTH_FUDGE * clip.w;"
TESS_SYMMETRY_FILES = ["shaders/gos_terrain.tese",
                       "shaders/gos_terrain_surface.vert",
                       "shaders/gos_terrain_thin.vert"]

# Enumerated pre-divide application sites (symbol pins, never line numbers).
BIAS_APPLICATION_PINS = [
    ("shaders/gos_terrain_surface.vert",       r"clip\.z \+= TERRAIN_DEPTH_FUDGE \* clip\.w"),
    ("shaders/gos_terrain_thin.vert",          r"clip\.z \+= TERRAIN_DEPTH_FUDGE \* clip\.w"),
    ("shaders/gos_terrain.tese",               r"clip\.z \+= TERRAIN_DEPTH_FUDGE \* clip\.w"),
    ("shaders/gos_terrain_mask_water.vert",    r"clip\.z \+= WATER_DEPTH_FUDGE_RASTER \* clip\.w"),
    ("shaders/gos_terrain_water_fast.vert",    r"clip\.z \+= WATER_DEPTH_FUDGE_FAST \* clip\.w"),
    ("shaders/gos_terrain_water_fast_mdi.vert", r"clip\.z \+= WATER_DEPTH_FUDGE_FAST \* clip\.w"),
    ("shaders/mech.vert",                      r"clip\.z \+= OBJECT_DEPTH_BIAS \* clip\.w"),
    ("shaders/static_prop.vert",               r"clip\.z \+= OBJECT_DEPTH_BIAS \* clip\.w"),
    ("shaders/terrain_overlay.vert",           r"clip4\.z \+= OVERLAY_DEPTH_BIAS \* clip4\.w"),
]

# ---------------------------------------------------------------------------
# RULE 3 tables
# ---------------------------------------------------------------------------

TESS_TOKENS_RE = re.compile(
    r"RendererFeature::TerrainTessellation|MC2_TERRAIN_TESS\b")
# Entries are graded, not asserted. Each one below shelters a hit in CODE
# (comments lexed out). A file on this list is exempt whether or not the reason
# still holds, so a dead entry is a standing permission slip for a consumption
# nobody has written yet.
TESS_TOKEN_ALLOWLIST = {
    # 5 code hits: enum TerrainTessellation = 3 + the kFeatureTable row.
    "RenderCore/RendererFeatureRegistry.h",
    # 2 code hits: the material-load kill-switch read (getenv, :5184) and the
    # printf naming it. This TU also DEFINES gos_IsTerrainTessellationActive()
    # (:10124) -- it is the chokepoint every other TU is told to consume, so it
    # is the one place the raw token belongs.
    "GameOS/gameos/gameos_graphics.cpp",
    # 2 code hits: registry unit tests assert the enum/table by name.
    "tests/unit/test_rendercore.cpp",
}
TESS_SCAN_DIRS = ["code", "gui", "mclib", "GameOS", "RenderCore", "RenderWorld",
                  "GameAdapters", "editor", "netlib", "HostServices",
                  "data_tools", "tools", "tests"]

# ---------------------------------------------------------------------------
# RULE 4 tables
# ---------------------------------------------------------------------------

PAIRS = {
    "edge_fog_frag": ["shaders/edge_fog.frag", "shaders/vulkan/edge_fog.frag"],
    "fog_oob_frag":  ["shaders/fog_oob.frag", "shaders/vulkan/fog_oob.frag"],
    "water_fast_vs": ["shaders/gos_terrain_water_fast.vert",
                      "shaders/gos_terrain_water_fast_mdi.vert"],
    "shadow_terrain_paths": ["shaders/shadow_terrain.tesc",
                             "shaders/shadow_terrain.tese",
                             "shaders/shadow_terrain.vert"],
}
ISLANDS = {
    "shaders/vulkan/edge_fog.vert":   "VK fullscreen-triangle VS for edge_fog; GL pass has no dedicated .vert sibling",
    "shaders/vulkan/fog_oob.vert":    "VK fullscreen-triangle VS for fog_oob; GL pass has no dedicated .vert sibling",
    # --- Native VK tier-1 islands. One shared reason.
    "shaders/vulkan/cloud_shadow.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/cloud_shadow.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/color_prop_tier1.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/color_prop_tier1.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/hud_color.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/hud_color.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/hud_tex.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/hud_tex.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/hzb_reduce.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/hzb_reduce.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/legacy_shape_tier1.vert":
        "native VK tier-1 island (VK-PARITY-F1): the legacy MLR gos_VERTEX object loops (ground vehicles, blob shadows, alpha nodes, additive spotlight cones) had no VK owner. Structural port of shaders/gos_tex_vertex.{vert,frag} (pre-transformed screen vertices) into the shared-GBuffer co-write pipeline, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pair tests/visual/captures/parF1-units-*), not by RULE4 hash lockstep.",
    "shaders/vulkan/legacy_shape_tier1.frag":
        "native VK tier-1 island (VK-PARITY-F1): the legacy MLR gos_VERTEX object loops (ground vehicles, blob shadows, alpha nodes, additive spotlight cones) had no VK owner. Structural port of shaders/gos_tex_vertex.{vert,frag} (pre-transformed screen vertices) into the shared-GBuffer co-write pipeline, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pair tests/visual/captures/parF1-units-*), not by RULE4 hash lockstep.",
    "shaders/vulkan/hdri_sky.vert":
        "native VK tier-1 island (VK-PARITY-F3): the HDRI equirect sky had no VK owner (VK cleared color0 to a flat 0.55/0.62/0.72). Structural port of shaders/hdri_skybox.{vert,frag} (frameFix==0 matrix path) as a fullscreen reverse-Z-far triangle in the shared-GBuffer LOAD pass, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pairs tests/visual/captures/parF3-*: sky band MAD 0.0 on mc2_01), not by RULE4 hash lockstep.",
    "shaders/vulkan/hdri_sky.frag":
        "native VK tier-1 island (VK-PARITY-F3): the HDRI equirect sky had no VK owner (VK cleared color0 to a flat 0.55/0.62/0.72). Structural port of shaders/hdri_skybox.{vert,frag} (frameFix==0 matrix path) as a fullscreen reverse-Z-far triangle in the shared-GBuffer LOAD pass, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pairs tests/visual/captures/parF3-*: sky band MAD 0.0 on mc2_01), not by RULE4 hash lockstep.",
    "shaders/vulkan/mech_tier1.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/mech_tier1.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/postprocess.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/postprocess.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/postprocess_visual.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/postprocess_visual.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/screen_shadow_csm.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/screen_shadow_csm.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/screen_shadow_static.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/screen_shadow_static.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_debug_overlay.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_debug_overlay.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_prop.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_prop.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_prop_alpha.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_mech.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_mech.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shadow_terrain_static.vert":
        "native VK island (static sun shadow map, VK-PARITY-GAPS row 17): untessellated port of the GL static pre-pass (shadow_terrain.vert/tesc/tese), NOT a text-lockstep sibling. Parity is enforced by the GL-vs-VK static depth dump (MC2_VK_SHADOW_MATRIX_DIAG) and runtime capture pairs.",
    "shaders/vulkan/shoreline.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/shoreline.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/ssao.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/ssao.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/ssao_apply.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/ssao_apply.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/terrain_overlay_tier1.vert":
        "native VK tier-1 island (VK-PARITY-OVERLAY-1): the GL world-space overlay draw groups (mission-static cement/pad/runway/road overlay bake + decals) flushed into the VK-discarded GL scene FBO and were invisible under VK. Structural port of shaders/terrain_overlay.{vert,frag} / decal.frag into the shared-GBuffer co-write pipeline sharing the terrain island descriptor set, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pair tests/visual/captures/parOV-*), not by RULE4 hash lockstep.",
    "shaders/vulkan/terrain_overlay_tier1.frag":
        "native VK tier-1 island (VK-PARITY-OVERLAY-1): the GL world-space overlay draw groups (mission-static cement/pad/runway/road overlay bake + decals) flushed into the VK-discarded GL scene FBO and were invisible under VK. Structural port of shaders/terrain_overlay.{vert,frag} / decal.frag into the shared-GBuffer co-write pipeline sharing the terrain island descriptor set, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pair tests/visual/captures/parOV-*), not by RULE4 hash lockstep.",
    "shaders/vulkan/terrain_decal_tier1.vert":
        "native VK tier-1 island (VK-PARITY-OVERLAY-1): the GL world-space overlay draw groups (mission-static cement/pad/runway/road overlay bake + decals) flushed into the VK-discarded GL scene FBO and were invisible under VK. Structural port of shaders/terrain_overlay.{vert,frag} / decal.frag into the shared-GBuffer co-write pipeline sharing the terrain island descriptor set, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pair tests/visual/captures/parOV-*), not by RULE4 hash lockstep.",
    "shaders/vulkan/terrain_decal_tier1.frag":
        "native VK tier-1 island (VK-PARITY-OVERLAY-1): the GL world-space overlay draw groups (mission-static cement/pad/runway/road overlay bake + decals) flushed into the VK-discarded GL scene FBO and were invisible under VK. Structural port of shaders/terrain_overlay.{vert,frag} / decal.frag into the shared-GBuffer co-write pipeline sharing the terrain island descriptor set, NOT a text-lockstep sibling. Parity is enforced at runtime pixel level (GL/VK capture pair tests/visual/captures/parOV-*), not by RULE4 hash lockstep.",
    "shaders/vulkan/terrain_tier1.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/terrain_tier1.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/terrain_tier1_skirt.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/vfx_particle.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/vfx_particle.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/water_geom.vert":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
    "shaders/vulkan/water_geom.frag":
        "native VK tier-1 island (VULKAN-BACKEND-L6/P-ladder): structural rewrite of the GL pass (Vulkan bindings/push-constants/SPIR-V), NOT a text-lockstep sibling. Parity is enforced at runtime pixel level per RendererFeatureRegistry convention (VK arc GL/VK visual capture pairs, VFX-9 2026-07-16 + tier1 smoke), not by RULE4 hash lockstep. Declared island-only at MERGE-WAVE-2 checker reconcile 2026-07-22.",
}


# ---------------------------------------------------------------------------
# Context: disk + optional in-memory overlay (used by --self-test so no disk
# mutation is ever needed to prove the checker can fail)
# ---------------------------------------------------------------------------

class Ctx:
    def __init__(self, vfs: dict | None = None):
        self.vfs = vfs or {}

    def read(self, rel: str) -> str:
        if rel in self.vfs:
            return self.vfs[rel]
        return checklib.read_text(rel)

    def exists(self, rel: str) -> bool:
        return rel in self.vfs or os.path.isfile(os.path.join(ROOT, rel))

    def list_shader_files(self, exts=(".vert", ".frag", ".tese", ".tesc",
                                      ".comp", ".hglsl")):
        seen = set()
        for rel in checklib.iter_source_files(["shaders"], exts):
            seen.add(rel)
        for rel in self.vfs:
            if rel.startswith("shaders/") and rel.endswith(exts):
                seen.add(rel)
        return sorted(seen)

    def list_code_files(self):
        seen = set()
        for rel in checklib.iter_source_files(TESS_SCAN_DIRS,
                                              (".cpp", ".h", ".hpp")):
            seen.add(rel)
        for rel in self.vfs:
            if rel.endswith((".cpp", ".h", ".hpp")):
                seen.add(rel)
        return sorted(seen)


_COLOR_DECL_RE = re.compile(r"\bin\s+(?:\w+\s+)?vec4\s+Color\s*;")
_COLOR_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9_])Color(?![A-Za-z0-9_])")
_VS_WRITE_RE = re.compile(r"(?<![A-Za-z0-9_])Color\s*=\s*([^;]+);", re.S)


def _classify_fs(text: str) -> tuple[str, list[str]]:
    """Observed FS convention for a Color-declaring frag: fs-swizzle /
    fs-raw / fs-dead / fs-mixed. Returns (convention, read descriptions)."""
    stripped = strip_comments(text)
    reads = []
    for m in _COLOR_TOKEN_RE.finditer(stripped):
        tail = stripped[m.end():m.end() + 16]
        before = stripped[max(0, m.start() - 24):m.start()]
        # Skip the interface declaration itself.
        if re.search(r"\bin\s+(?:\w+\s+)?vec4\s*$", before):
            continue
        reads.append("swizzled" if tail.startswith(".bgra") else "raw")
    if not reads:
        return "fs-dead", reads
    if all(r == "swizzled" for r in reads):
        return "fs-swizzle", reads
    if all(r == "raw" for r in reads):
        return "fs-raw", reads
    return "fs-mixed", reads


def rule1_color(ctx: Ctx) -> list[str]:
    problems = []
    observed: dict[str, str] = {}

    for rel in ctx.list_shader_files((".frag",)):
        text = ctx.read(rel)
        # Exact raw pre-filter before the comment lexer (strip_comments is a
        # per-character scan). Stripping only ever DELETES, so
        # a declaration absent from the raw text cannot appear after stripping.
        if not _COLOR_DECL_RE.search(text):
            continue
        if not _COLOR_DECL_RE.search(strip_comments(text)):
            continue
        conv, reads = _classify_fs(text)
        observed[rel] = conv
        expected = FS_CONVENTIONS.get(rel)
        if expected is None:
            problems.append(
                "RULE1: %s reads interface var Color but has NO convention "
                "table row — add one to FS_CONVENTIONS (self-registration)" % rel)
        elif conv != expected:
            problems.append(
                "RULE1: %s observed convention %s != declared %s (reads: %s)"
                % (rel, conv, expected, reads))
    # Rows whose file no longer declares Color: stale table (warn-as-fail so
    # the table stays ground truth).
    for rel in FS_CONVENTIONS:
        if rel not in observed and ctx.exists(rel):
            problems.append(
                "RULE1: table row %s no longer declares Color — remove or "
                "update the row" % rel)

    # (c) family parity — one member drifting is the failure signature.
    for fam, members in FS_FAMILIES.items():
        convs = {m: observed.get(m, "<missing>") for m in members}
        if len(set(convs.values())) > 1:
            problems.append("RULE1: family %r diverged: %s" % (fam, convs))

    # (b) VS computed writes must be pre-swizzled / known / waivered.
    for rel in ctx.list_shader_files((".vert", ".tese")):
        raw = ctx.read(rel)
        stripped = strip_comments(raw)
        raw_lines = raw.splitlines()
        for m in _VS_WRITE_RE.finditer(stripped):
            rhs = normalize_ws(m.group(1))
            if rhs.endswith(".bgra"):
                continue                       # pre-swizzled
            if re.fullmatch(r"[A-Za-z_]\w*", rhs):
                continue                       # bare attribute passthrough
            if "tcs_Color" in rhs:
                continue                       # TES interpolation passthrough
            if _NEUTRAL_SPLAT_RE.fullmatch(rhs):
                continue                       # swizzle-neutral splat constant
            lineno = stripped[:m.start()].count("\n")  # 0-based
            line_raw = raw_lines[lineno] if lineno < len(raw_lines) else ""
            if "COLOR-CONTRACT-WAIVER:" in line_raw:
                continue
            if any(rel == f and rhs == known_rhs
                   for f, known_rhs, _ in KNOWN_VS_WRITES):
                continue
            problems.append(
                "RULE1: %s:%d computed Color write %r lacks .bgra, a "
                "KNOWN_VS_WRITES row, and a // COLOR-CONTRACT-WAIVER: line"
                % (rel, lineno + 1, rhs))

    # Double-swizzle family: both water fast VS members must keep every
    # computed write pre-swizzled (asymmetric drift = red).
    for rel in VS_DOUBLE_SWIZZLE_FAMILY:
        if not ctx.exists(rel):
            problems.append("RULE1: double-swizzle family member missing: %s" % rel)
    return problems


_LOCAL_CONST_RE = re.compile(
    r"const\s+float\s+(\w*_DEPTH_(?:FUDGE|BIAS)\w*)\s*=\s*([0-9.eE+\-]+)")
_HGLSL_CONST_RE = re.compile(
    r"const\s+float\s+(\w+)\s*=\s*([0-9.eE+\-]+)\s*;")
_CPP_CONST_RE = re.compile(
    r"constexpr\s+float\s+(\w+)\s*=\s*([0-9.eE+\-]+)f?\s*;")
_POST_DIVIDE_RE = re.compile(
    r"\w+\.z\s*=\s*[^;]*rhw[^;]*[+\-][^;]*(?:FUDGE|BIAS)", re.S)


def _parse_const_map(text: str, pattern: re.Pattern) -> dict[str, float]:
    out = {}
    for m in pattern.finditer(strip_comments(text)):
        try:
            out[m.group(1)] = float(m.group(2))
        except ValueError:
            pass  # non-literal RHS (e.g. the C++-only alias) — outside scope
    return out


def rule2_depth(ctx: Ctx) -> list[str]:
    problems = []
    hglsl_map = _parse_const_map(ctx.read(HGLSL_HEADER), _HGLSL_CONST_RE)
    cpp_map = _parse_const_map(ctx.read(CPP_HEADER), _CPP_CONST_RE)
    if not hglsl_map or not cpp_map:
        return ["RULE2: could not parse constant maps from %s / %s"
                % (HGLSL_HEADER, CPP_HEADER)]

    # (b) lockstep on the shared-name intersection. The C++-only alias
    # WATER_DEPTH_FUDGE has a non-literal RHS and is not in the hglsl, so it
    # is outside the intersection by construction (design allowlist).
    for name in sorted(set(hglsl_map) & set(cpp_map)):
        if hglsl_map[name] != cpp_map[name]:
            problems.append(
                "RULE2b: LOCKSTEP MISMATCH %s: hglsl=%r cpp=%r"
                % (name, hglsl_map[name], cpp_map[name]))

    for rel in ctx.list_shader_files():
        if rel == HGLSL_HEADER:
            continue
        raw = ctx.read(rel)
        # (a) local mirrors need the marker + the canonical value.
        for lineno, line in enumerate(raw.splitlines(), start=1):
            m = _LOCAL_CONST_RE.search(line)
            if not m:
                continue
            name, val = m.group(1), float(m.group(2))
            if "LOCKSTEP-MIRROR: terrain_depth_bias.hglsl" not in line:
                problems.append(
                    "RULE2a: %s:%d local const %s lacks the "
                    "'// LOCKSTEP-MIRROR: terrain_depth_bias.hglsl' marker"
                    % (rel, lineno, name))
            canonical = hglsl_map.get(name)
            if canonical is None:
                problems.append(
                    "RULE2a: %s:%d local const %s has no canonical sibling "
                    "in %s" % (rel, lineno, name, HGLSL_HEADER))
            elif val != canonical:
                problems.append(
                    "RULE2a: %s:%d local const %s=%r != canonical %r"
                    % (rel, lineno, name, val, canonical))
        # (c) banned post-divide form (shader scope only).
        stripped = strip_comments(raw)
        if _POST_DIVIDE_RE.search(stripped):
            problems.append(
                "RULE2c: %s applies a depth FUDGE/BIAS in the banned "
                "POST-divide form (screen.z = ... rhw ... +/- K); use the "
                "pre-divide form `x.z += K * x.w` (see "
                "gos_terrain_surface.vert:24-41 rationale)" % rel)

    # (c-positive) enumerated application sites keep the pre-divide shape.
    for rel, pat in BIAS_APPLICATION_PINS:
        norm = normalize_ws(strip_comments(ctx.read(rel)))
        if not re.search(pat, norm):
            problems.append(
                "RULE2c: %s no longer carries the pre-divide application "
                "`%s` — if the bias moved, update BIAS_APPLICATION_PINS in "
                "the same commit" % (rel, pat))

    # (d) tess/non-tess symmetry: byte-identical normalized line in all three.
    want = normalize_ws(TESS_SYMMETRY_LINE)
    for rel in TESS_SYMMETRY_FILES:
        norm = normalize_ws(strip_comments(ctx.read(rel)))
        if want not in norm:
            problems.append(
                "RULE2d: %s is missing the shared bias line `%s` — the "
                "tess/non-tess emitters must apply TERRAIN_DEPTH_FUDGE "
                "identically" % (rel, TESS_SYMMETRY_LINE))
    return problems


def rule3_predicate(ctx: Ctx) -> list[str]:
    """A game TU must CONSUME the predicate, not the raw tess token.

    Scanned over code with comments lexed out (CONTRACT-GREEN-1, 2026-07-31).
    This rule used to match the raw file text, so PROSE naming the kill-switch
    was a violation. It went red on mclib/mapdata.cpp:2445, whose only sin was
    a comment that documents the shape it copied:

        // Default-ON killswitch (MC2_TERRAIN_TESS / MC2_COLORMAP_CPU_RETIRE shape):

    A comment cannot consume a predicate, so that was never the thing the rule
    exists to stop; and the two ways of clearing it without touching the rule
    were both worse -- reword accurate documentation to dodge a grep, or
    allowlist the whole of mapdata.cpp and lose the rule over a 5000-line TU.

    NOT `checklib.strip_comments`, deliberately: that one is regex-based and a
    `/*` inside a `//` comment makes it delete 2304 lines of gameos_graphics.cpp
    including the real getenv this rule most wants to see (see its docstring).
    Stripping with it would have traded one blind spot for a far bigger one.
    strip_comments_lexed preserves newlines, so the offsets below still map to
    source line numbers.
    """
    problems = []
    for rel in ctx.list_code_files():
        if rel in TESS_TOKEN_ALLOWLIST:
            continue
        try:
            raw = ctx.read(rel)
        except OSError:
            continue
        # Pre-filter on the RAW text before paying for the lexer. Exact, not an
        # approximation: lexing only ever DELETES characters, so a token present
        # after lexing was necessarily present before. Skipping the ~99.9% of
        # files that never mention the token at all keeps this rule at its
        # pre-lexer cost -- the lexer is a per-character Python loop, and
        # applying it to every TU took the row 0.49s -> 3.44s, which matters
        # because this aggregator's own header records that the two slowest
        # rows are 60% of a 6-minute suite and that a suite too slow to run
        # before a commit is how checks go dark.
        if not TESS_TOKENS_RE.search(raw):
            continue
        text = checklib.strip_comments_lexed(raw)
        m = TESS_TOKENS_RE.search(text)
        if m:
            lineno = text[:m.start()].count("\n") + 1
            problems.append(
                "RULE3: raw tess token %r in %s:%d — consume "
                "gos_IsModernShadowPathActive() / "
                "gos_IsTerrainTessellationActive() instead (issue #34 "
                "chokepoint), or extend the allowlist with review"
                % (m.group(0), rel, lineno))
    return problems


def _sha1(ctx: Ctx, rel: str) -> str:
    if rel in ctx.vfs:
        data = ctx.vfs[rel].encode("utf-8")
    else:
        with open(os.path.join(ROOT, rel), "rb") as f:
            data = f.read()
    return hashlib.sha1(data.replace(b"\r\n", b"\n")).hexdigest()


def _load_stamps() -> dict:
    try:
        with open(os.path.join(ROOT, STAMPS_PATH), "r", encoding="utf-8") as f:
            return json.load(f)
    except OSError:
        return {}


def rule4_pairs(ctx: Ctx) -> list[str]:
    problems = []
    stamps = _load_stamps()
    manifest_files = set(ISLANDS)
    for members in PAIRS.values():
        manifest_files.update(members)

    # Every shaders/vulkan/ file must be pair member or island-only.
    for rel in ctx.list_shader_files():
        if rel.startswith("shaders/vulkan/") and rel not in manifest_files:
            problems.append(
                "RULE4: %s is not declared as a pair member or island-only "
                "entry — new VK islands need a pairing (or a stated island "
                "reason) + runtime pixel-parity per RendererFeatureRegistry "
                "convention" % rel)

    for name, members in PAIRS.items():
        st = stamps.get(name, {})
        for rel in members:
            if not ctx.exists(rel):
                problems.append("RULE4: pair %r member missing: %s" % (name, rel))
                continue
            cur = _sha1(ctx, rel)
            want = st.get(rel)
            if want is None:
                problems.append(
                    "RULE4: pair %r member %s has no stamp — run "
                    "`--stamp %s` after verifying parity" % (name, rel, name))
            elif cur != want:
                problems.append(
                    "RULE4: pair %r member %s changed since last verified "
                    "parity (sha1 %s != stamped %s) — re-run the pair's "
                    "parity evidence, then `--stamp %s`"
                    % (name, rel, cur[:12], want[:12], name))
    return problems


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def run_all(ctx: Ctx, quiet: bool) -> int:
    all_problems = []
    for label, fn in (("RULE1 color", rule1_color),
                      ("RULE2 depth", rule2_depth),
                      ("RULE3 predicate", rule3_predicate),
                      ("RULE4 pairs", rule4_pairs)):
        try:
            probs = fn(ctx)
        except OSError as e:
            print("split-path-contracts: HARNESS ERROR in %s: %s" % (label, e),
                  file=sys.stderr)
            return EXIT_HARNESS_ERROR
        if not quiet:
            print("  %-16s %s" % (label, "OK" if not probs
                                  else "%d problem(s)" % len(probs)))
        all_problems.extend(probs)
    if all_problems:
        print("split-path-contracts: FAIL", file=sys.stderr)
        for p in all_problems:
            print("  " + p, file=sys.stderr)
        return EXIT_VIOLATION
    if not quiet:
        print("split-path-contracts: PASS")
    return EXIT_OK


def do_stamp(names: list[str]) -> int:
    stamps = _load_stamps()
    ctx = Ctx()
    for name in names:
        if name not in PAIRS:
            print("unknown pair %r (known: %s)" % (name, ", ".join(PAIRS)),
                  file=sys.stderr)
            return EXIT_HARNESS_ERROR
        stamps[name] = {rel: _sha1(ctx, rel) for rel in PAIRS[name]}
        print("stamped %s (%d member(s))" % (name, len(PAIRS[name])))
    with open(os.path.join(ROOT, STAMPS_PATH), "w", encoding="utf-8",
              newline="\n") as f:
        json.dump(stamps, f, indent=2, sort_keys=True)
        f.write("\n")
    return EXIT_OK


def self_test() -> int:
    """Prove each rule can still fail (house pattern check-contracts.sh
    vulkan_bindings_selftest) — in-memory overlays, no disk mutation."""
    failures = []

    if run_all(Ctx(), quiet=True) != EXIT_OK:
        print("split-path-contracts --self-test: FAIL (not green on current "
              "tree)", file=sys.stderr)
        return EXIT_VIOLATION

    # RULE1: strip the .bgra off the untextured quad FS — the exact regression.
    t = checklib.read_text("shaders/gos_vertex.frag").replace(
        "Color.bgra", "Color")
    failures.append(("RULE1 #24 swizzle-drop",
                     run_all(Ctx({"shaders/gos_vertex.frag": t}), True)))

    # RULE2: local mirror drifts from the canonical header value.
    t = checklib.read_text("shaders/terrain_lod_chunk.vert").replace(
        "const float TERRAIN_DEPTH_FUDGE = 0.0;",
        "const float TERRAIN_DEPTH_FUDGE = 0.5;")
    failures.append(("RULE2 mirror-value drift",
                     run_all(Ctx({"shaders/terrain_lod_chunk.vert": t}), True)))

    # RULE2c: banned post-divide form reappears in a shader.
    t = checklib.read_text("shaders/gos_terrain_surface.vert") + \
        "\n// injected\nvoid bad(){ float rhw=1.0; float z; " \
        "z = 0.0; screen.z = clip.z*rhw + TERRAIN_DEPTH_FUDGE; }\n"
    failures.append(("RULE2c post-divide form",
                     run_all(Ctx({"shaders/gos_terrain_surface.vert": t}), True)))

    # RULE3: a game TU consumes the raw tess token.
    failures.append(("RULE3 raw token", run_all(Ctx({
        "code/selftest_fake.cpp":
        "#include <cstdlib>\nbool f(){return getenv(\"MC2_TERRAIN_TESS\");}\n"
    }), True)))

    # RULE3 ANTI-REGRESSION. The comment-blind fix is only
    # safe while the stripper is a real lexer. checklib.strip_comments -- the
    # obvious thing to reach for -- treats the `/*` inside a `//` comment as a
    # block-comment opener and deletes everything to the next `*/`, which on
    # gameos_graphics.cpp is 2304 lines of live code. This fixture reproduces
    # that shape exactly: prose quoting `/*`, then a REAL consumption, then an
    # inline block comment that closes the phantom. Swap the lexer back for the
    # regex stripper and this arm reports NOT DETECTED.
    failures.append(("RULE3 token behind /*-in-//", run_all(Ctx({
        "code/selftest_blindspan.cpp":
        "// prose quoting a shell one-liner: git log -S'/*' -- x\n"
        "#include <cstdlib>\n"
        "bool f(){return getenv(\"MC2_TERRAIN_TESS\");}\n"
        "int g(){ return 1 /* closes the phantom block */; }\n"
    }), True)))

    # RULE4: a pair member edited without re-stamping.
    t = checklib.read_text("shaders/edge_fog.frag") + "\n// drift\n"
    failures.append(("RULE4 stamp drift",
                     run_all(Ctx({"shaders/edge_fog.frag": t}), True)))

    # --- arms that must NOT fire -------------------------------------------
    # A rule that cannot be made quiet by legitimate code is only half a spec.
    # RULE3 grades CODE; prose naming the kill-switch is documentation, and
    # mclib/mapdata.cpp:2445 is the live example that made this row red for a
    # day. Pinned in BOTH directions so a future "just grep the raw text"
    # simplification cannot pass the suite.
    negatives = []
    negatives.append(("RULE3 comment-only mention is not a consumption",
                      run_all(Ctx({
                          "code/selftest_comment_only.cpp":
                          "// Default-ON killswitch (MC2_TERRAIN_TESS shape):\n"
                          "// unset = on. RendererFeature::TerrainTessellation\n"
                          "int f(){return 0;}\n"
                      }), True)))

    bad = [name for name, rc in failures if rc == EXIT_OK]
    bad += [name for name, rc in negatives if rc != EXIT_OK]
    for name, rc in failures:
        print("  self-test %-28s %s" % (name,
              "detected (rc=%d)" % rc if rc != EXIT_OK else "NOT DETECTED"))
    for name, rc in negatives:
        print("  self-test %-28s %s" % (name[:28],
              "correctly quiet" if rc == EXIT_OK
              else "FALSE POSITIVE (rc=%d)" % rc))
    if bad:
        print("split-path-contracts --self-test: FAIL (wrong verdict: %s)"
              % ", ".join(bad), file=sys.stderr)
        return EXIT_VIOLATION
    print("split-path-contracts --self-test: PASS (green on tree, red on all "
          "%d synthetic violations, quiet on %d legitimate shape(s))"
          % (len(failures), len(negatives)))
    return EXIT_OK


def main() -> int:
    args = sys.argv[1:]
    if "--self-test" in args:
        return self_test()
    if "--stamp-all" in args:
        return do_stamp(list(PAIRS))
    if "--stamp" in args:
        i = args.index("--stamp")
        if i + 1 >= len(args):
            print("--stamp needs a pair name", file=sys.stderr)
            return EXIT_HARNESS_ERROR
        return do_stamp([args[i + 1]])
    quiet = "--quiet" in args or "-q" in args
    return run_all(Ctx(), quiet)


if __name__ == "__main__":
    sys.exit(main())
