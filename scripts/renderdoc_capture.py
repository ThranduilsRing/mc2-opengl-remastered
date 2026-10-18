#!/usr/bin/env python3
"""Tier 5 RenderDoc capture orchestrator.

Drives the engine-side hook in gos_rdoc_capture.cpp end-to-end:

  1. Launches mc2.exe under `renderdoccmd capture` so renderdoc.dll is
     injected and the in-process API resolves.
  2. The engine waits for the WasEverFrameSolidArmed latch, counts frames,
     and calls RENDERDOC_API_1_5_0::TriggerCapture() at frame N. With
     MC2_RDC_EXIT_AFTER=1 the engine exits after the capture is finalized.
  3. The resulting .rdc is converted to a pipeline-state JSON via
     `renderdoccmd convert` and (optionally) diffed against a baseline.

The harness is opt-in. It is not on the default smoke gate. See
docs/testing-strategy.md Tier 5 for the contract.
"""
from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from smoke_lib import nofocus  # noqa: E402

# Canonical install path on this machine (memory says RenderDoc lives here).
DEFAULT_RDOC_DIR = Path(r"C:/Program Files/RenderDoc")
WORKTREE = Path(__file__).resolve().parent.parent
DEPLOY = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4")

# Lines / attributes known to vary between two identical runs even when the
# pipeline is byte-for-byte equivalent. Replaced with a fixed token before
# diffing so we don't chase noise. Targets the RenderDoc XML export schema:
#   <timebase ...>, <duration ...>, <frame_index ...>, GL object names
#   (Buffer 12345 -> Buffer NNN), and machine-local capture metadata.
NOISE_LINE_PATTERNS = [
    (re.compile(r"<timebase[^>]*/>"),       "<timebase NORMALIZED/>"),
    (re.compile(r"<frame_index[^>]*/>"),    "<frame_index NORMALIZED/>"),
    (re.compile(r'machineIdent="[^"]*"'),   'machineIdent="NORMALIZED"'),
    (re.compile(r'timeStamp="[^"]*"'),      'timeStamp="NORMALIZED"'),
    (re.compile(r'driver="[^"]*"'),         'driver="NORMALIZED"'),
    # Per-chunk profiling metadata: thread IDs, wall-clock timestamps, and
    # per-call duration. These vary between two identical runs of the same
    # build and have nothing to do with the GL pipeline state.
    (re.compile(r'\sthreadID="\d+"'),       ' threadID="N"'),
    (re.compile(r'\stimestamp="\d+"'),      ' timestamp="N"'),
    (re.compile(r'\sduration="\d+"'),       ' duration="N"'),
    # Capture serial / GUID-like fields if present.
    (re.compile(r'captureBeginTime="[^"]*"'), 'captureBeginTime="N"'),
    # Per-frame uniform / vertex-attrib float payloads. The pipeline-state
    # contract is "which program is bound, which textures, which blend /
    # depth / sampler state" -- NOT "are the bytes inside a UBO identical
    # frame-to-frame." MC2 is animation-driven; camera floats, view matrix
    # entries, and per-actor transform floats drift sub-frame even with a
    # fixed seed. Normalize them so the diff only fires on pipeline-state
    # regressions, which is the entire reason for Tier 5.
    (re.compile(r'(<float typename="float" width="4">)[^<]+(</float>)'),
                 r'\1N\2'),
    (re.compile(r'(<double typename="double" width="8">)[^<]+(</double>)'),
                 r'\1N\2'),
    # GL object IDs allocated by the driver (textures, sync objects, buffers)
    # have monotonically-increasing handles. Two runs that present the same
    # frame land on different handle values because earlier frames have
    # allocated different counts of transient objects. The pipeline contract
    # is "what kind of object", not "which numeric handle".
    (re.compile(r'(<ResourceId [^>]*>)\d+(</ResourceId>)'), r'\1N\2'),
    # Streamed-VBO / streamed-UBO upload sizes: per-frame culling changes
    # how many bytes get flushed to the same persistent-mapped buffer; the
    # buffer identity is stable, only the per-frame written byteLength
    # fluctuates.
    (re.compile(r'(<buffer [^>]*\sbyteLength=")\d+"'), r'\1N"'),
    # Per-frame counted integers (uint / int chunks). These cover GL sync
    # serials, draw counts, and other monotonic counters that drift between
    # otherwise-identical runs. We intentionally do NOT normalize <enum> --
    # enum values are pipeline-state (blend mode, depth func, etc).
    (re.compile(r'(<uint typename="uint64_t" width="8"[^>]*>)\d+(</uint>)'),
                 r'\1N\2'),
    (re.compile(r'(<uint typename="uint32_t" width="4"[^>]*>)\d+(</uint>)'),
                 r'\1N\2'),
]


