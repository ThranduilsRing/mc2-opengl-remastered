#!/usr/bin/env python3
"""sim_identity.py -- SIM-IDENTITY-HASH-1 refuse-to-grade gate.

THE PROBLEM THIS CLOSES
=======================
Mover nondeterminism was being chased as a metrics problem. It is a DESYNC --
the class lockstep games (OpenTTD desync logs, OpenRCT2 sprite checksums,
Dolphin FIFO, libTAS) industrialized long ago. Their fix is not a sharper pixel
diff; it is a world-state hash plus a refusal to compare arms whose worlds
differ.

MC2 had been trying to settle sim-identity and render-identity with ONE
instrument -- the pixel sha. A pixel sha cannot tell "this gate changed the
shading" from "these two runs were showing different worlds". So:

  * three of four A/B arms were nearly reported as fixes while sitting inside
    run-to-run drift, and
  * one lane's "regression handle" turned out to be non-comparable arms.

The engine half (mclib/sim_identity.h + code/sim_identity.cpp) emits a
world-state hash per capture and, optionally, per sim frame. This is the half
that REFUSES.

THE VERDICT SET
===============
  GRADED       worlds identical -> a pixel comparison means something.
  LATENT       worlds differ ONLY in non-visual fields (target / brain / move /
               path). No pixel can differ this frame, so the comparison still
               means something -- but the sim HAS diverged and will propagate.
               Graded, and loudly annotated. This is the early warning
               DETERMINISM-RATCHET-2 had to hand-derive from ~58k log lines.
  UNGRADED     worlds differ in VISUAL fields (position / rotation / gesture /
               anim frame / damage / status / team) or in population. The arms
               are NOT COMPARABLE. Not a pass. Not a fail. UNGRADED.
  UNVERIFIED   a hash is ABSENT on at least one arm, so world identity could not
               be checked at all. Grades, but the verdict is stamped and the
               warning is loud -- see below for why this is NOT a quiet pass.

WHY `UNVERIFIED` EXISTS AND IS NOT JUST UNGRADED
================================================
Refusing on an absent block is the obviously "rigorous" choice and it is wrong,
because it would return UNGRADED for every manifest captured before this slice
existed and every capture taken with an older exe. That is the wallpaper trap
arriving on day one, and the predictable result is that somebody disables the
gate -- leaving nothing.

So absence GRADES BY DEFAULT with a stamped, loud UNVERIFIED, and
`require_identity=True` (CLI `--require-sim-identity`) turns absence into a hard
UNGRADED. Once every deploy in use emits the block, strict mode should become
the default for release gating; until then, refusing would be refusing to grade
the entire past.

The distinction UNVERIFIED preserves is the one that actually matters: a
verdict that says "I could not check whether these arms were comparable" is a
different claim from "I checked and they were". The old grader could only make
the second claim, and made it whether or not it was true.

Note the deliberate asymmetry: an empty-but-PRESENT block (mover_count 0, a
mission with no movers) is GRADED, not UNVERIFIED. The engine emits a block even
with zero movers precisely so "nothing to diverge" and "nothing measured" stay
distinguishable.

HOW `UNGRADED` IS KEPT FROM BECOMING WALLPAPER
==============================================
A refusal verdict that fires on most runs gets switched off, and then it is
worse than nothing. Five mechanisms, in order of how much work they do:

 1. VISUAL/NON-VISUAL SPLIT (the big one). Grounded in measurement, not taste:
    on mc2_10 the first divergence (t=34.1s) was ONE mover's `target` with every
    position identical; the first POSITION divergence was 0.3s later. A gate
    blind to that distinction refuses at t=34.1s -- where the picture provably
    cannot differ. This one refuses only from t=34.4s.
 2. QUANTIZED grading digest. Sub-quantum float wobble that cannot move a pixel
    does not refuse. The strict (raw-bit) digest is still reported, so the
    difference stays visible without being fatal.
 3. `sim_affecting` DECLARATION. A gate that legitimately changes the sim (a
    called-shot fix, an AI change) declares it; sim divergence is then EXPECTED
    and reported, not refused. Without this, every sim-affecting gate would be
    permanently ungraded -- guaranteed wallpaper.
 4. The hash defaults ON in the engine. A refusal gate fed by a default-OFF
    producer would return UNGRADED for every pre-existing manifest on day one.
 5. AN UNGRADED RATE IS REPORTED, and `--max-ungraded-frac` can assert it. If
    this gate starts firing on most runs, that is itself a measured, visible
    fact rather than something people quietly route around.

LOCALISATION
============
A verdict that says only "something diverged" is barely better than the drift it
replaced. Every UNGRADED/LATENT verdict names:
  * the FIRST diverging mover by id (ascending, so it is stable run to run),
  * WHICH fields differ, with both values,
  * the true numeric delta beside the quantum for float fields, so a
    quantum-boundary straddle (delta << quantum) is instantly distinguishable
    from real divergence (delta >> quantum),
  * the population delta when the mover SETS differ.

Pure stdlib.

Usage:
  sim_identity.py compare A.json B.json [--sim-affecting] [--json]
  sim_identity.py trace-diff run_a.log run_b.log      # first divergent sim frame
  sim_identity.py self-test
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Verdicts.
GRADED = "GRADED"
LATENT = "LATENT"
UNVERIFIED = "UNVERIFIED"
UNGRADED = "UNGRADED"

# Process exit code for "not comparable". Distinct from 0 (pass) and 1 (fail)
# on purpose: a silent pass on incomparable arms is how false fixes got
# reported, and a FAIL would be just as wrong -- nothing was measured.
#
# Code census at time of writing (run_golden_parity.py): 0 pass, 1 parity fail,
# 2 usage, 3 abort, 4 capture-liveness, 5 backend-fallback-inconclusive, and 6
# is claimed by the in-flight GOLDEN-PROVENANCE-TRUTH-1 lane ("provenance
# unestablished" -- same refuse-to-grade family, different axis: it refuses when
# the two arms ran different BINARIES, this refuses when they ran different
# WORLDS). 7 is free.
EXIT_UNGRADED = 7

# Field classification. Must mirror mclib/sim_identity.h -- digestMoverVisual()
# folds exactly the VISUAL set. `check-sim-identity-fieldsets` (contract) proves
# the two lists have not drifted apart.
VISUAL_FIELDS = ("team", "pos", "rot", "hp", "gesture", "anim", "status")
NONVISUAL_FIELDS = ("target", "brain", "mstate", "pstep", "psteps")

# Float fields get a delta-vs-quantum report; the rest are exact.
FLOAT_FIELDS = {"pos": "pos", "rot": "ang", "hp": "frac"}


# ---------------------------------------------------------------------------
# manifest access
# ---------------------------------------------------------------------------
def find_block(obj):
    """Find the sim_identity block anywhere in a manifest.

    Deliberately a search, not a fixed path: the block is written by the engine
    into a per-bookmark sidecar and then re-nested by run_visual_capture and
    again by run_golden_parity. Pinning one path would make the gate silently
    blind the first time a wrapper changed its nesting -- and a blind gate that
    returns GRADED is exactly the failure mode this whole slice exists to stop.
    Returns the first block found in deterministic key order, or None.
    """
    if isinstance(obj, dict):
        blk = obj.get("sim_identity")
        if isinstance(blk, dict) and "digest" in blk:
            return blk
        for k in sorted(obj.keys()):
            r = find_block(obj[k])
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = find_block(v)
            if r is not None:
                return r
    return None


def load_block(path_or_obj):
    if isinstance(path_or_obj, (str, Path)):
        obj = json.loads(Path(path_or_obj).read_text(encoding="utf-8"))
    else:
        obj = path_or_obj
    return find_block(obj)


def _movers_by_id(block):
    out = {}
    for m in block.get("movers", []) or []:
        try:
            out[int(m["id"])] = m
        except (KeyError, TypeError, ValueError):
            continue
    return out


# ---------------------------------------------------------------------------
# comparison
# ---------------------------------------------------------------------------
def _field_diffs(ma, mb, quantum):
    """Per-field diffs for one mover, split visual vs non-visual.
    Returns (visual_diffs, nonvisual_diffs); each item is a dict."""
    vis, non = [], []
    for field in VISUAL_FIELDS + NONVISUAL_FIELDS:
        av, bv = ma.get(field), mb.get(field)
        if av == bv:
            continue
        item = {"field": field, "a": av, "b": bv}
        qkey = FLOAT_FIELDS.get(field)
        if qkey:
            q = (quantum or {}).get(qkey)
            try:
                if isinstance(av, list) and isinstance(bv, list):
                    delta = max(abs(float(x) - float(y)) for x, y in zip(av, bv))
                else:
                    delta = abs(float(av) - float(bv))
                item["delta"] = delta
                item["quantum"] = q
                # A delta below the quantum means the two values landed in
                # different buckets only because they straddled a boundary --
                # real, but far below anything a pixel can show. Naming it here
                # stops a boundary artifact from reading as genuine divergence.
                if q:
                    item["subquantum"] = delta < float(q)
            except (TypeError, ValueError):
                pass
        (vis if field in VISUAL_FIELDS else non).append(item)
    return vis, non


def compare(block_a, block_b, sim_affecting=False, require_identity=False):
    """Compare two sim_identity blocks.

    Returns a dict: verdict, reason, and (when they differ) full localisation.
    `sim_affecting=True` declares that the two arms are EXPECTED to have
    different worlds -- the divergence is then reported but never refused.
    `require_identity=True` promotes an absent block from UNVERIFIED to a hard
    UNGRADED (strict mode; correct once every deploy emits the block).
    """
    res = {
        "verdict": GRADED,
        "reason": "",
        "sim_affecting": bool(sim_affecting),
        "digest_a": None, "digest_b": None,
        "visual_a": None, "visual_b": None,
        "strict_a": None, "strict_b": None,
        "first_divergent_mover": None,
        "visual_diff_movers": [],
        "nonvisual_diff_movers": [],
        "only_in_a": [], "only_in_b": [],
        "detail": [],
    }

    # --- absence: stamped UNVERIFIED by default, hard refusal in strict mode -
    if block_a is None or block_b is None:
        which = ",".join([x for x, b in (("A", block_a), ("B", block_b))
                          if b is None])
        res["verdict"] = UNGRADED if require_identity else UNVERIFIED
        res["reason"] = (
            "sim_identity block ABSENT on arm(s) %s -- world identity was NOT "
            "checked. This is not a claim that the arms were comparable, only "
            "that nobody looked. Cause is almost always an exe predating "
            "SIM-IDENTITY-HASH-1, or MC2_SIM_IDENTITY explicitly set off (it "
            "defaults ON, and a mission with zero movers still emits a block). "
            "%s" % (which or "?",
                    "Strict mode (--require-sim-identity): refusing."
                    if require_identity else
                    "Grading anyway; pass --require-sim-identity to refuse.")
        )
        return res

    res["digest_a"] = block_a.get("digest")
    res["digest_b"] = block_b.get("digest")
    res["visual_a"] = block_a.get("digest_visual")
    res["visual_b"] = block_b.get("digest_visual")
    res["strict_a"] = block_a.get("digest_strict")
    res["strict_b"] = block_b.get("digest_strict")

    # --- quanta must match, or the digests are not commensurable ------------
    qa, qb = block_a.get("quantum"), block_b.get("quantum")
    if qa != qb:
        res["verdict"] = UNGRADED
        res["reason"] = ("quantum sets differ (A=%s B=%s) -- digests built with "
                         "different quanta are not commensurable" % (qa, qb))
        return res

    # --- a truncated population would hash 'clean' while diverging ----------
    if block_a.get("truncated") or block_b.get("truncated"):
        res["verdict"] = UNGRADED
        res["reason"] = ("mover population TRUNCATED on at least one arm -- a "
                         "digest over a partial world can agree while the "
                         "unhashed remainder diverges")
        return res

    # --- fast path: identical graded digests --------------------------------
    if (res["digest_a"] == res["digest_b"]
            and res["digest_a"] is not None):
        res["reason"] = "world-state digests identical"
        if res["strict_a"] != res["strict_b"]:
            # Same graded world, different raw bits: sub-quantum float noise.
            # Reported so it is never invisible; not a refusal, because nothing
            # below a quantum can move a pixel.
            res["reason"] += (" (strict digests differ -- sub-quantum float "
                              "noise only; graded world identical)")
            res["detail"].append("SUBQUANTUM: strict %s != %s"
                                 % (res["strict_a"], res["strict_b"]))
        return res

    # --- localise -----------------------------------------------------------
    ma, mb = _movers_by_id(block_a), _movers_by_id(block_b)
    ids_a, ids_b = set(ma), set(mb)
    res["only_in_a"] = sorted(ids_a - ids_b)
    res["only_in_b"] = sorted(ids_b - ids_a)

    first_div = None
    for mid in sorted(ids_a & ids_b):
        vis, non = _field_diffs(ma[mid], mb[mid], qa)
        if vis:
            res["visual_diff_movers"].append(
                {"id": mid, "fields": vis})
        if non:
            res["nonvisual_diff_movers"].append(
                {"id": mid, "fields": non})
        if (vis or non) and first_div is None:
            first_div = {"id": mid, "visual": vis, "nonvisual": non}
    res["first_divergent_mover"] = first_div

    population_differs = bool(res["only_in_a"] or res["only_in_b"])
    visual_differs = bool(res["visual_diff_movers"]) or population_differs

    if sim_affecting:
        res["verdict"] = GRADED
        res["reason"] = (
            "worlds differ but the arms DECLARE sim_affecting -- divergence is "
            "expected and is reported, not refused (%d mover(s) differ "
            "visually, %d non-visually)"
            % (len(res["visual_diff_movers"]), len(res["nonvisual_diff_movers"])))
        return res

    if visual_differs:
        res["verdict"] = UNGRADED
        bits = []
        if population_differs:
            bits.append("mover POPULATION differs (only-in-A=%s only-in-B=%s)"
                        % (res["only_in_a"] or "[]", res["only_in_b"] or "[]"))
        if res["visual_diff_movers"]:
            bits.append("%d mover(s) differ in VISUAL fields"
                        % len(res["visual_diff_movers"]))
        res["reason"] = ("; ".join(bits) +
                         " -- the two arms were drawing different worlds, so a "
                         "pixel comparison between them measures nothing")
        return res

    # Non-visual only: the sim HAS diverged, but not yet visibly.
    res["verdict"] = LATENT
    res["reason"] = (
        "%d mover(s) differ in NON-VISUAL fields only (%s); every visual field "
        "and the whole population match, so the rendered frame cannot differ. "
        "Graded -- but the sim HAS desynced and will propagate to visible state."
        % (len(res["nonvisual_diff_movers"]),
           ",".join(sorted({f["field"]
                            for m in res["nonvisual_diff_movers"]
                            for f in m["fields"]}))))
    return res


# ---------------------------------------------------------------------------
# reporting
# ---------------------------------------------------------------------------
def _fmt_field(f):
    s = "%s: %r -> %r" % (f["field"], f["a"], f["b"])
    if "delta" in f and f.get("quantum"):
        s += ("  (delta=%.6g quantum=%.6g%s)"
              % (f["delta"], f["quantum"],
                 ", SUB-QUANTUM boundary straddle" if f.get("subquantum") else ""))
    return s


def render(res, prefix="[sim_identity]"):
    out = []
    out.append("%s VERDICT: %s" % (prefix, res["verdict"]))
    out.append("%s   %s" % (prefix, res["reason"]))
    if res["digest_a"] or res["digest_b"]:
        out.append("%s   graded A=%s B=%s" % (prefix, res["digest_a"], res["digest_b"]))
        out.append("%s   visual A=%s B=%s" % (prefix, res["visual_a"], res["visual_b"]))
    for d in res["detail"]:
        out.append("%s   %s" % (prefix, d))
    fd = res.get("first_divergent_mover")
    if fd:
        out.append("%s   FIRST divergent mover: id=%d" % (prefix, fd["id"]))
        for f in fd["visual"]:
            out.append("%s     VISUAL     %s" % (prefix, _fmt_field(f)))
        for f in fd["nonvisual"]:
            out.append("%s     non-visual %s" % (prefix, _fmt_field(f)))
    if res["only_in_a"] or res["only_in_b"]:
        out.append("%s   population: only-in-A=%s only-in-B=%s"
                   % (prefix, res["only_in_a"], res["only_in_b"]))
    n_vis = len(res["visual_diff_movers"])
    n_non = len(res["nonvisual_diff_movers"])
    if n_vis or n_non:
        out.append("%s   totals: %d mover(s) visual, %d mover(s) non-visual"
                   % (prefix, n_vis, n_non))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# trace diff -- first divergent sim frame from two run logs
# ---------------------------------------------------------------------------
def parse_trace(path):
    """Parse '[SIMID v1] simframe=N t=T n=C graded=G visual=V strict=S' lines
    into an ordered list of dicts. Ignores everything else in the log."""
    rows = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("[SIMID v1] "):
            continue
        kv = {}
        for tok in line[len("[SIMID v1] "):].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        if "simframe" in kv:
            rows.append(kv)
    return rows


def trace_diff(path_a, path_b):
    """First sim frame at which two runs' world hashes diverge, and in which
    digest -- graded (any field) vs visual (pixel-relevant)."""
    ra, rb = parse_trace(path_a), parse_trace(path_b)
    by_a = {r["simframe"]: r for r in ra}
    by_b = {r["simframe"]: r for r in rb}
    common = sorted(set(by_a) & set(by_b), key=lambda s: int(s))
    res = {"frames_a": len(ra), "frames_b": len(rb), "common": len(common),
           "first_graded": None, "first_visual": None, "first_strict": None}
    for f in common:
        a, b = by_a[f], by_b[f]
        if res["first_strict"] is None and a.get("strict") != b.get("strict"):
            res["first_strict"] = {"simframe": f, "t": a.get("t")}
        if res["first_graded"] is None and a.get("graded") != b.get("graded"):
            res["first_graded"] = {"simframe": f, "t": a.get("t"),
                                   "a": a.get("graded"), "b": b.get("graded")}
        if res["first_visual"] is None and a.get("visual") != b.get("visual"):
            res["first_visual"] = {"simframe": f, "t": a.get("t"),
                                   "a": a.get("visual"), "b": b.get("visual")}
        if res["first_graded"] and res["first_visual"] and res["first_strict"]:
            break
    return res


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------
def _selftest():
    """Offline proof that each verdict fires for the right reason.

    Case 4 is the one that matters most: it is the REAL mc2_10 divergence
    (DETERMINISM-RATCHET-2, t=34.1s -- id=711 Centipede, target -1 vs 84146,
    all positions identical). A gate that refuses there is wallpaper; a gate
    that reports it as clean is blind. It must come back LATENT.
    """
    q = {"pos": 0.015625, "ang": 0.015625, "frac": 0.0009765625}

    def mover(mid, pos=(0.0, 0.0, 0.0), rot=0.0, target=-1, gesture=1,
              hp=1.0, team=1, anim=0, status=0, brain=0, mstate=0,
              pstep=0, psteps=0):
        return {"id": mid, "team": team, "pos": list(pos), "rot": rot, "hp": hp,
                "gesture": gesture, "anim": anim, "status": status,
                "target": target, "brain": brain, "mstate": mstate,
                "pstep": pstep, "psteps": psteps}

    def block(movers, digest="d0", visual="v0", strict="s0", **kw):
        b = {"schema": "SIM_IDENTITY_V1", "digest": digest,
             "digest_visual": visual, "digest_strict": strict,
             "mover_count": len(movers), "truncated": False,
             "quantum": q, "movers": movers}
        b.update(kw)
        return b

    cases = []

    # 1. identical worlds -> GRADED
    m = [mover(711, (-2112.0, 2922.7, 252.4)), mover(102, (5.0, 6.0, 7.0))]
    cases.append(("identical worlds", block(m), block(list(m)), False, GRADED))

    # 2. absent block on one arm -> UNVERIFIED (stamped, but still grades:
    #    refusing here would refuse every pre-slice manifest = day-one wallpaper)
    cases.append(("block absent on B (default)", block(m), None, False, UNVERIFIED))

    # 3. absent on both -> UNVERIFIED
    cases.append(("block absent on both (default)", None, None, False, UNVERIFIED))

    # 3b. an empty-but-PRESENT block is GRADED, not UNVERIFIED -- "no movers to
    #     diverge" must stay distinguishable from "nothing measured".
    cases.append(("empty world, block present on both",
                  block([]), block([]), False, GRADED))

    # 4. THE REAL CASE: target-only, positions identical -> LATENT (gradeable)
    a4 = [mover(711, (-2112.0, 2922.7, 252.4), target=-1)]
    b4 = [mover(711, (-2112.0, 2922.7, 252.4), target=84146)]
    cases.append(("mc2_10 t=34.1s target-only (positions identical)",
                  block(a4, digest="dA"), block(b4, digest="dB"), False, LATENT))

    # 5. position divergence 0.3s later -> UNGRADED (refuses, correctly)
    a5 = [mover(711, (-2112.0, 2922.7, 252.4))]
    b5 = [mover(711, (-2112.0, 2925.9, 252.4))]
    cases.append(("mc2_10 t=34.4s position divergence",
                  block(a5, digest="dA"), block(b5, digest="dB"), False, UNGRADED))

    # 6. sim-affecting declared -> GRADED even with visual divergence
    cases.append(("visual divergence + sim_affecting declared",
                  block(a5, digest="dA"), block(b5, digest="dB"), True, GRADED))

    # 7. population differs -> UNGRADED
    a7 = [mover(1), mover(2)]
    b7 = [mover(1)]
    cases.append(("mover population differs",
                  block(a7, digest="dA"), block(b7, digest="dB"), False, UNGRADED))

    # 8. truncated -> UNGRADED
    cases.append(("population truncated",
                  block(m, truncated=True), block(m), False, UNGRADED))

    # 9. quanta differ -> UNGRADED (digests not commensurable)
    b9 = block(m)
    b9["quantum"] = {"pos": 0.5, "ang": 0.5, "frac": 0.5}
    cases.append(("quantum sets differ", block(m), b9, False, UNGRADED))

    # 10. same graded digest, different strict -> GRADED + SUBQUANTUM note
    cases.append(("sub-quantum float noise only",
                  block(m, strict="sA"), block(m, strict="sB"), False, GRADED))

    ok = True
    for name, ba, bb, affecting, want in cases:
        got = compare(ba, bb, sim_affecting=affecting)
        good = got["verdict"] == want
        ok = ok and good
        print("[selftest] %-52s -> %-10s (want %-10s) [%s]"
              % (name[:52], got["verdict"], want, "OK" if good else "WRONG"))

    # --- strict mode promotes absence to a hard refusal --------------------
    strict = compare(block(m), None, require_identity=True)
    strict_ok = strict["verdict"] == UNGRADED
    ok = ok and strict_ok
    print("[selftest] %-52s -> %-10s (want %-10s) [%s]"
          % ("block absent + --require-sim-identity", strict["verdict"],
             UNGRADED, "OK" if strict_ok else "WRONG"))

    # --- localisation must NAME the mover and the field ---------------------
    r = compare(block(a4, digest="dA"), block(b4, digest="dB"))
    fd = r.get("first_divergent_mover") or {}
    loc_ok = (fd.get("id") == 711
              and any(f["field"] == "target" for f in fd.get("nonvisual", []))
              and not fd.get("visual"))
    ok = ok and loc_ok
    print("[selftest] %-52s -> %s [%s]"
          % ("localises id=711 field=target",
             "id=%s fields=%s" % (fd.get("id"),
                                  [f["field"] for f in fd.get("nonvisual", [])]),
             "OK" if loc_ok else "WRONG"))

    # --- boundary straddle must be NAMED sub-quantum ------------------------
    a11 = [mover(5, (10.0, 0.0, 0.0))]
    b11 = [mover(5, (10.0 + 0.000001, 0.0, 0.0))]
    r11 = compare(block(a11, digest="dA"), block(b11, digest="dB"))
    f11 = (r11.get("first_divergent_mover") or {}).get("visual", [])
    straddle_ok = bool(f11) and f11[0].get("subquantum") is True
    ok = ok and straddle_ok
    print("[selftest] %-52s -> %s [%s]"
          % ("names a quantum-boundary straddle as SUB-QUANTUM",
             "subquantum=%s" % (f11[0].get("subquantum") if f11 else None),
             "OK" if straddle_ok else "WRONG"))

    # --- a real (super-quantum) move must NOT be called sub-quantum ---------
    a12 = [mover(5, (10.0, 0.0, 0.0))]
    b12 = [mover(5, (13.2, 0.0, 0.0))]
    r12 = compare(block(a12, digest="dA"), block(b12, digest="dB"))
    f12 = (r12.get("first_divergent_mover") or {}).get("visual", [])
    real_ok = bool(f12) and f12[0].get("subquantum") is False
    ok = ok and real_ok
    print("[selftest] %-52s -> %s [%s]"
          % ("real divergence is NOT labelled sub-quantum",
             "subquantum=%s" % (f12[0].get("subquantum") if f12 else None),
             "OK" if real_ok else "WRONG"))

    # --- find_block must reach a NESTED block -------------------------------
    nested = {"a": {"b": [{"c": {"sim_identity": block(m)}}]}}
    nest_ok = find_block(nested) is not None
    ok = ok and nest_ok
    print("[selftest] %-52s -> %s [%s]"
          % ("find_block reaches a deeply nested block",
             nest_ok, "OK" if nest_ok else "WRONG"))

    print("[selftest] RESULT: %s" % ("ALL PASS" if ok else "FAILED"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("self-test", help="offline verdict + localisation proof")

    cp = sub.add_parser("compare", help="compare two manifests' sim identity")
    cp.add_argument("a")
    cp.add_argument("b")
    cp.add_argument("--sim-affecting", action="store_true",
                    help="declare the arms are EXPECTED to have different "
                         "worlds; divergence is reported, never refused")
    cp.add_argument("--require-sim-identity", action="store_true",
                    help="strict: an ABSENT hash becomes UNGRADED instead of a "
                         "stamped UNVERIFIED. Correct once every deploy in use "
                         "emits the block; refuses the entire past until then.")
    cp.add_argument("--json", action="store_true", help="emit the raw result dict")

    td = sub.add_parser("trace-diff",
                        help="first divergent sim frame between two run logs "
                             "(needs MC2_SIM_IDENTITY_TRACE=1)")
    td.add_argument("a")
    td.add_argument("b")

    args = ap.parse_args()

    if args.cmd == "self-test":
        return _selftest()

    if args.cmd == "trace-diff":
        r = trace_diff(args.a, args.b)
        print(json.dumps(r, indent=2))
        if r["common"] == 0:
            print("[sim_identity] no common sim frames -- was "
                  "MC2_SIM_IDENTITY_TRACE=1 set for BOTH runs?", file=sys.stderr)
            return 2
        if r["first_visual"]:
            print("[sim_identity] first VISUAL divergence at simframe=%s (t=%s)"
                  % (r["first_visual"]["simframe"], r["first_visual"]["t"]))
        elif r["first_graded"]:
            print("[sim_identity] sim diverged (simframe=%s) but never visibly "
                  "in this window -- LATENT throughout"
                  % r["first_graded"]["simframe"])
        else:
            print("[sim_identity] no divergence across %d common sim frames"
                  % r["common"])
        return 0

    ba, bb = load_block(args.a), load_block(args.b)
    res = compare(ba, bb, sim_affecting=args.sim_affecting,
                  require_identity=args.require_sim_identity)
    if args.json:
        print(json.dumps(res, indent=2, sort_keys=True))
    else:
        print(render(res))
    return EXIT_UNGRADED if res["verdict"] == UNGRADED else 0


if __name__ == "__main__":
    sys.exit(main())
