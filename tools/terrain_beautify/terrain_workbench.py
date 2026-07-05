#!/usr/bin/env python3
"""TERRAIN-BEAUTIFY-WORKBENCH-1 / TERRAIN-WORKBENCH-REPORT-1.

Offline terrain workbench: load a stock mission + its beautify products and emit a
static report (contact-sheet PNG + cross-section profiles + HTML + a JSON of
PASS/WARN/FAIL checks) so the bake/reshape can be tuned WITHOUT launching MC2.

Loads (whatever exists):
  stock .pak heightfield + .fit water elevation
  analyzer-style masks (slope/curvature/water/shoreline/cliff/pyramid/protected)
  <mission>.beauty/height_delta.r32        (gameplay smoothing delta, optional)
  <mission>.beauty/protected.r8            (protection mask, optional)
  <mission>.beauty/visual_height_<F>x.r32  (visual heightfield, optional)

Emits to <out>/<mission>/ :
  contact_sheet.png   shaded panels (hillshade orig/visual, delta, slope, cliff,
                      pyramid, protected, water/shoreline)
  profiles.png        cross-section slices (worst pyramid H/V, largest cliff) orig vs visual
  workbench.html      contact sheet + profiles + checks table
  workbench_report.json   numeric checks (corner-pin, protected delta, waterline,
                      footprint delta, max visual delta, blockiness, pyramid score)

Run:  python terrain_workbench.py mc2_01 [--missions-dir DIR] [--beauty DIR] [--out DIR]

PIL + numpy only (no matplotlib). Exit 0 if no FAIL check, else 1.
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, derive_masks,
    detect_pyramid_islands, read_object_footprints, read_water_elevation,
    _dilate, WORLD_UNITS_PER_VERTEX, CLIFF_SLOPE_DEG,
)
from mission_terrain_analyzer import label_components  # noqa: E402
from visual_heightfield import (  # noqa: E402  TERRAIN-REAUTH-UNPIN-1 gates
    upsample_corner_pinned, detect_coarse_extrema, mountain_rock_mask, _box3,
    coarse_cliff_mask, onesided_step, _bilinear_sample,  # CLIFF-SMOOTH-1
    extract_waterline_contours, smooth_contour, _polyline_arclen,  # SHORE-CONTOUR-1
    contour_facet_stats,
)

PANEL = 220   # panel pixel size


# --- rendering helpers -------------------------------------------------------
def _resize(rgb: np.ndarray, n: int = PANEL) -> Image.Image:
    return Image.fromarray(rgb, "RGB").resize((n, n), Image.NEAREST)


def hillshade(elev: np.ndarray) -> np.ndarray:
    gy, gx = np.gradient(elev)
    nx, ny, nz = -gx, -gy, np.full_like(elev, WORLD_UNITS_PER_VERTEX)
    L = np.sqrt(nx * nx + ny * ny + nz * nz) + 1e-9
    light = np.array([-0.5, -0.5, 0.7]); light /= np.linalg.norm(light)
    shade = np.clip((nx / L) * light[0] + (ny / L) * light[1] + (nz / L) * light[2], 0.05, 1.0)
    e = elev - elev.min(); e = e / (e.max() + 1e-9)
    base = 0.3 + 0.7 * e
    v = np.clip(base * shade * 1.5, 0, 1)
    g = (v * 255).astype(np.uint8)
    return np.stack([g, g, g], axis=-1)


def colorize(mask: np.ndarray, color, over=None) -> np.ndarray:
    h, w = mask.shape
    rgb = np.zeros((h, w, 3), np.uint8) if over is None else over.copy()
    rgb[mask] = color
    return rgb


def signed_delta_rgb(d: np.ndarray) -> np.ndarray:
    m = np.abs(d).max() + 1e-9
    t = np.clip(d / m, -1, 1)
    r = np.where(t > 0, (t * 255), 0).astype(np.uint8)
    b = np.where(t < 0, (-t * 255), 0).astype(np.uint8)
    g = np.zeros_like(r)
    return np.stack([r, g, b], axis=-1)


def label_panel(img: Image.Image, text: str) -> Image.Image:
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width, 12], fill=(0, 0, 0))
    d.text((2, 1), text, fill=(255, 255, 0))
    return img


def shore_contour_overlay(surface: np.ndarray, water_elev: float,
                          smooth_radius_cells: float, factor: int,
                          n: int = PANEL, min_len_cells: float = 3.0) -> tuple:
    """SHORE-CONTOUR-1 deliverable panel (the user's blue-line comparison):
    the BLOCKY extracted waterline (marching squares at waterElev, RED) vs the
    arc-length Gaussian SMOOTHED waterline (BLUE). Both are drawn as polylines
    over the surface hillshade, resampled to panel resolution. Returns
    (PIL.Image, before_stats, after_stats). Deterministic; PIL only."""
    V = surface.shape[0]
    contours = extract_waterline_contours(surface, float(water_elev))
    min_len = min_len_cells * factor
    keep = [c for c in contours
            if _polyline_arclen(c["pts"], c["closed"]) >= min_len]
    cell_wu = WORLD_UNITS_PER_VERTEX / factor
    before = contour_facet_stats(keep, cell_wu) if keep else None
    sigma = smooth_radius_cells * factor
    smoothed = [smooth_contour(c["pts"], c["closed"], sigma, 0.5) for c in keep]
    after = (contour_facet_stats(
        [{"pts": s, "closed": c["closed"]} for s, c in zip(smoothed, keep)],
        cell_wu) if keep else None)

    img = Image.fromarray(hillshade(surface), "RGB").resize(
        (n, n), Image.NEAREST)
    d = ImageDraw.Draw(img)
    scale = (n - 1) / max(1, V - 1)

    def _draw(polys, closed_list, color, width=1):
        for pts, closed in zip(polys, closed_list):
            xy = [(float(c) * scale, float(r) * scale) for r, c in pts]
            if len(xy) < 2:
                continue
            if closed:
                xy.append(xy[0])
            d.line(xy, fill=color, width=width)

    closed = [c["closed"] for c in keep]
    _draw([c["pts"] for c in keep], closed, (235, 70, 70), 1)   # blocky (red)
    _draw(smoothed, closed, (80, 150, 255), 2)                  # smoothed (blue)
    return img, before, after


def profile_plot(width, height, title, x, series) -> Image.Image:
    """series: list of (label, yvals, color)."""
    img = Image.new("RGB", (width, height), (16, 16, 20))
    d = ImageDraw.Draw(img)
    allv = np.concatenate([s[1] for s in series])
    lo, hi = float(allv.min()), float(allv.max())
    rng = (hi - lo) or 1.0
    pad = 18
    def px(i): return pad + (i / max(1, len(x) - 1)) * (width - 2 * pad)
    def py(v): return height - pad - ((v - lo) / rng) * (height - 2 * pad)
    d.rectangle([pad, pad, width - pad, height - pad], outline=(80, 80, 80))
    for (lab, yv, col) in series:
        pts = [(px(i), py(yv[i])) for i in range(len(yv))]
        d.line(pts, fill=col, width=1)
    d.text((pad, 2), title, fill=(220, 220, 220))
    # legend
    for k, (lab, _, col) in enumerate(series):
        d.text((pad + 4, pad + 2 + 11 * k), lab, fill=col)
    return img


def compose_grid(panels, cols, gap=4, bg=(28, 28, 32)) -> Image.Image:
    if not panels:
        return Image.new("RGB", (PANEL, PANEL), bg)
    rows = (len(panels) + cols - 1) // cols
    W = cols * PANEL + (cols + 1) * gap
    H = rows * PANEL + (rows + 1) * gap
    sheet = Image.new("RGB", (W, H), bg)
    for i, p in enumerate(panels):
        r, c = divmod(i, cols)
        sheet.paste(p, (gap + c * (PANEL + gap), gap + r * (PANEL + gap)))
    return sheet


# --- main analysis -----------------------------------------------------------
def coarse_from_visual(visual: np.ndarray, side: int, factor: int) -> np.ndarray:
    """Sample the visual grid at coarse-vertex positions -> (side,side)."""
    return visual[::factor, ::factor][:side, :side]


def nearest_up(elev, factor):
    N = elev.shape[0]; V = (N - 1) * factor + 1
    idx = np.clip((np.arange(V) + factor // 2) // factor, 0, N - 1)
    return elev[np.ix_(idx, idx)]


def blockiness(elev):
    sr = np.zeros_like(elev); sc = np.zeros_like(elev)
    sr[1:-1, :] = elev[2:, :] - 2 * elev[1:-1, :] + elev[:-2, :]
    sc[:, 1:-1] = elev[:, 2:] - 2 * elev[:, 1:-1] + elev[:, :-2]
    return float((sr * sr + sc * sc).mean())


# --- CLIFF-SMOOTH-1: reference-cliff cross-section ----------------------------
def cliff_reference_section(elev, water, protect=None,
                            cliff_slope_deg=30.0, half_len=10.0):
    """Slope census -> reference cliff cross-section.

    Largest connected component of the DILATED one-sided cliff mask = the
    mission's reference cliff feature (mc2_17's big water-facing bluff class;
    dilation matters — on a terraced staircase every riser is a separate
    undilated component, the ring joins the bluff into one). Inside it, the
    section is anchored at the STEEPEST riser and runs along the LOCAL (9x9)
    downhill direction with a fixed +/- half_len coarse-cell span: a section
    through a known cliff FACE, not a transect of the whole mountain range
    (mc2_24's range is one ~3000-cell component — its centroid/mean-gradient
    line would measure valley terrain, not the staircase).

    `protect` (roads/footprints, structural pin): protected cells are pinned
    to the bilinear baseline BY CONTRACT and can never melt — a building-
    pedestal mesa edge is not a fair reference cliff. Components are ranked
    and anchored by their UNPROTECTED cells only."""
    mask = coarse_cliff_mask(elev, water_coarse=water,
                             cliff_slope_deg=cliff_slope_deg, dilate_iters=1)
    if not mask.any():
        return None
    meltable = mask if protect is None else (mask & ~protect)
    if not meltable.any():
        return None
    labels, count = label_components(mask)
    best, best_n = 0, 0
    for lid in range(1, count + 1):
        n = int(((labels == lid) & meltable).sum())
        if n > best_n:
            best, best_n = lid, n
    comp = labels == best
    step = np.where(comp & meltable, onesided_step(elev), -1.0)
    ar, ac = np.unravel_index(int(np.argmax(step)), step.shape)
    n = elev.shape[0]
    r0, r1 = max(0, ar - 4), min(n, ar + 5)
    c0, c1 = max(0, ac - 4), min(n, ac + 5)
    gy, gx = np.gradient(elev)
    vy, vx = float(gy[r0:r1, c0:c1].mean()), float(gx[r0:r1, c0:c1].mean())
    nrm = float(np.hypot(vy, vx))
    vy, vx = (1.0, 0.0) if nrm < 1e-9 else (vy / nrm, vx / nrm)
    return {"component_cells": best_n, "total_cliff_cells": int(mask.sum()),
            "center_rc": (float(ar), float(ac)), "dir_rc": (vy, vx),
            "half_len": float(half_len),
            "anchor_step_wu": float(step[ar, ac]),
            "water_facing": bool((_dilate(comp) & water).any())}


def profile_step_metrics(prof, ds_wu, tread_deg=12.0):
    """Terrace metrics along a cliff profile: tread count (near-flat runs >= 3
    samples), step heights between consecutive treads, and second-difference
    energy (staircase concentrates curvature at step edges; a melted face
    spreads and shrinks it)."""
    g = np.gradient(prof) / ds_wu
    deg = np.degrees(np.arctan(np.abs(g)))
    is_tread = deg < tread_deg
    treads = []
    i, n = 0, len(prof)
    while i < n:
        if is_tread[i]:
            j = i
            while j + 1 < n and is_tread[j + 1]:
                j += 1
            if j - i + 1 >= 3:
                treads.append(float(prof[i:j + 1].mean()))
            i = j + 1
        else:
            i += 1
    steps = [abs(treads[k + 1] - treads[k]) for k in range(len(treads) - 1)]
    s = prof[2:] - 2 * prof[1:-1] + prof[:-2]
    return {"tread_count": len(treads),
            "max_step_wu": (float(max(steps)) if steps else 0.0),
            "mean_step_wu": (float(np.mean(steps)) if steps else 0.0),
            "second_diff_energy": float((s * s).mean())}


def run(mission: str, missions_dir: Path, beauty_root: Path, out_root: Path) -> dict:
    pak = missions_dir / f"{mission}.pak"
    if not pak.is_file():
        return {"mission": mission, "error": f"not found: {pak}"}
    _, side, blocks = locate_mapdata(read_packets(pak))
    water_elev = read_water_elevation(missions_dir / f"{mission}.fit")
    layers = extract_layers(side, blocks, water_elev)
    elev = layers["elev"]
    masks = derive_masks(elev, layers["water"], layers["overlay"])
    pyramids, pyr_mask = detect_pyramid_islands(elev, masks["land"], layers["water"])
    foot, _objs, _ = read_object_footprints(read_packets(pak), side)
    protect_struct = _dilate(layers["overlay"] | foot)

    bdir = beauty_root / f"{mission}.beauty"
    checks = []

    def chk(name, status, detail):
        checks.append({"check": name, "status": status, "detail": detail})

    # --- gameplay smoothing delta (height_delta.r32), if present ---
    gdelta = None
    f = bdir / "height_delta.r32"
    if f.is_file():
        gdelta = np.fromfile(f, "<f4").reshape(side, side).astype(np.float64)
        pmax = float(np.abs(gdelta[protect_struct]).max()) if protect_struct.any() else 0.0
        chk("protected_struct_delta", "PASS" if pmax == 0 else "FAIL",
            f"max|Δ| on roads/buildings = {pmax:.3f}wu (want 0)")
        wmax = float(np.abs(gdelta[layers["water"]]).max()) if layers["water"].any() else 0.0
        chk("waterline_delta", "PASS" if wmax <= 0.0 else "WARN",
            f"max|Δ| on water = {wmax:.3f}wu")
        fmax = float(np.abs(gdelta[foot]).max()) if foot.any() else 0.0
        chk("building_footprint_delta", "PASS" if fmax == 0 else "FAIL",
            f"max|Δ| on footprints = {fmax:.3f}wu (want 0)")

    # --- visual heightfield (visual_height_<F>x.r32), if present ---
    visual = None; factor = None
    cliff_prof_img = None; cliff_metrics = None    # CLIFF-SMOOTH-1
    rep = bdir / "visual_height_report.json"
    if rep.is_file():
        vmeta = json.loads(rep.read_text())
        factor = vmeta.get("factor", 4)
        vf = bdir / vmeta.get("visual_height_file", f"visual_height_{factor}x.r32")
        if vf.is_file():
            V = vmeta["visual_side"]
            visual = np.fromfile(vf, "<f4").reshape(V, V).astype(np.float64)
            cs = coarse_from_visual(visual, side, factor)
            cerr = float(np.abs(cs - elev).max())
            reauth = vmeta.get("reauth")
            if reauth:
                # TERRAIN-REAUTH-UNPIN-1: corners are intentionally UNpinned;
                # the guarantee flips to landform fidelity. All gates below are
                # RECOMPUTED from the .r32 (report values not trusted).
                # CLIFF-SMOOTH-1 recalibration (documented): on the cliff mask
                # the drift ceiling is cliff_drift, terrace-step (non-regional)
                # extrema are exempt, and shape fidelity gates OFF-cliff; the
                # on-cliff relaxation is the slice working as designed.
                cliff_meta = reauth.get("cliff") or {}
                cliff_on = float(cliff_meta.get("cliff_drift_wu", 0.0)) > 0.0 \
                    and int(cliff_meta.get("coarse_cliff_cells", 0)) > 0
                cliff_deg = float(cliff_meta.get("cliff_slope_deg", 30.0))
                cliff_coarse = None
                if cliff_on:
                    cliff_coarse = coarse_cliff_mask(
                        elev, water_coarse=layers["water"],
                        cliff_slope_deg=cliff_deg)  # dilate=1, same as bake
                md = float(reauth.get("effective_max_drift_wu",
                                      reauth.get("max_drift_wu", 24.0)))
                chk("corner_unpinned",
                    "PASS" if (0.05 < cerr <= md * 1.25) else "FAIL",
                    f"max corner move = {cerr:.2f}wu (want >0.05 and <= {md * 1.25:.1f} "
                    f"= 1.25*effective_max_drift"
                    + (f"; cliff mask allows {cliff_meta['cliff_drift_wu']:.0f}wu"
                       if cliff_on else ""))
                corr = float(np.corrcoef(elev.ravel(), cs.ravel())[0, 1])
                if cliff_on:
                    off = ~cliff_coarse
                    corr_off = float(np.corrcoef(elev[off].ravel(),
                                                 cs[off].ravel())[0, 1])
                    chk("shape_fidelity_corr",
                        "PASS" if corr_off >= 0.99 else "FAIL",
                        f"off-cliff landform correlation = {corr_off:.4f} "
                        f"(gate, cliff mask relaxed per CLIFF-SMOOTH-1; "
                        f"global incl. melted cliffs = {corr:.4f})")
                else:
                    chk("shape_fidelity_corr", "PASS" if corr >= 0.99 else "FAIL",
                        f"coarse landform correlation before/after = {corr:.4f} (want >= 0.99)")
                tol_frac = float(reauth.get("shape_tolerance", 0.10))
                # SHORE-CONTOUR-1: a non-regional extremum inside the shore band
                # is a bank facet edge the reshape intentionally moves; the bake
                # relaxes it (shore_w > 0.05 at the guard point), so the
                # workbench gate must too or it flags the slice's own deliverable
                # as a violation. The band weight is recomputed deterministically
                # from the bilinear baseline (same input the bake reshape sees).
                shore_wc = None
                shore_wf = None                         # fine-grid band weight
                shore_meta = reauth.get("shore")
                if shore_meta and layers["water"].any():
                    from visual_heightfield import shore_contour_reshape
                    _bl = upsample_corner_pinned(elev, factor)
                    _out, _w, _si = shore_contour_reshape(
                        _bl, factor, float(shore_meta.get("water_elev", 0.0)),
                        smooth_radius_cells=float(
                            shore_meta.get("smooth_radius_cells", 3.5)),
                        band_cells=float(shore_meta.get("band_cells", 1.5)),
                        max_adjust=float(shore_meta.get("max_adjust_wu", 64.0)))
                    if _w is not None:
                        shore_wf = _w
                        shore_wc = _w[::factor, ::factor]   # coarse-sampled band
                extrema = detect_coarse_extrema(elev)
                viol, worst, relaxed, enforced, relaxed_shore = 0, 0.0, 0, 0, 0
                for e in extrema:
                    if (cliff_coarse is not None and cliff_coarse[e["r"], e["c"]]
                            and not e["is_regional"]):
                        relaxed += 1     # terrace-step extremum on the cliff mask
                        continue
                    if (shore_wc is not None and not e["is_regional"]
                            and shore_wc[e["r"], e["c"]] > 0.05):
                        relaxed_shore += 1   # bank facet edge on the shore band
                        continue
                    enforced += 1
                    tol = max(0.5, tol_frac * e["relief"])
                    mv = abs(float(visual[e["r"] * factor, e["c"] * factor]) - e["h0"])
                    worst = max(worst, mv / max(1e-9, e["relief"]))
                    if mv > tol + 1e-6:
                        viol += 1
                chk("extrema_preserved", "PASS" if viol == 0 else "FAIL",
                    f"{viol}/{enforced} enforced peaks/pits moved past "
                    f"{100 * tol_frac:.0f}% of local relief (worst {100 * worst:.1f}%)"
                    + (f"; {relaxed} terrace extrema relaxed on the cliff mask "
                       f"(CLIFF-SMOOTH-1, true peaks stay pinned)"
                       if relaxed else "")
                    + (f"; {relaxed_shore} bank-edge extrema relaxed on the shore "
                       f"band (SHORE-CONTOUR-1, regional peaks stay pinned)"
                       if relaxed_shore else ""))
                # facet-flattening proof (pyramid edges become curves): the
                # SMOOTH-stage crease energy from the bake report must beat the
                # bilinear baseline (final surface may legitimately add ridge
                # curvature under --mountainify).
                ce = reauth.get("facet_crease_energy") or {}
                cb, csm = ce.get("bilinear_base"), ce.get("smoothed")
                if cb is not None and csm is not None:
                    chk("facet_crease_reduced", "PASS" if csm < cb * 0.85 else "WARN",
                        f"smooth-stage crease energy {csm:.2f} vs bilinear {cb:.2f} "
                        f"({100 * (1 - csm / (cb + 1e-9)):.0f}% lower, want >=15%)")
                if reauth.get("mountainify"):
                    # feature-ADDING proof: recompute the smooth-only surface
                    # with the SAME (deterministic) pipeline parameters and
                    # measure the detail actually added on the rock channel.
                    # (A bilinear high-freq comparison is invalid: coarse-cell
                    # creases are themselves high-frequency, so the smoothing
                    # removes about as much energy as the ridges add.)
                    from visual_heightfield import reauth_visual
                    # SHORE-CONTOUR-1: the smooth-only reference must run the
                    # SAME shore reshape (mountainify off) so d = visual - plain
                    # isolates ONLY the added ridged detail. Without it the
                    # reference lacks the up-to-max_adjust bank displacement and
                    # that leaks into rms_calm (water-adjacent, non-rock cells),
                    # spuriously failing the >=2x-calm gate.
                    _sm = reauth.get("shore") or {}
                    plain, _ = reauth_visual(
                        elev, factor, protect_struct,
                        shape_tolerance=tol_frac,
                        max_drift=float(reauth.get("max_drift_wu", 24.0)),
                        passes=int(reauth.get("passes", 150)),
                        water_coarse=layers["water"],
                        # CLIFF-SMOOTH-1: the smooth-only reference must melt
                        # the same cliffs, so the diff isolates ADDED detail.
                        cliff_drift=float(cliff_meta.get("cliff_drift_wu", 0.0)),
                        cliff_slope_deg=cliff_deg,
                        cliff_melt_passes=int(cliff_meta.get("melt_passes", 300)),
                        water_elev=(float(_sm["water_elev"])
                                    if _sm and layers["water"].any() else None),
                        shore_smooth_radius=float(_sm.get("smooth_radius_cells", 0.0))
                        if _sm else 0.0,
                        shore_band_cells=float(_sm.get("band_cells", 1.5)),
                        shore_max_adjust=float(_sm.get("max_adjust_wu", 64.0)))
                    rockw = mountain_rock_mask(plain, elev, factor,
                                               water_coarse=layers["water"])
                    if shore_wf is not None:
                        # SHORE-CONTOUR-1: the bake excludes ridged detail in the
                        # shore band (rock *= 1 - shore_w), so the detected-detail
                        # census must exclude it too, or the reference `plain`
                        # (which has NO shore reshape) shows spurious diff there
                        # and drags rms_rock below the gate. Match the bake mask.
                        rockw = rockw * (1.0 - shore_wf)
                    rock = rockw > 0.35
                    calm = rockw < 0.05
                    if rock.any():
                        d = visual - plain
                        rms_rock = float(np.sqrt((d[rock] ** 2).mean()))
                        rms_calm = (float(np.sqrt((d[calm] ** 2).mean()))
                                    if calm.any() else 0.0)
                        ok = rms_rock >= 0.8 and rms_rock >= 2.0 * max(0.05, rms_calm)
                        chk("mountain_detail_present", "PASS" if ok else "FAIL",
                            f"added detail RMS on rock {rms_rock:.2f}wu "
                            f"(calm {rms_calm:.2f}wu; want >=0.8wu and >=2x calm)")
                    else:
                        chk("mountain_detail_present", "WARN", "no rock-channel cells found")
                # CLIFF-SMOOTH-1: reference-cliff cross-section (slope census
                # picks the largest cliff component — mc2_17's big water-facing
                # bluff class). The user-visible deliverable: terraced steps
                # must melt into a continuous face.
                sec = cliff_reference_section(elev, layers["water"],
                                              protect=protect_struct,
                                              cliff_slope_deg=cliff_deg)
                if sec is not None and sec["component_cells"] >= 8:
                    bf = upsample_corner_pinned(elev, factor)
                    cy, cx = sec["center_rc"]
                    vy, vx = sec["dir_rc"]
                    half = sec["half_len"]
                    npts = int(2 * half * factor) + 1
                    ts = np.linspace(-half, half, npts)
                    rr = np.clip(cy + ts * vy, 0, side - 1)
                    cc = np.clip(cx + ts * vx, 0, side - 1)
                    prof_b = _bilinear_sample(bf, rr * factor, cc * factor)
                    prof_a = _bilinear_sample(visual, rr * factor, cc * factor)
                    ds_wu = (ts[1] - ts[0]) * WORLD_UNITS_PER_VERTEX
                    # metrics on the face span only (where the BEFORE profile
                    # is steep) so the flat approach on both ends doesn't
                    # dilute the staircase signal.
                    gdeg = np.degrees(np.arctan(np.abs(np.gradient(prof_b)) / ds_wu))
                    steep = np.where(gdeg > 25.0)[0]
                    if steep.size:
                        s0 = max(0, int(steep[0]) - factor)
                        s1 = min(npts, int(steep[-1]) + factor + 1)
                        mb = profile_step_metrics(prof_b[s0:s1], ds_wu)
                        ma = profile_step_metrics(prof_a[s0:s1], ds_wu)
                        crease_cut = 1.0 - ma["second_diff_energy"] / (
                            mb["second_diff_energy"] + 1e-9)
                        tread_ok = (ma["tread_count"] <= max(1, mb["tread_count"] // 2)
                                    or ma["mean_step_wu"] <= 0.5 * mb["mean_step_wu"] + 1e-9)
                        detail = (
                            f"ref cliff {sec['component_cells']} cells"
                            f"{' water-facing' if sec['water_facing'] else ''}: "
                            f"treads {mb['tread_count']}->{ma['tread_count']}, "
                            f"step mean {mb['mean_step_wu']:.0f}->{ma['mean_step_wu']:.0f}wu"
                            f" max {mb['max_step_wu']:.0f}->{ma['max_step_wu']:.0f}wu, "
                            f"profile curvature energy -{100 * crease_cut:.0f}%")
                        if not cliff_on:
                            chk("cliff_steps_melted", "WARN",
                                detail + " (bake has CLIFF-SMOOTH-1 disabled)")
                        elif crease_cut >= 0.40 or tread_ok:
                            chk("cliff_steps_melted", "PASS", detail)
                        elif crease_cut >= 0.15:
                            chk("cliff_steps_melted", "WARN", detail)
                        else:
                            chk("cliff_steps_melted", "FAIL", detail)
                        cliff_prof_img = profile_plot(
                            760, 300,
                            f"reference cliff x-section ({sec['component_cells']} cells"
                            f"{', water-facing' if sec['water_facing'] else ''}) "
                            f"treads {mb['tread_count']}->{ma['tread_count']} "
                            f"step max {mb['max_step_wu']:.0f}->{ma['max_step_wu']:.0f}wu",
                            np.arange(npts),
                            [("before (bilinear bake)", prof_b, (110, 110, 120)),
                             ("after (CLIFF-SMOOTH-1)", prof_a, (255, 150, 80))])
                        cliff_metrics = {
                            "component_cells": sec["component_cells"],
                            "total_cliff_cells": sec["total_cliff_cells"],
                            "water_facing": sec["water_facing"],
                            "center_rc": [round(cy, 1), round(cx, 1)],
                            "anchor_step_wu": sec["anchor_step_wu"],
                            "before": mb, "after": ma,
                            "curvature_energy_cut_frac": crease_cut}
            else:
                # Reshaped bakes intentionally move corners up to corner_clamp;
                # bilinear bakes must be exact. Read the bound from the report.
                clamp = (vmeta.get("reshape") or {}).get("corner_clamp_wu", 0.0)
                chk("corner_pin", "PASS" if cerr <= clamp + 1e-4 else "FAIL",
                    f"max corner move = {cerr:.3g}wu (clamp {clamp:.1f})")
            # blockiness: compare visual (sampled at coarse) vs original — should be == (bilinear
            # preserves coarse), and visual-fine should be smoother than nearest-up.
            b_near = blockiness(nearest_up(elev, factor))
            b_vis = blockiness(visual)
            chk("blockiness_reduced", "PASS" if b_vis < b_near else "WARN",
                f"visual 2nd-diff var {b_vis:.2f} vs blocky {b_near:.2f} "
                f"({100*(1-b_vis/(b_near+1e-9)):.0f}% lower)")

    # --- contact sheet panels ---
    panels = []
    panels.append(label_panel(_resize(hillshade(elev)), "orig height (hillshade)"))
    if visual is not None:
        panels.append(label_panel(_resize(hillshade(visual)), f"visual height x{factor}"))
        d = visual - nearest_up(elev, factor)
        panels.append(label_panel(_resize(signed_delta_rgb(d)), "visual delta (r=up,b=down)"))
    if gdelta is not None:
        panels.append(label_panel(_resize(signed_delta_rgb(gdelta)), "gameplay delta"))
    panels.append(label_panel(_resize(colorize(masks["slope_deg"] > CLIFF_SLOPE_DEG, (230, 120, 40),
                                               hillshade(elev))), "cliff candidates"))
    panels.append(label_panel(_resize(colorize(pyr_mask, (255, 60, 255), hillshade(elev))), "pyramid islands"))
    panels.append(label_panel(_resize(colorize(protect_struct, (230, 40, 40),
                                               colorize(layers["water"], (40, 80, 220), hillshade(elev)))),
                              "protected (red struct / blue water)"))
    panels.append(label_panel(_resize(colorize(masks["shoreline"], (60, 220, 220),
                                               colorize(layers["water"], (30, 60, 160), hillshade(elev)))),
                              "water + shoreline"))

    # --- SHORE-CONTOUR-1 deliverable: blocky-before (red) vs smoothed (blue)
    #     waterline overlay (the user's blue-line comparison). Drawn from the
    #     shipped visual surface when the shore stage ran; the "after" contour
    #     is extracted directly from the final .r32, so it is what actually
    #     bakes, not a re-simulation.
    shore_overlay_img = None
    if (visual is not None and vmeta.get("reauth")
            and (vmeta["reauth"].get("shore") or {}) and layers["water"].any()):
        sh = vmeta["reauth"]["shore"]
        srad = float(sh.get("smooth_radius_cells", 3.5))
        we = float(sh.get("water_elev", read_water_elevation(
            missions_dir / f"{mission}.fit")))
        shore_overlay_img, sh_before, sh_after = shore_contour_overlay(
            visual, we, srad, factor)
        panels.append(label_panel(shore_overlay_img.copy(),
                                  "shore: blocky(red) vs smoothed(blue)"))
        if sh_before is not None and sh_after is not None:
            fac = (sh_after["corner_count"] < sh_before["corner_count"]
                   and sh_after["median_facet_wu"] >= sh_before["median_facet_wu"])
            chk("shore_contour_smoothed", "PASS" if fac else "WARN",
                f"waterline corners {sh_before['corner_count']}->"
                f"{sh_after['corner_count']}, median facet "
                f"{sh_before['median_facet_wu']:.0f}->"
                f"{sh_after['median_facet_wu']:.0f}wu "
                f"({sh_before['contours']} contour(s), "
                f"adj_max={sh.get('bank_adjust_wu', {}).get('max', 0):.1f}wu)")
    sheet = compose_grid(panels, cols=4)

    # --- cross-section profiles: worst pyramid H/V + largest cliff ---
    # For reauth bakes the bilinear baseline is drawn too: "pyramid edges become
    # curves while summit height preserved" must be visible against the
    # straight-line (corner-pinned) baseline.
    prof_imgs = []
    PW, PH = 360, 200
    base_fine = None
    if visual is not None and vmeta.get("reauth"):   # vmeta defined whenever visual is
        base_fine = upsample_corner_pinned(elev, factor)

    def _vis_series(row=None, col=None):
        ser = []
        if visual is None:
            return ser
        for name, arr, colr in (("bilinear", base_fine, (110, 110, 120)),
                                ("visual(fine)", visual, (255, 150, 80))):
            if arr is None:
                continue
            v = arr[row * factor, :] if row is not None else arr[:, col * factor]
            ser.append((name, np.interp(np.linspace(0, len(v) - 1, side),
                                        np.arange(len(v)), v), colr))
        return ser

    if pyramids:
        py = pyramids[0]["peak_rc"]
        r0, c0 = py
        xs = np.arange(side)
        ser = [("orig", elev[r0, :], (120, 200, 255))] + _vis_series(row=r0)
        prof_imgs.append(profile_plot(PW, PH, f"pyramid#1 H-slice row={r0}", xs, ser))
        ser = [("orig", elev[:, c0], (120, 200, 255))] + _vis_series(col=c0)
        prof_imgs.append(profile_plot(PW, PH, f"pyramid#1 V-slice col={c0}", np.arange(side), ser))
    # steepest face row (max cliff-cell count = the most mountainous slice)
    cliffmask = (masks["slope_deg"] > CLIFF_SLOPE_DEG)
    if cliffmask.any():
        rc = int(np.argmax(cliffmask.sum(axis=1)))
        ser = [("orig", elev[rc, :], (120, 200, 255))] + _vis_series(row=rc)
        prof_imgs.append(profile_plot(PW, PH, f"steepest-face slice row={rc}", np.arange(side), ser))
    profiles = compose_grid([p.resize((PANEL * 2, PANEL)) for p in prof_imgs], cols=2) if prof_imgs else None

    out = out_root / mission
    out.mkdir(parents=True, exist_ok=True)
    sheet.save(out / "contact_sheet.png")
    if profiles is not None:
        profiles.save(out / "profiles.png")
    if cliff_prof_img is not None:
        cliff_prof_img.save(out / "cliff_profile.png")
    if shore_overlay_img is not None:
        # SHORE-CONTOUR-1 deliverable at panel*2 for legibility of the two lines.
        shore_overlay_img.resize((PANEL * 2, PANEL * 2), Image.NEAREST).save(
            out / "shore_contour.png")

    # summary status
    has_fail = any(c["status"] == "FAIL" for c in checks)
    has_warn = any(c["status"] == "WARN" for c in checks)
    summary = "FAIL" if has_fail else ("WARN" if has_warn else "PASS")

    report = {
        "mission": mission, "grid_side": int(side), "factor": factor,
        "summary": summary, "checks": checks,
        "pyramid_count": len(pyramids),
        "blockiness_orig": blockiness(elev),
        "cliff_section": cliff_metrics,     # CLIFF-SMOOTH-1 reference cliff
    }
    (out / "workbench_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    # HTML
    def b64(p):
        return base64.b64encode((out / p).read_bytes()).decode() if (out / p).is_file() else ""
    rows = "".join(
        f"<tr><td>{c['check']}</td><td style='color:{'#5f5' if c['status']=='PASS' else '#fd5' if c['status']=='WARN' else '#f55'}'>"
        f"{c['status']}</td><td>{c['detail']}</td></tr>" for c in checks)
    html = f"""<!doctype html><meta charset=utf-8><title>workbench {mission}</title>
