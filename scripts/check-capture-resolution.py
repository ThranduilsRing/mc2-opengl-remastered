#!/usr/bin/env python3
"""check-capture-resolution.py -- CAPTURE-RESOLUTION-ASSERT-1 (HARNESS-HYGIENE-3)

THE INSTRUMENT GAP THIS CLOSES
------------------------------
Golden regions are defined as NORMALIZED FRACTIONS of the frame
(tools/visual_verify/regions.py: "resolution-independent (preferred)"), and
tools/visual_verify/verify.py::diff_crops silently nearest-neighbour-resizes a
candidate crop up to the golden's dimensions whenever they differ:

    if (gw, gh) != (cw, ch):
        c_rgb = _nearest_resize_rgb(c_rgb, cw, ch, gw, gh)

The consequence, observed in the field: a deploy install shipped at 800x600.
Every region still cropped cleanly, every crop upscaled ~4.8x to the 4K golden's
dimensions, and the whole set graded and BLESSED successfully with roughly 17x
fewer real pixels behind it. Nothing in the pipeline compared the capture's
resolution to the golden's reference resolution. It was caught only because a
human happened to read a sidecar.

No tool asserted it. This one does.

WHAT IS AUTHORITATIVE
---------------------
The candidate's resolution is measured from the PNG IHDR of each captured
frame, NOT from a sidecar field -- capture_run.json does not record resolution
at all, and a sidecar is exactly the sort of claim that goes stale. The
reference comes from the golden's `capture_env.resolution` ("WxH"), which
bless.py records at bless time.

A golden with NO declared reference resolution is itself a FAILURE: it cannot
be checked, so it must not be silently trusted.

Usage:
    py -3 scripts/check-capture-resolution.py --golden DIR --run DIR
    py -3 scripts/check-capture-resolution.py --selftest
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

PREFIX = "[capture-resolution]"


class ResolutionMismatch(Exception):
    """Raised when a capture's resolution differs from the golden reference."""


def png_size(data: bytes) -> "tuple[int, int]":
    """(width, height) from a PNG IHDR. Raises ValueError on a non-PNG."""
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    if data[12:16] != b"IHDR":
        raise ValueError("no IHDR")
    w, h = struct.unpack(">II", data[16:24])
    return int(w), int(h)


def parse_res(s: str) -> "tuple[int, int]":
    """Parse 'WxH' -> (W, H)."""
    txt = str(s).lower().replace(" ", "")
    if "x" not in txt:
        raise ValueError(f"malformed resolution {s!r} (want WxH)")
    a, b = txt.split("x", 1)
    return int(a), int(b)


def golden_reference_resolution(golden_dir: Path) -> "tuple[int, int]":
    gj = Path(golden_dir) / "golden.json"
    if not gj.exists():
        raise ResolutionMismatch(f"no golden.json in {golden_dir}")
    doc = json.loads(gj.read_text(encoding="utf-8"))
    res = (doc.get("capture_env") or {}).get("resolution")
    if not res:
        raise ResolutionMismatch(
            f"golden {golden_dir} declares NO capture_env.resolution. An "
            f"unchecked golden is not a trusted golden -- re-bless it with a "
            f"bless.py that records the measured resolution.")
    return parse_res(res)


def candidate_resolutions(run_dir: Path) -> "list[tuple[str, int, int]]":
    """[(bookmark, w, h)] measured from each pose's primary PNG."""
    cap_path = Path(run_dir) / "capture_run.json"
    if not cap_path.exists():
        raise ResolutionMismatch(f"no capture_run.json in {run_dir}")
    cap = json.loads(cap_path.read_text(encoding="utf-8"))
    out = []
    for pose in cap.get("poses", []):
        png = pose.get("primary_png")
        if not png or not Path(png).exists():
            continue
        try:
            w, h = png_size(Path(png).read_bytes()[:64])
        except ValueError as e:
            raise ResolutionMismatch(
                f"{pose.get('bookmark')}: unreadable capture PNG ({e})")
        out.append((pose.get("bookmark", "?"), w, h))
    return out


def assert_capture_resolution(golden_dir, run_dir) -> dict:
    """Raise ResolutionMismatch unless every captured frame matches the golden.

    Returns a small record suitable for embedding in a report on success.
    """
    gw, gh = golden_reference_resolution(Path(golden_dir))
    poses = candidate_resolutions(Path(run_dir))
    if not poses:
        raise ResolutionMismatch(
            f"{run_dir}: no readable capture PNGs -- cannot verify resolution")
    bad = [(bm, w, h) for bm, w, h in poses if (w, h) != (gw, gh)]
    golden_name = f"tests/visual/goldens/{Path(golden_dir).name}"
    if bad:
        detail = ", ".join(f"{bm}={w}x{h}" for bm, w, h in bad[:6])
        ratio = (gw * gh) / max(1, bad[0][1] * bad[0][2])
        raise ResolutionMismatch(
            f"capture resolution differs from the golden reference "
            f"{gw}x{gh}: {len(bad)}/{len(poses)} pose(s) mismatched [{detail}]. "
            f"The first mismatch carries ~{ratio:.1f}x fewer pixels. Region "
            f"crops are resolution-independent and verify.py upscales silently, "
            f"so this WOULD have graded and blessed successfully. Either re-capture at "
            f"{gw}x{gh} (the install's options.cfg ResolutionX/Y; MC2_WIDTH/MC2_HEIGHT override it), or, when the golden is the stale one "
            f"(captures are pinned to the size in scripts/smoke_lib/render_resolution.py), RE-BLESS it at the pinned size: capture three "
            f"runs with `py -3 tools/visual_verify/capture.py --install <install> --bookmarks all --out <run>` and run "
            f"`py -3 tools/visual_verify/bless.py --run <run1> --run <run2> --run <run3> --to {golden_name}`. "
            f"The golden set at the pinned size is render_resolution.CURRENT_GOLDEN (`--golden current`).")
    return {"reference": f"{gw}x{gh}", "poses_checked": len(poses),
            "verdict": "MATCH"}