def normalize_text(line: str) -> str:
    for pat, repl in NOISE_LINE_PATTERNS:
        line = pat.sub(repl, line)
    return line


def find_renderdoccmd(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise SystemExit(f"renderdoccmd not found at {p}")
    on_path = shutil.which("renderdoccmd")
    if on_path:
        return Path(on_path)
    candidate = DEFAULT_RDOC_DIR / "renderdoccmd.exe"
    if candidate.is_file():
        return candidate
    raise SystemExit("renderdoccmd not found (not on PATH, not in default install)")


# A handful of XML chunks are driver-bookkeeping rather than engine pipeline
# state, and their textual ORDER inside the capture is not stable run-to-run
# (the OpenGL driver schedules adjacent persistent-mapped-buffer writes in
# whatever order suits it). We drop them entirely from the diff input -- if
# the engine binds a different program or texture, that diff fires from the
# surrounding glBindTexture / glUseProgram / glDraw* chunks, which ARE order-
# stable.
DRIVER_BOOKKEEPING_BLOCKS = [
    "Internal::Coherent Mapped Memory Write",
]


def load_normalized_lines(path: Path) -> list[str]:
    out = []
    skip_depth = 0
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = normalize_text(raw)
            if skip_depth > 0:
                # Inside a skipped <chunk> ... </chunk> block; consume until
                # we see the closing tag at the same indent.
                if "</chunk>" in raw:
                    skip_depth -= 1
                continue
            if any(b in raw for b in DRIVER_BOOKKEEPING_BLOCKS):
                skip_depth = 1
                continue
            out.append(line)
    return out


def _disk_guard_advisory(path, need_gb: float, label: str) -> None:
    """DISK-GUARD-1 advisory probe. Prints the guard's verdict, never blocks.

    Developer capture tools warn rather than abort: a lost capture is cheap to
    redo, and blocking an investigation on a threshold is its own failure mode.
    The two sites that DO abort are build_release.sh and deploy_payload.py,
    where a truncated output is expensive and hard to notice.
    """
    guard = Path(__file__).resolve().parent / "check-disk-space.py"
    if not guard.is_file():
        return
    try:
        # background-launch: allow disk-guard advisory prints its verdict to the caller's console; inherits it (no new window when the parent has one)
        subprocess.run([sys.executable, str(guard), "--path", str(path),
                        "--need-gb", str(need_gb), "--label", label],
                       timeout=60)
    except Exception as e:  # noqa: BLE001 -- advisory only
        print(f"[rdoc_capture] disk guard skipped ({e})", file=sys.stderr)


def expand_out_template(raw: str, mission: str, frame: int) -> str:
    """Expand {mission}/{frame} in --out, and REFUSE any other placeholder.

    CAPTURE-FILENAME-1 (2026-07-29). A 1,010,693,435-byte capture was found on
    disk named `vk_airport_{frame}_frame41.rdc`. `--out` was passed straight
    through to RenderDoc's SetCaptureFilePathTemplate, so a caller who wrote
    `--out .../vk_airport_{frame}` (reasonably assuming a placeholder, and/or
    having built the string with a plain `r"..."` instead of an f-string) got a
    literal `{frame}` baked into a ~1 GB filename. Nothing complained.

    Two changes, because expansion alone is not enough:
      1. {mission} and {frame} now actually expand, so the natural assumption
         is correct rather than silently wrong.
      2. Any *surviving* brace is a hard error. A typo'd or unsupported
         placeholder must not be discoverable only by finding a gigabyte of
         mis-named evidence weeks later.
    """
    expanded = raw.replace("{mission}", str(mission)).replace(
        "{frame}", str(frame))
    if "{" in expanded or "}" in expanded:
        raise SystemExit(
            f"[rdoc_capture] --out contains an unexpanded placeholder: "
            f"{expanded!r}\n"
            f"  supported placeholders: {{mission}}, {{frame}}\n"
            f"  (refusing to write a ~1 GB capture under a mis-named path -- "
            f"see CAPTURE-FILENAME-1)")
    return expanded


def run_capture(args) -> Path:
    rdc_path = Path(expand_out_template(
        args.out, args.mission, args.frame)).resolve()
    rdc_path.parent.mkdir(parents=True, exist_ok=True)

    # A single .rdc can run ~1 GB, and the XML conversion writes another large
    # file alongside it. Advisory here -- a capture is cheap to redo and
    # this is a developer tool, so it warns rather than blocks.
    _disk_guard_advisory(rdc_path.parent, need_gb=3,
                         label="RenderDoc capture (.rdc + XML)")

    # Path template the engine hands to SetCaptureFilePathTemplate must NOT
    # include the .rdc suffix; RenderDoc appends it (potentially with a
    # _frameN segment). Strip the suffix here.
    tpl = rdc_path.with_suffix("")

    env = os.environ.copy()
    env["MC2_RDC_CAPTURE_FRAME"] = str(args.frame)
    env["MC2_RDC_CAPTURE_PATH"]  = str(tpl)
    env["MC2_RDC_EXIT_AFTER"]    = "1"
    # Engine-side smoke harness auto-loads the requested mission and supplies
    # mission_ready / heartbeat events so the engine actually advances past
    # the menu without manual driving. MC2_SMOKE_MODE arms the parser; the
    # CLI flags below provide the values.
    env["MC2_SMOKE_MODE"] = "1"
    env.setdefault("MC2_SMOKE_SEED", "0xC0FFEE")

    rdoc_cmd = find_renderdoccmd(args.renderdoccmd)
    exe = (Path(args.exe) if args.exe else DEPLOY / "mc2.exe").resolve()
    if not exe.is_file():
        raise SystemExit(f"mc2.exe missing at {exe}; deploy before capture")

    cmd = [str(rdoc_cmd), "capture", "-w", str(exe),
           "--mission", args.mission,
           "--duration", "120",
           "--smoke-active"]
    print(f"[rdoc_capture] launching: {' '.join(cmd)}", flush=True)
    print(f"[rdoc_capture]   frame={args.frame} mission={args.mission}",
          flush=True)
    print(f"[rdoc_capture]   template={tpl}", flush=True)

    # NOTE: our STARTUPINFO stops at renderdoccmd -- it spawns mc2.exe itself,
    # so nCmdShow never reaches the engine window. The ENV is what covers it:
    # renderdoccmd passes its environment through, so nofocus's
    # MC2_NO_CURSOR_GRAB=1 reaches the engine and suppresses SDL activation
    # and cursor grabbing.
    proc = nofocus.run(cmd, env=env, cwd=str(DEPLOY))
    # renderdoccmd capture returns the child's exit code. The engine exits 0
    # on success after MC2_RDC_EXIT_AFTER fires.
    if proc.returncode not in (0, 4):
        print(f"[rdoc_capture] child exited {proc.returncode}", file=sys.stderr)

    # RenderDoc may append _frame0001.rdc; resolve the actual file.
    produced = sorted(rdc_path.parent.glob(rdc_path.stem + "*.rdc"))
    if not produced:
        raise SystemExit(f"no .rdc produced (looked for {rdc_path.stem}*.rdc)")
    final = produced[-1]
    print(f"[rdoc_capture] capture file: {final}", flush=True)
    return final


def convert_capture(rdc: Path, out_xml: Path, rdoc_cmd: Path) -> Path:
    """Convert .rdc to RenderDoc's XML capture export.

    RenderDoc v1.7 lists xml, zip.xml, chrome.json, rdc as the only
    convert-format options (`renderdoccmd convert --help`). The XML form is
    a structured dump of every API call + pipeline-state binding for every
    EID -- exactly what the user's existing manual pipeline-state export
    workflow produces, just for the whole frame.
    """
    out_xml.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(rdoc_cmd), "convert",
           "-f", str(rdc), "-o", str(out_xml), "-c", "xml"]
    # background-launch: allow renderdoccmd convert is a console file converter, not the game; windowless via hidden_kwargs
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          **nofocus.hidden_kwargs())
    if proc.returncode != 0 or not out_xml.is_file():
        raise SystemExit(
            f"renderdoccmd convert failed (rc={proc.returncode}): "
            f"{proc.stderr or proc.stdout}"
        )
    print(f"[rdoc_capture] converted: {out_xml}", flush=True)
    return out_xml


