#!/usr/bin/env python3
"""check-vk-umbrella-readers.py -- the two rules that keep the VK umbrella reader model honest.

The 25 VK umbrella members are Tri rows with inherits("MC2_VK_SHARED_GBUFFER"). Their only reader is
mc2vkgate::armed<> (GameOS/gameos/vk_shared_gbuffer_gate.h), which wraps gate::effective<> and adds the
session fail-soft veto the static Gates library cannot see. The umbrella itself is read by one function,
mc2vkgate::umbrellaOn() in mclib/vk_umbrella_gate.cpp.

  R1  NO BYPASS. Any `effective<"MC2_VK_...">` outside the adapter header, or any direct read of
      MC2_VK_SHARED_GBUFFER outside mclib/vk_umbrella_gate.cpp, skips the veto, so a mid-session fail-soft
      would arm an island the GL present can never show.
  R2  PER-CALL READERS STAY UNCACHED. The sites in PER_CALL re-read armed<> (or vfxArmed()/umbrellaOn()) on
      every call so that a mid-session fail-soft disarms them (VK-FAILSOFT-DISARM-1, HUD-GATE-HOIST-1). The
      answer must not be frozen by
        - a `static` declaration initialized from the read (also through a static lambda),
        - an assignment to a name declared `static` in an enclosing function, or declared at file or
          namespace scope (the `static int state = -1; if (state == -1) state = armed<>();` idiom),
        - an assignment to a member (`m_x = ...`, `this->x = ...`, `obj.x = ...`).
      Region gates that are resolved once per process (the island gate functions) are not on this list: they
      cache by design.

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
UMBRELLA_READER = "mclib/vk_umbrella_gate.cpp"
SCAN_DIRS = ("code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld", "GameAdapters",
             "HostServices", "editor", "gui")
EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")

# (file, key). key is a gate name (every `armed<"gate">` in the file is a per-call read), "vfxArmed" or
# "umbrellaOn" (every call of that function is), or "nostatic" (the file must contain no `static` at all).
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
    # the terrain submit path fills the island's per-frame parameter block
    ("GameOS/gameos/gos_terrain_lod_chunk.cpp", "MC2_VK_TERRAIN_LIGHTING"),
    ("GameOS/gameos/gos_terrain_lod_chunk.cpp", "MC2_VK_TERRAIN_DYNSHADOW"),
    ("GameOS/gameos/gos_terrain_lod_chunk.cpp", "MC2_VK_TERRAIN_CSM"),
    ("GameOS/gameos/gos_terrain_lod_chunk.cpp", "MC2_VK_TERRAIN_DETAIL"),
    # the translation units outside the VK islands call the out-of-line functions (vk_umbrella_light.h)
    ("code/gamecam.cpp", "vfxArmed"),
    ("code/objmgr.cpp", "vfxArmed"),
    ("code/weaponbolt_render.cpp", "vfxArmed"),
    ("mclib/appear.cpp", "umbrellaOn"),
    # the out-of-line functions themselves hold no state
    (UMBRELLA_READER, "MC2_VK_VFX"),
    (UMBRELLA_READER, "nostatic"),
)

_COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
_STRING_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')
_KEYWORDS = {"else", "return", "do", "case", "throw", "goto"}


def blank_comments(src: str) -> str:
    """Comments replaced by spaces of the same length (newlines kept), so offsets and line numbers hold.
    String and character literals are skipped over, so a "/*" or "//" inside one starts no comment."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        two = src[i:i + 2]
        if two == '//':
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif two == '/*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def blank_strings(code: str) -> str:
    """String and character literals blanked (quotes kept), so braces inside them are not structure."""
    return _STRING_RE.sub(lambda m: m.group(0)[0] + ' ' * (len(m.group(0)) - 2) + m.group(0)[-1], code)


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


# ---------------------------------------------------------------------------------------------------------
# Scope analysis for "assignment to a cached name"
# ---------------------------------------------------------------------------------------------------------

def blocks_of(bcode: str) -> list:
    """[(open, close)] for every balanced {...} of `bcode` (strings and comments already blanked)."""
    out, stack = [], []
    for i, c in enumerate(bcode):
        if c == '{':
            stack.append(i)
        elif c == '}' and stack:
            out.append((stack.pop(), i))
    return out


def direct_text(bcode: str, start: int, end: int, transparent: frozenset = frozenset()) -> str:
    """The text of bcode[start:end] outside every nested {...}, except that a block opened at an index in
    `transparent` (a namespace) keeps its contents: its declarations are at namespace scope."""
    out, hidden, stack = [], 0, []
    for i in range(start, end):
        c = bcode[i]
        if c == '{':
            opaque = i not in transparent
            stack.append(opaque)
            hidden += opaque
            out.append(' ')
        elif c == '}':
            if stack and stack.pop():
                hidden -= 1
            out.append(' ')
        elif hidden == 0:
            out.append(c)
        else:
            out.append(' ' if c != '\n' else '\n')
    return ''.join(out)