<body style='background:#1a1a1f;color:#ddd;font-family:monospace'>
<h2>terrain workbench — {mission} ({side}^2, x{factor}) — <b style='color:{'#5f5' if summary=='PASS' else '#fd5' if summary=='WARN' else '#f55'}'>{summary}</b></h2>
<table border=1 cellpadding=4 style='border-collapse:collapse'>{rows}</table>
<h3>contact sheet</h3><img src='data:image/png;base64,{b64('contact_sheet.png')}'>
{("<h3>SHORE-CONTOUR-1 waterline: blocky (red) vs smoothed (blue)</h3><img src='data:image/png;base64," + b64('shore_contour.png') + "'>") if shore_overlay_img is not None else ""}
<h3>profiles</h3><img src='data:image/png;base64,{b64('profiles.png')}'>
{("<h3>reference cliff cross-section (CLIFF-SMOOTH-1)</h3><img src='data:image/png;base64," + b64('cliff_profile.png') + "'>") if cliff_prof_img is not None else ""}
</body>"""
    (out / "workbench.html").write_text(html, encoding="utf-8")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description="TERRAIN-WORKBENCH-REPORT-1 (offline)")
    ap.add_argument("missions", nargs="*", default=["mc2_01", "mc2_24"])
    ap.add_argument("--missions-dir", default="A:/Games/Carver5-feasibility/data/missions")
    ap.add_argument("--beauty", default="tests/terrain/beautify", help="dir holding <mission>.beauty/")
    ap.add_argument("--out", default="tests/terrain/workbench")
    args = ap.parse_args()
    rc = 0
    for m in args.missions:
        r = run(m, Path(args.missions_dir), Path(args.beauty), Path(args.out))
        if r.get("error"):
            print(f"[workbench] {m}: ERROR {r['error']}", file=sys.stderr); rc = 1; continue
        print(f"[workbench] {m}: {r['summary']}  " +
              "  ".join(f"{c['check']}={c['status']}" for c in r["checks"]) +
              f"  -> {Path(args.out) / m}/workbench.html")
        if r["summary"] == "FAIL":
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