# ---------------------------------------------------------------------------
# Self-test: positive AND negative controls, no engine and no real capture.
# ---------------------------------------------------------------------------
def _fake_png(w: int, h: int) -> bytes:
    return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR"
            + struct.pack(">II", w, h) + b"\x08\x02\x00\x00\x00" + b"\x00" * 8)


def _selftest() -> int:
    import tempfile
    ok = True

    def build(tmp: Path, golden_res, cap_res, omit_res=False):
        g = tmp / "golden"; g.mkdir(parents=True, exist_ok=True)
        env = {} if omit_res else {"resolution": golden_res}
        (g / "golden.json").write_text(json.dumps(
            {"kind": "visual-verify-golden", "capture_env": env}))
        r = tmp / "run"; r.mkdir(parents=True, exist_ok=True)
        w, h = parse_res(cap_res)
        png = r / "pose.png"
        png.write_bytes(_fake_png(w, h))
        (r / "capture_run.json").write_text(json.dumps(
            {"kind": "visual-verify-capture",
             "poses": [{"bookmark": "bm1", "primary_png": str(png)}]}))
        return g, r

    def case(label, golden_res, cap_res, expect_fail, omit_res=False):
        nonlocal ok
        with tempfile.TemporaryDirectory() as td:
            g, r = build(Path(td), golden_res, cap_res, omit_res)
            try:
                assert_capture_resolution(g, r)
                got, msg = False, ""
            except ResolutionMismatch as e:
                got, msg = True, str(e)
        good = got == expect_fail
        if not good:
            ok = False
        print(f"{PREFIX} selftest {'ok' if good else 'BROKEN'}: {label} "
              f"(expected_fail={expect_fail} got_fail={got})"
              + (f" :: {msg[:100]}" if msg else ""))

    # POSITIVE CONTROL -- matching resolution must pass.
    case("4K capture vs 4K golden passes", "3840x2160", "3840x2160", False)
    # NEGATIVE CONTROL -- the exact field incident: 800x600 vs a 4K golden.
    case("800x600 capture vs 4K golden FAILS", "3840x2160", "800x600", True)
    # NEGATIVE CONTROL -- subtle, same aspect ratio (would look fine upscaled).
    case("1920x1080 vs 4K golden FAILS (same aspect)", "3840x2160", "1920x1080", True)
    # NEGATIVE CONTROL -- one axis off by a little.
    case("3840x2159 vs 4K golden FAILS", "3840x2160", "3840x2159", True)
    # The refusal names the re-bless command, so the person who hits it is not left to find it.
    with tempfile.TemporaryDirectory() as td:
        g, r = build(Path(td), "3840x2160", "1920x1080")
        try:
            assert_capture_resolution(g, r)
            named = False
        except ResolutionMismatch as e:
            named = "bless.py --run" in str(e) and "RE-BLESS" in str(e)
        if not named:
            ok = False
        print(f"{PREFIX} selftest {'ok' if named else 'BROKEN'}: the refusal names the re-bless command")
    # NEGATIVE CONTROL -- golden that declares no reference at all.
    case("golden with no declared resolution FAILS", "3840x2160", "3840x2160",
         True, omit_res=True)

    print(f"{PREFIX} selftest {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def current_golden_dir() -> Path:
    """tests/visual/goldens/<render_resolution.CURRENT_GOLDEN>: the golden set blessed at the pinned capture size."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from smoke_lib import render_resolution
    return Path(__file__).resolve().parents[1] / "tests" / "visual" / "goldens" / render_resolution.CURRENT_GOLDEN


def check_current_golden() -> "list[str]":
    """Problems with the CURRENT_GOLDEN set itself: missing on disk, or recorded at a size other than the pinned capture size."""
    from smoke_lib import render_resolution
    gdir = current_golden_dir()
    if not (gdir / "golden.json").is_file():
        return [f"render_resolution.CURRENT_GOLDEN names {gdir.name}, which has no golden.json under tests/visual/goldens"]
    try:
        have = golden_reference_resolution(gdir)
    except ResolutionMismatch as e:
        return [str(e)]
    want = (render_resolution.PINNED_WIDTH, render_resolution.PINNED_HEIGHT)
    return [] if have == want else [f"{gdir.name} is recorded at {have[0]}x{have[1]} but captures are pinned to {want[0]}x{want[1]}"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--golden", help="golden dir (contains golden.json), or `current` for the golden set at the pinned size")
    ap.add_argument("--run", help="capture dir (contains capture_run.json)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--current", action="store_true",
                    help="check the CURRENT_GOLDEN set itself (present, recorded at the pinned size); with --run also grade that capture against it")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()
    if args.golden == "current" or args.current:
        problems = check_current_golden()
        for p in problems:
            print(f"FAIL: {PREFIX} {p}", file=sys.stderr)
        if problems:
            return 1
        if args.golden in (None, "current"):
            args.golden = str(current_golden_dir())
        if not args.run:
            print(f"PASS: {PREFIX} the current golden {Path(args.golden).name} is recorded at the pinned size")
            return 0
    if not (args.golden and args.run):
        ap.error("--golden and --run are required (or use --selftest / --current)")
    try:
        rec = assert_capture_resolution(args.golden, args.run)
    except ResolutionMismatch as e:
        print(f"FAIL: {PREFIX} {e}", file=sys.stderr)
        return 1
    print(f"PASS: {PREFIX} {rec['poses_checked']} pose(s) at {rec['reference']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