def is_namespace_block(bcode: str, open_idx: int) -> bool:
    head_start = max(bcode.rfind(';', 0, open_idx), bcode.rfind('}', 0, open_idx), bcode.rfind('{', 0, open_idx)) + 1
    return re.search(r'\bnamespace\b', bcode[head_start:open_idx]) is not None


def last_assignment_target(stmt: str):
    """(target, is_declaration) of the assignment whose right-hand side runs to the end of `stmt`, or None."""
    m = re.search(r'([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*=(?!=)[^=;]*$', stmt)
    if not m:
        return None
    target = m.group(1)
    before = stmt[:m.start(1)]
    w = re.search(r'([A-Za-z_]\w*)\s*[*&]*\s*$', before)
    # `const bool on = ...`, `int state = ...`: a declaration of a local (a `static` one is caught separately)
    decl = bool(w) and w.group(1) not in _KEYWORDS and not re.search(r'[)\]]\s*$', before) and '->' not in target and '.' not in target
    return target, decl


def assigns_cached_name(bcode: str, blocks: list, pos: int, stmt: str):
    """Reason string when the read at `pos` is assigned to a name that outlives the call, else None."""
    t = last_assignment_target(stmt)
    if not t:
        return None
    target, decl = t
    if decl:
        return None
    if '->' in target or '.' in target or target.startswith('m_'):
        return "assigned to the member %s" % re.sub(r'\s+', '', target)
    name = target
    transparent = frozenset(b[0] for b in blocks if is_namespace_block(bcode, b[0]))
    enclosing = sorted((b for b in blocks if b[0] < pos < b[1]), key=lambda b: b[0], reverse=True)
    for (o, c) in enclosing + [(-1, len(bcode))]:
        scope = direct_text(bcode, o + 1, min(c, pos), transparent)
        file_or_ns = o == -1 or is_namespace_block(bcode, o)
        if file_or_ns:
            if re.search(r'[\w>*&]\s+%s\s*(?:=(?!=)|;|\[|\{)' % re.escape(name), scope):
                return "assigned to %s, declared at file or namespace scope" % name
        elif re.search(r'\bstatic\b[^;(){}=]*?\b%s\s*(?:=(?!=)|;|\[|\{)' % re.escape(name), scope):
            return "assigned to %s, a function-local static" % name
    return None


def line_of(code: str, pos: int) -> int:
    return code.count('\n', 0, pos) + 1


# ---------------------------------------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------------------------------------

_EFFECTIVE_RE = re.compile(r'\beffective\s*<\s*"MC2_VK_')
_UMBRELLA_DIRECT_RE = re.compile(r'\b(?:get|state|isSet)\s*<\s*"MC2_VK_SHARED_GBUFFER"')


def check_direct_effective(texts: dict[str, str]) -> list[str]:
    errs = []
    for rel, src in texts.items():
        code = blank_comments(src)
        if rel != ADAPTER:
            for m in _EFFECTIVE_RE.finditer(code):
                errs.append("R1 %s:%d gate::effective<\"MC2_VK_...\"> outside %s skips the fail-soft veto; use "
                            "mc2vkgate::armed<>" % (rel, line_of(code, m.start()), ADAPTER))
        if rel != UMBRELLA_READER:
            for m in _UMBRELLA_DIRECT_RE.finditer(code):
                errs.append("R1 %s:%d MC2_VK_SHARED_GBUFFER is read directly outside %s; use "
                            "mc2vkgate::umbrellaOn() (it adds the fail-soft veto)"
                            % (rel, line_of(code, m.start()), UMBRELLA_READER))
    return errs


def _pattern_for(key: str):
    if key == "vfxArmed":
        return re.compile(r'\bvfxArmed\s*\(')
    if key == "umbrellaOn":
        return re.compile(r'\bumbrellaOn\s*\(')
    return re.compile(r'\barmed\s*<\s*"%s"\s*>' % re.escape(key))


