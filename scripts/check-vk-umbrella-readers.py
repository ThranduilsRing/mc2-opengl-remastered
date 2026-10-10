#!/usr/bin/env python3
"""check-vk-umbrella-readers.py -- the two rules that keep the VK umbrella reader model honest.

The 25 VK umbrella members are Tri rows with inherits("MC2_VK_SHARED_GBUFFER"). Their only reader is
mc2vkgate::armed<> (GameOS/gameos/vk_shared_gbuffer_gate.h), which wraps gate::effective<> and adds the
session fail-soft veto the static Gates library cannot see.

  R1  NO DIRECT effective<>. Any `effective<"MC2_VK_...">` outside the adapter header skips the veto, so a
      mid-session fail-soft would arm an island the GL present can never show.
  R2  PER-CALL READERS STAY UNCACHED. The sites below re-read armed<> on every call so that a mid-session
      fail-soft disarms them (VK-FAILSOFT-DISARM-1, HUD-GATE-HOIST-1). Wrapping one in a function-local
      static (or a static lambda) would freeze the first answer. Region gates that are resolved once per
      process are not on this list: they cache by design.

USAGE
  py -3 scripts/check-vk-umbrella-readers.py [--quiet]
  py -3 scripts/check-vk-umbrella-readers.py --selftest
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ADAPTER = "GameOS/gameos/vk_shared_gbuffer_gate.h"
SCAN_DIRS = ("code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld", "GameAdapters",
             "HostServices", "editor", "gui")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")

# (file, gate): every `armed<"gate">` (or, for "vfxArmed", every vfxArmed() call) in the file is a per-call read.
PER_CALL = (
    ("GameOS/gameos/engine_present_vk.cpp", "MC2_VK_PRESENT_DIRECT_FBO"),
    ("GameOS/gameos/engine_present_vk.cpp", "MC2_VK_PRESENT_TAIL_HOOK"),
    ("GameOS/gameos/engine_present_vk.cpp", "MC2_VK_HUD_COLOR_PROOF"),
    ("GameOS/gameos/engine_present_vk.cpp", "MC2_VK_POST_COMPOSITE"),
    ("GameOS/gameos/gos_postprocess.cpp", "MC2_VK_SHARED_SCENE_DEPTH"),
    ("GameOS/gameos/vulkan_vfx_island.cpp", "MC2_VK_VFX"),
    ("GameOS/gameos/vulkan_water_island.cpp", "MC2_VK_WATER_TIER1"),
    ("GameOS/gameos/gameos_graphics.cpp", "MC2_VK_VFX"),
    ("GameOS/gameos/gameos_graphics.cpp", "MC2_VK_HUD"),
    ("GameOS/gameos/gameos_graphics.cpp", "MC2_VK_HUD_SKIP_GL"),
    # the translation units outside the VK islands call the out-of-line vfxArmed() (vk_umbrella_light.h)
    ("code/gamecam.cpp", "vfxArmed"),
    ("code/objmgr.cpp", "vfxArmed"),
    ("code/weaponbolt_render.cpp", "vfxArmed"),
)

_COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def blank_comments(src: str) -> str:
    """Comments replaced by spaces of the same length (newlines kept), so offsets and line numbers hold."""
    return _COMMENT_RE.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src)


def statement_start(code: str, pos: int) -> int:
    """Offset just after the previous ';' or '}' before `pos`: the start of the statement holding `pos`
    ('{' is not a boundary, so the body of a lambda belongs to the statement that declares it)."""
    return max(code.rfind(';', 0, pos), code.rfind('}', 0, pos)) + 1


_LAMBDA_OPEN = re.compile(r'\[[^\]]*\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?(?:->\s*[\w:]+\s*)?\{')


def is_static_cached(stmt: str) -> bool:
    """Is the read at the end of `stmt` the initializer of a `static` declaration (possibly through a lambda)?
    A `static` function header earlier in the same segment ends in `{` without a lambda introducer, so the
    reads in its body are not cached by it."""
    i = stmt.rfind('static')
    if i < 0 or not re.match(r'static\b', stmt[i:]):
        return False
    tail = _LAMBDA_OPEN.sub('', stmt[i:])
    return '{' not in tail and '=' in tail


def check_direct_effective(texts: dict[str, str]) -> list[str]:
    errs = []
    rx = re.compile(r'\beffective\s*<\s*"MC2_VK_')
    for rel, src in texts.items():
        if rel == ADAPTER:
            continue
        code = blank_comments(src)
        for m in rx.finditer(code):
            errs.append("R1 %s:%d gate::effective<\"MC2_VK_...\"> outside %s skips the fail-soft veto; use "
                        "mc2vkgate::armed<>" % (rel, code.count('\n', 0, m.start()) + 1, ADAPTER))
    return errs


def check_per_call(texts: dict[str, str]) -> list[str]:
    errs = []
    for rel, gate in PER_CALL:
        src = texts.get(rel)
        if src is None:
            errs.append("R2 %s is missing: the per-call reader list is stale" % rel)
            continue
        code = blank_comments(src)
        if gate == "vfxArmed":
            rx = re.compile(r'\bvfxArmed\s*\(')
        else:
            rx = re.compile(r'\barmed\s*<\s*"%s"\s*>' % re.escape(gate))
        found = list(rx.finditer(code))
        if not found:
            errs.append("R2 %s no longer reads %s: update PER_CALL (or this reader went blind)" % (rel, gate))
        for m in found:
            stmt = code[statement_start(code, m.start()):m.start()]
            if is_static_cached(stmt):
                errs.append("R2 %s:%d armed<\"%s\"> is cached in a static: a mid-session fail-soft would never "
                            "disarm it" % (rel, code.count('\n', 0, m.start()) + 1, gate))
    return errs


def load(root: Path) -> dict[str, str]:
    out = {}
    for d in SCAN_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.suffix in EXTS and p.is_file():
                rel = p.relative_to(root).as_posix()
                try:
                    out[rel] = p.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    pass
    return out


def check(root: Path) -> list[str]:
    texts = load(root)
    return check_direct_effective(texts) + check_per_call(texts)


def selftest() -> int:
    rc = 0
    live = check(ROOT)
    if live:
        print("SELF-TEST ABORTED: the live tree already fails:")
        for e in live:
            print("  " + e)
        return 1
    cases = [
        ("R1 direct effective<> is caught",
         {"a.cpp": 'bool f() { return mc2::gate::effective<"MC2_VK_VFX">(false); }'}, check_direct_effective, True),
        ("R1 the adapter itself is allowed",
         {ADAPTER: 'return gate::effective<n>(g_mc2VkSessionForcedGL); // effective<"MC2_VK_X">'}, check_direct_effective, False),
        ("R1 a comment is not a call",
         {"a.cpp": '// effective<"MC2_VK_VFX">(false)\nint x;'}, check_direct_effective, False),
        ("R2 a static cache is caught",
         {"code/gamecam.cpp": 'bool f() { static const bool s = mc2vkgate::vfxArmed(); return s; }'},
         lambda t: [e for e in check_per_call(t) if "cached in a static" in e], True),
        ("R2 a static lambda is caught",
         {"code/gamecam.cpp": 'bool f() { static const bool s = []() { return mc2vkgate::vfxArmed(); }(); return s; }'},
         lambda t: [e for e in check_per_call(t) if "cached in a static" in e], True),
        ("R2 a per-call read passes",
         {"code/gamecam.cpp": 'void g() {} bool f() { const bool s = mc2vkgate::vfxArmed(); return s; }'},
         lambda t: [e for e in check_per_call(t) if "cached in a static" in e], False),
    ]
    for name, texts, fn, expect in cases:
        got = bool(fn(texts))
        ok = got == expect
        print("  [%s] %s" % ("OK  " if ok else "FAIL", name))
        rc |= not ok
    print("[vk-umbrella-readers] self-test " + ("PASSED" if rc == 0 else "FAILED"))
    return int(rc)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    errs = check(ROOT)
    if not a.quiet or errs:
        print("[vk-umbrella-readers] %d per-call reader group(s) checked" % len(PER_CALL))
    for e in errs:
        print("  FAIL " + e)
    if not errs and not a.quiet:
        print("[vk-umbrella-readers] OK")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