def diff_against(baseline: Path, current: Path) -> int:
    if not baseline.is_file():
        print(f"[rdoc_capture] no baseline at {baseline} -- seeding it",
              flush=True)
        shutil.copy2(current, baseline)
        return 0
    a = load_normalized_lines(baseline)
    b = load_normalized_lines(current)
    if a == b:
        print("[rdoc_capture] diff vs baseline: NO CHANGE", flush=True)
        return 0
    diff = list(difflib.unified_diff(a, b, str(baseline), str(current), n=2))
    print(f"[rdoc_capture] diff vs baseline: DRIFT ({len(diff)} lines)",
          flush=True)
    for line in diff[:200]:
        print(line.rstrip())
    if len(diff) > 200:
        print(f"... ({len(diff) - 200} more lines)")
    return 1


def cmd_capture(args) -> int:
    rdc = run_capture(args)
    if args.skip_convert:
        return 0
    rdoc_cmd = find_renderdoccmd(args.renderdoccmd)
    out_xml = Path(args.xml) if args.xml else rdc.with_suffix(".xml")
    convert_capture(rdc, out_xml, rdoc_cmd)
    if args.baseline:
        return diff_against(Path(args.baseline), out_xml)
    return 0


def cmd_diff_self(args) -> int:
    """Capture twice and diff the two JSON outputs -- the harness sanity test."""
    tmpdir = Path(tempfile.mkdtemp(prefix="rdoc_self_"))
    try:
        a_rdc = run_capture(argparse.Namespace(**{
            **vars(args),
            "out": str(tmpdir / "a.rdc"),
        }))
        b_rdc = run_capture(argparse.Namespace(**{
            **vars(args),
            "out": str(tmpdir / "b.rdc"),
        }))
        rdoc_cmd = find_renderdoccmd(args.renderdoccmd)
        a_xml = convert_capture(a_rdc, tmpdir / "a.xml", rdoc_cmd)
        b_xml = convert_capture(b_rdc, tmpdir / "b.xml", rdoc_cmd)
        return diff_against(a_xml, b_xml)
    finally:
        # Leave tmpdir on failure for inspection; clean on success.
        pass


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    cap = sub.add_parser("capture", help="capture one frame; optional diff")
    cap.add_argument("--mission", default="mc2_01")
    cap.add_argument("--frame", type=int, default=120,
                     help="frames after intro-complete latch")
    cap.add_argument("--out", required=True,
                     help="output .rdc path (suffix optional). Supports the "
                          "{mission} and {frame} placeholders; any other "
                          "brace is a hard error (CAPTURE-FILENAME-1).")
    cap.add_argument("--xml", help="output pipeline-state XML path (default: alongside .rdc)")
    cap.add_argument("--baseline", help="baseline XML to diff against")
    cap.add_argument("--skip-convert", action="store_true",
                     help="produce only .rdc; skip JSON conversion")
    cap.add_argument("--renderdoccmd",
                      help="explicit path to renderdoccmd.exe")
    cap.add_argument("--exe",
                     help="explicit mc2.exe path (default: canonical v0.4 deploy)")
    cap.set_defaults(func=cmd_capture)

    ds = sub.add_parser("diff-self",
                        help="capture twice and diff -- harness sanity test")
    ds.add_argument("--mission", default="mc2_01")
    ds.add_argument("--frame", type=int, default=120)
    ds.add_argument("--out", default="tests/smoke/captures/diff_self.rdc",
                    help="ignored; templates are auto-generated under tmpdir")
    ds.add_argument("--renderdoccmd")
    ds.add_argument("--exe",
                    help="explicit mc2.exe path (default: canonical v0.4 deploy)")
    ds.set_defaults(func=cmd_diff_self)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