def check_per_call(texts: dict[str, str]) -> list[str]:
    errs = []
    for rel, key in PER_CALL:
        src = texts.get(rel)
        if src is None:
            errs.append("R2 %s is missing: the per-call reader list is stale" % rel)
            continue
        code = blank_comments(src)
        if key == "nostatic":
            for m in re.finditer(r'\bstatic\b', blank_strings(code)):
                errs.append("R2 %s:%d `static` in a file whose functions must hold no state (cached answers would "
                            "survive a fail-soft)" % (rel, line_of(code, m.start())))
            continue
        found = list(_pattern_for(key).finditer(code))
        if not found:
            errs.append("R2 %s no longer reads %s: update PER_CALL (or this reader went blind)" % (rel, key))
            continue
        bcode = blank_strings(code)
        blocks = blocks_of(bcode)
        for m in found:
            stmt = code[statement_start(code, m.start()):m.start()]
            if is_static_cached(stmt):
                errs.append("R2 %s:%d %s is cached in a static: a mid-session fail-soft would never disarm it"
                            % (rel, line_of(code, m.start()), key))
                continue
            why = assigns_cached_name(bcode, blocks, m.start(), stmt)
            if why:
                errs.append("R2 %s:%d %s is cached (%s): a mid-session fail-soft would never disarm it"
                            % (rel, line_of(code, m.start()), key, why))
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

    def cached(t):
        return [e for e in check_per_call(t) if " cached" in e or "is cached" in e]

    gc = "code/gamecam.cpp"
    cases = [
        ("R1 direct effective<> is caught",
         {"a.cpp": 'bool f() { return mc2::gate::effective<"MC2_VK_VFX">(false); }'}, check_direct_effective, True),
        ("R1 the adapter itself is allowed",
         {ADAPTER: 'return gate::effective<n>(g_mc2VkSessionForcedGL); // effective<"MC2_VK_X">'}, check_direct_effective, False),
        ("R1 a comment is not a call",
         {"a.cpp": '// effective<"MC2_VK_VFX">(false)\nint x;'}, check_direct_effective, False),
        ("R1 a direct read of the umbrella is caught",
         {"a.cpp": 'bool f() { return mc2::gate::get<"MC2_VK_SHARED_GBUFFER">(); }'}, check_direct_effective, True),
        ("R1 the umbrella reader may read it",
         {UMBRELLA_READER: 'bool umbrellaOn() { return gate::get<"MC2_VK_SHARED_GBUFFER">(); }'}, check_direct_effective, False),
        ("R2 a static cache is caught",
         {gc: 'bool f() { static const bool s = mc2vkgate::vfxArmed(); return s; }'}, cached, True),
        ("R2 a static lambda is caught",
         {gc: 'bool f() { static const bool s = []() { return mc2vkgate::vfxArmed(); }(); return s; }'}, cached, True),
        ("R2 the static state idiom is caught",
         {gc: 'bool f() { static int state = -1; if (state == -1) state = mc2vkgate::vfxArmed() ? 1 : 0; '
              'return state == 1; }'}, cached, True),
        ("R2 the static state idiom inside a block is caught",
         {gc: 'bool f() { static int state = -1; if (state == -1) { state = mc2vkgate::vfxArmed() ? 1 : 0; } '
              'return state == 1; }'}, cached, True),
        ("R2 a namespace-scope static is caught",
         {gc: 'static bool g_on = false; void init() { g_on = mc2vkgate::vfxArmed(); }'}, cached, True),
        ("R2 an anonymous-namespace variable is caught",
         {gc: 'namespace { bool g_on; } void init() { g_on = mc2vkgate::vfxArmed(); }'}, cached, True),
        ("R2 a member cache is caught",
         {gc: 'void C::f() { m_on = mc2vkgate::vfxArmed(); }'}, cached, True),
        ("R2 a this-> member cache is caught",
         {gc: 'void C::f() { this->on = mc2vkgate::vfxArmed(); }'}, cached, True),
        ("R2 an object member cache is caught",
         {gc: 'void f(S& s) { s.on = mc2vkgate::vfxArmed(); }'}, cached, True),
        ("R2 a per-call read passes",
         {gc: 'void g() {} bool f() { const bool s = mc2vkgate::vfxArmed(); return s; }'}, cached, False),
        ("R2 a local assigned from the read passes",
         {gc: 'bool f() { bool on = false; on = mc2vkgate::vfxArmed(); return on; }'}, cached, False),
        ("R2 a local that shadows a file-scope static passes",
         {gc: 'static const bool s_on = true; bool f() { const bool s_on = mc2vkgate::vfxArmed(); return s_on; }'},
         cached, False),
        ("R2 an unrelated static in the function passes",
         {gc: 'bool f() { static int calls = 0; ++calls; const bool x = mc2vkgate::vfxArmed(); return x; }'},
         cached, False),
        ("R2 a file that must hold no state is caught",
         {UMBRELLA_READER: 'bool vfxArmed() { static bool s = armed<"MC2_VK_VFX">(); return s; }'},
         lambda t: [e for e in check_per_call(t) if "must hold no state" in e], True),
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
