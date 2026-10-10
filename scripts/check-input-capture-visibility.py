#!/usr/bin/env python3
"""CURSOR-GRAB-FIX-1 -- no input capture on a code path where the window is not
user-visible.

WHY THIS EXISTS
---------------
The user reported FIVE separate times that agent-initiated launches steal his
mouse, keyboard and focus while he is working. The response was a mechanical
block on all engine launches, which then bottlenecked a full day of unverified
work. The cause turned out to be an ordinary bug, not anything inherent:

  MC2_WINDOW_OFFSCREEN=1 created the SDL window at (-32000,-32000), SHOWED it
  (gos_render.cpp, the single SDL_ShowWindow), and then ran
  create_window -> set_mouse_grab(true) -> apply_mouse_grab(), whose only
  suppression test was gos_CursorControlSuppressed() -- which reads
  MC2_NO_CURSOR_GRAB / MC2_SMOKE_MODE and knows NOTHING about
  MC2_WINDOW_OFFSCREEN. So Win32 ClipCursor confined the user's PHYSICAL
  pointer to a rectangle sitting on no monitor. The offscreen mode that existed
  to keep agent runs off his desktop was itself taking his cursor.

Two properties made it survive:

  * the SDL_WINDOWEVENT_FOCUS_GAINED handler in gameosmain.cpp re-arms the grab
    unconditionally, so any one-shot suppression at startup is undone the first
    time the window gains focus; and
  * "offscreen" and "may not touch the cursor" were separate parses of separate
    env vars in separate translation units, free to disagree.

WHAT THIS CHECKS
----------------
RULE 1 -- GUARDED CHOKEPOINT. Each function listed in GUARDED_SITES must exist,
  and inside its own body the guard call must appear BEFORE the first
  input-capturing call. This is the arm that fails if someone restores the
  unconditional grab.

RULE 2 -- NO UNLISTED CAPTURE SITE. Any engine source that calls an
  input-capturing or foreground-stealing API must be named in POLICY_FILES.
  Adding a new grab/clip/warp somewhere else fails here. Comments and string
  literals are stripped first, so prose naming an API is not a hit.

RULE 3 -- NO STALE POLICY ENTRY. Every file in POLICY_FILES and every function
  in GUARDED_SITES must still exist and still contain what it claims. A check
  that goes green because someone renamed a function is not a check, so the
  absence of a named symbol is a FAILURE, never a pass.

RULE 4 -- ONE PARSE OF MC2_WINDOW_OFFSCREEN. Exactly one read of that
  variable (a getenv, or a typed gate get/is/isSet/str/raw of "MC2_WINDOW_OFFSCREEN") may exist in engine C++. A second parse is how the two halves of the
  policy drifted apart the first time.

Scope is the ENGINE only. `editor/` is excluded on purpose: the MFC editor is a
user-launched desktop application whose SetCapture()/ReleaseCapture() calls are
ordinary Win32 mouse tracking inside a window the user opened, and its SDL child
surface explicitly RELEASES every grab (EditorGosRender.cpp). `tools/` is
excluded for the same reason -- the launcher is a dialog the user double-clicked.

The harness/Python side of the same policy is covered by
scripts/check-background-launch.py; this file is its engine-side counterpart.

Usage:
  py -3 scripts/check-input-capture-visibility.py [--quiet]
  py -3 scripts/check-input-capture-visibility.py --selftest
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

#: Engine source roots. Deliberately broad -- a new capture call in any of these
#: must declare itself. See the module docstring for why editor/ and tools/ are
#: not here.
SCAN_DIRS = ("GameOS", "code", "mclib", "GuiRuntime", "RenderCore",
             "RenderWorld", "GameAdapters", "HostServices", "EditorBridge",
             "Viewer", "ARM")

SCAN_SUFFIXES = (".cpp", ".h", ".hpp", ".c", ".cc")

#: Every way this process can take the user's pointer or force itself in front
#: of whatever he is doing. Matched as a call (identifier followed by "(") after
#: comment/string stripping, except HWND_TOPMOST which is a constant.
CAPTURE_APIS = (
    # Win32 pointer confinement / movement
    "ClipCursor", "SetCapture", "SetCursorPos",
    # Win32 foreground theft
    "SetForegroundWindow", "SetActiveWindow", "BringWindowToTop",
    "SwitchToThisWindow",
    # SDL pointer confinement / movement / focus
    "SDL_SetWindowMouseGrab", "SDL_SetWindowGrab", "SDL_SetWindowKeyboardGrab",
    "SDL_SetRelativeMouseMode", "SDL_CaptureMouse", "SDL_SetWindowMouseRect",
    "SDL_WarpMouseInWindow", "SDL_WarpMouseGlobal",
    "SDL_RaiseWindow", "SDL_SetWindowInputFocus", "SDL_FlashWindow",
)

#: Not a call -- a SetWindowPos z-order constant that puts a window above
#: everything the user owns. Matched as a bare token.
CAPTURE_TOKENS = ("HWND_TOPMOST",)

#: file -> why it is allowed to contain a capture API at all. Each of these must
#: also have at least one GUARDED_SITES row proving the call is gated.
POLICY_FILES = {
    "GameOS/gameos/gos_render.cpp":
        "THE chokepoint. apply_mouse_grab() is the single place the engine "
        "grabs/clips the pointer; every arm path funnels through it.",
    "GameOS/gameos/gameos_input.cpp":
        "gos_SetMousePosition() warps the OS pointer (the tutorial guiFrozen "
        "path recenters it every frame), and this TU defines the policy itself.",
    "GameOS/gameos/gos_crashbundle.cpp":
        "the crash dialog is HWND_TOPMOST + modal; on an unattended run that is "
        "a focus-stealing box in the middle of the user's work.",
}

#: (file, function, guard-substring, [capture-substrings]) -- the guard must
#: appear at a LOWER offset in the function body than every capture listed.
#:
#: The guard is named by the exact call text, not by "some conditional exists":
#: the pre-fix code was conditional too (on gos_CursorControlSuppressed()) and
#: still shipped the bug, because it tested the wrong condition.
GUARDED_SITES = (
    ("GameOS/gameos/gos_render.cpp", "apply_mouse_grab",
     "gos_CursorCaptureForbidden",
     ("SDL_SetWindowMouseGrab", "ClipCursor(&")),
    ("GameOS/gameos/gameos_input.cpp", "gos_SetMousePosition",
     "gos_CursorCaptureForbidden",
     ("SDL_WarpMouseInWindow",)),
    ("GameOS/gameos/gos_crashbundle.cpp", "show_crash_dialog",
     "g_cfg.unattended",
     ("DialogBoxIndirectParamW",)),
)

#: RULE 3's geometry failsafe: apply_mouse_grab must refuse to clip the pointer
#: to a rectangle on no monitor, independently of every env gate.
FAILSAFE_SITE = ("GameOS/gameos/gos_render.cpp", "apply_mouse_grab",
                 "MONITOR_DEFAULTTONULL", "ClipCursor(&")

#: RULE 4.
OFFSCREEN_VAR = "MC2_WINDOW_OFFSCREEN"
OFFSCREEN_PARSE_OWNER = "GameOS/gameos/gameos_input.cpp"


# ---------------------------------------------------------------------------
# Comment / string stripping
# ---------------------------------------------------------------------------
def strip_cxx(src: str, keep_strings: bool = False) -> str:
    """Blank out comments and (unless keep_strings) string/char literals,
    PRESERVING length and newlines so offsets and line numbers still map back to
    the original.

    keep_strings=True is for RULE 4, which must SEE the env-var name inside its
    string literal while still ignoring any comment that merely names it.

    Length preservation is the point: RULE 1 compares OFFSETS of a guard and a
    capture call, so anything that shortened the text would move them relative
    to each other. Replacing each removed byte with a space (newlines kept) is
    the only transform that cannot do that.
    """
    out = list(src)
    i, n = 0, len(src)
    NORMAL, LINE, BLOCK, STR, CHAR = range(5)
    state = NORMAL

    def blank(k: int) -> None:
        if out[k] != "\n":
            out[k] = " "

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state == NORMAL:
            if c == "/" and nxt == "/":
                state = LINE
                blank(i); blank(i + 1); i += 2; continue
            if c == "/" and nxt == "*":
                state = BLOCK
                blank(i); blank(i + 1); i += 2; continue
            if c == '"':
                state = STR
                if not keep_strings:
                    blank(i)
                i += 1; continue
            if c == "'":
                state = CHAR
                if not keep_strings:
                    blank(i)
                i += 1; continue
            i += 1; continue
        if state == LINE:
            if c == "\n":
                state = NORMAL
                i += 1; continue
            blank(i); i += 1; continue
        if state == BLOCK:
            if c == "*" and nxt == "/":
                blank(i); blank(i + 1); state = NORMAL; i += 2; continue
            blank(i); i += 1; continue
        # STR / CHAR
        if c == "\\":
            if not keep_strings:
                blank(i)
                if i + 1 < n:
                    blank(i + 1)
            i += 2; continue
        if (state == STR and c == '"') or (state == CHAR and c == "'"):
            if not keep_strings:
                blank(i)
            state = NORMAL; i += 1; continue
        if not keep_strings:
            blank(i)
        i += 1; continue
    return "".join(out)


def function_body(code: str, name: str):
    """(start, end) offsets of `name`'s body in ALREADY-STRIPPED code, or None.

    Finds `name` followed by optional space and "(", requires the matching ")"
    to be followed (past whitespace and simple qualifiers) by "{" -- so a CALL
    to the function and a prototype ending in ";" are both skipped and only a
    DEFINITION matches.
    """
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", code):
        # walk to the matching close paren
        depth, j = 0, m.end() - 1
        while j < len(code):
            if code[j] == "(":
                depth += 1
            elif code[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        else:
            continue
        k = j + 1
        while k < len(code) and (code[k].isspace() or code[k] in "&*"):
            k += 1
        # allow trailing qualifiers before the brace (const/noexcept/override)
        tail = re.match(r"(?:const|noexcept|override|final|\s)*", code[k:k + 40])
        k += tail.end() if tail else 0
        if k >= len(code) or code[k] != "{":
            continue
        depth, e = 0, k
        while e < len(code):
            if code[e] == "{":
                depth += 1
            elif code[e] == "}":
                depth -= 1
                if depth == 0:
                    return (k, e)
            e += 1
        return None
    return None


_SOURCE_CACHE: list = []


def iter_sources():
    """Engine sources, enumerated once per process (the tree walk is ~1k files
    and this is called from several rules and from every selftest arm)."""
    if not _SOURCE_CACHE:
        for d in SCAN_DIRS:
            base = ROOT / d
            if not base.is_dir():
                continue
            for p in sorted(base.rglob("*")):
                if p.suffix.lower() not in SCAN_SUFFIXES or not p.is_file():
                    continue
                parts = set(p.parts)
                if "3rdparty" in parts or "build64" in parts or "build" in parts:
                    continue
                _SOURCE_CACHE.append(p)
    return list(_SOURCE_CACHE)


#: Cheap pre-filter over RAW text. strip_cxx() can only DELETE matches, never
#: create one, so a file whose raw bytes contain none of these tokens cannot
#: produce a hit and need not be stripped at all. Purely a speed device -- it
#: changes no verdict, and the selftest's stripper arm still proves that a file
#: which passes this filter via a COMMENT is correctly not counted.
_PREFILTER = re.compile("|".join(
    re.escape(t) for t in (CAPTURE_APIS + CAPTURE_TOKENS + (OFFSCREEN_VAR,))))


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def capture_hits(code: str):
    """[(offset, token)] for every capture API CALL / token in stripped code."""
    hits = []
    for api in CAPTURE_APIS:
        for m in re.finditer(r"\b" + re.escape(api) + r"\s*\(", code):
            hits.append((m.start(), api))
    for tok in CAPTURE_TOKENS:
        for m in re.finditer(r"\b" + re.escape(tok) + r"\b", code):
            hits.append((m.start(), tok))
    return sorted(hits)


# ---------------------------------------------------------------------------
# The check
# ---------------------------------------------------------------------------
def check(read=None):
    """read(relpath) -> str|None. Injectable so --selftest can mutate a copy of
    the tree in memory without ever writing to it."""
    if read is None:
        def read(r):
            p = ROOT / r
            return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None

    problems: list[str] = []
    stripped: dict[str, str] = {}
    nocomment: dict[str, str] = {}

    raw: dict[str, str] = {}

    def get_raw(r):
        if r not in raw:
            raw[r] = read(r)
        return raw[r]

    def interesting(r):
        src = get_raw(r)
        return src is not None and _PREFILTER.search(src) is not None

    def get(r):
        if r not in stripped:
            src = get_raw(r)
            stripped[r] = None if src is None else strip_cxx(src)
        return stripped[r]

    def get_with_strings(r):
        """Comments removed, string literals INTACT. RULE 4 only."""
        if r not in nocomment:
            src = get_raw(r)
            nocomment[r] = None if src is None else strip_cxx(src, keep_strings=True)
        return nocomment[r]

    # ---- RULE 2: no unlisted capture site ---------------------------------
    offenders_by_file: dict[str, list] = {}
    for p in iter_sources():
        r = rel(p)
        if not interesting(r):
            continue
        code = get(r)
        if code is None:
            continue
        hits = capture_hits(code)
        if hits:
            offenders_by_file[r] = hits
        if hits and r not in POLICY_FILES:
            where = ", ".join(sorted({t for _o, t in hits})[:4])
            line = code[:hits[0][0]].count("\n") + 1
            problems.append(
                f"{r}:{line}: calls an input-capturing / foreground-stealing API "
                f"({where}) but is not in check-input-capture-visibility."
                f"POLICY_FILES. Every such call must be provably unreachable "
                f"when the window is not visible to the user -- route it through "
                f"gos_CursorCaptureForbidden() and register the site in "
                f"GUARDED_SITES, or delete it.")

    # ---- RULE 3: no stale policy entry ------------------------------------
    for r, why in sorted(POLICY_FILES.items()):
        if get(r) is None:
            problems.append(
                f"{r}: listed in POLICY_FILES but the file does not exist. A "
                f"stale exception is a standing licence to grab the cursor.")
        elif r not in offenders_by_file:
            problems.append(
                f"{r}: listed in POLICY_FILES ({why}) but no longer contains any "
                f"capture API. If the call moved, move the entry with it; a "
                f"policy row that guards nothing makes the board read green for "
                f"a check that stopped looking.")

    # ---- RULE 1: guarded chokepoint ---------------------------------------
    for r, fn, guard, captures in GUARDED_SITES:
        code = get(r)
        if code is None:
            problems.append(f"{r}: GUARDED_SITES names this file; it is missing.")
            continue
        span = function_body(code, fn)
        if span is None:
            problems.append(
                f"{r}: GUARDED_SITES requires a definition of {fn}() and there "
                f"is none. It was renamed, inlined or deleted -- which is "
                f"exactly the way a guard silently stops guarding. Point the "
                f"row at the new chokepoint (and read it first).")
            continue
        body = code[span[0]:span[1]]
        gpos = body.find(guard)
        if gpos < 0:
            line = code[:span[0]].count("\n") + 1
            problems.append(
                f"{r}:{line}: {fn}() does not call {guard}. This is THE bug "
                f"CURSOR-GRAB-FIX-1 fixed: an input-capturing call reachable on "
                f"a path where the window is not user-visible. MC2_WINDOW_"
                f"OFFSCREEN parks the window at (-32000,-32000) and shows it, so "
                f"an unguarded grab clips the user's physical cursor to a "
                f"rectangle on no monitor.")
            continue
        for cap in captures:
            cpos = body.find(cap)
            if cpos < 0:
                problems.append(
                    f"{r}: {fn}() no longer contains {cap!r}, which GUARDED_SITES "
                    f"claims it guards. Update the row -- do not assume the "
                    f"capture went away.")
            elif cpos < gpos:
                line = code[:span[0] + cpos].count("\n") + 1
                problems.append(
                    f"{r}:{line}: {fn}() reaches {cap} BEFORE its {guard} guard. "
                    f"The guard must dominate every capture in the body.")

    # ---- RULE 3b: the geometry failsafe -----------------------------------
    r, fn, failsafe, cap = FAILSAFE_SITE
    code = get(r)
    span = function_body(code, fn) if code else None
    if span is None:
        problems.append(f"{r}: FAILSAFE_SITE requires {fn}(); not found.")
    else:
        body = code[span[0]:span[1]]
        fpos, cpos = body.find(failsafe), body.find(cap)
        if fpos < 0 or cpos < 0 or fpos > cpos:
            line = code[:span[0]].count("\n") + 1
            problems.append(
                f"{r}:{line}: {fn}() must refuse {cap} when the clip rectangle "
                f"is on no monitor ({failsafe}), before applying it. This "
                f"failsafe is deliberately independent of every env gate: if any "
                f"future path reaches the clip unguarded, the user must still be "
                f"able to move his pointer back to his own desktop.")

    # ---- RULE 4: one parse of MC2_WINDOW_OFFSCREEN ------------------------
    parsers = []
    for p in iter_sources():
        r = rel(p)
        if not interesting(r):
            continue
        code = get_with_strings(r)
        if code is None:
            continue
        for m in re.finditer(r"(?:getenv\s*\(\s*|\b(?:get|is|isSet|isNonEmpty|str|raw)\s*<\s*)\"" + OFFSCREEN_VAR + r"\"", code):
            parsers.append((r, code[:m.start()].count("\n") + 1))
    if len(parsers) != 1 or parsers[0][0] != OFFSCREEN_PARSE_OWNER:
        where = ", ".join(f"{f}:{l}" for f, l in parsers) or "(none)"
        problems.append(
            f"{OFFSCREEN_VAR} must be parsed exactly once, in "
            f"{OFFSCREEN_PARSE_OWNER}; found {len(parsers)} at [{where}]. Two "
            f"parses is how 'the window is offscreen' and 'we may not take the "
            f"cursor' became separate answers to the same question.")

    return problems


# ---------------------------------------------------------------------------
# Selftest -- prove every arm fires
# ---------------------------------------------------------------------------
def selftest() -> int:
    real = {}
    for r in set(list(POLICY_FILES) + [g[0] for g in GUARDED_SITES]):
        p = ROOT / r
        real[r] = p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None

    def reader(overrides):
        def read(r):
            if r in overrides:
                return overrides[r]
            if r in real:
                return real[r]
            p = ROOT / r
            return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None
        return read

    failures = []

    def grade(name, overrides, needle):
        probs = check(read=reader(overrides))
        hit = [p for p in probs if needle in p]
        if hit:
            print(f"  PASS  {name}")
        else:
            print(f"  FAIL  {name}: expected a problem containing {needle!r}; "
                  f"got {probs or '(clean)'}")
            failures.append(name)

    print("[input-capture-visibility] selftest")

    # Arm 0: the tree as it stands must be CLEAN, or every arm below is
    # measuring noise.
    base = check(read=reader({}))
    if base:
        print(f"  FAIL  baseline clean: {base}")
        failures.append("baseline")
    else:
        print("  PASS  baseline clean")

    # RULE 1, the real regression: restore the unguarded grab.
    gr = "GameOS/gameos/gos_render.cpp"
    unguarded = real[gr].replace("gos_CursorCaptureForbidden(window, &forbidReason)",
                                 "false")
    grade("RULE 1 -- guard removed from apply_mouse_grab",
          {gr: unguarded}, "does not call gos_CursorCaptureForbidden")

    # RULE 1, rename: the guard is gone because the function is.
    renamed = real[gr].replace("static void apply_mouse_grab(",
                               "static void apply_pointer_confinement(")
    grade("RULE 1 -- chokepoint renamed away",
          {gr: renamed}, "requires a definition of apply_mouse_grab()")

    # RULE 2: a brand-new capture call in an unlisted engine file.
    victim = None
    for p in iter_sources():
        r = rel(p)
        if r not in POLICY_FILES and p.suffix == ".cpp":
            victim = r
            break
    if victim:
        src = (ROOT / victim).read_text(encoding="utf-8", errors="replace")
        grade("RULE 2 -- new capture call in an unlisted file",
              {victim: src + "\nvoid mc2_selftest_steal(){ ClipCursor(0); }\n"},
              "not in check-input-capture-visibility.POLICY_FILES")
    else:
        print("  FAIL  RULE 2: found no unlisted engine .cpp to mutate")
        failures.append("RULE 2 victim")

    # RULE 3b: the geometry failsafe deleted.
    nofail = real[gr].replace("MONITOR_DEFAULTTONULL", "MONITOR_DEFAULTTOPRIMARY")
    grade("RULE 3b -- off-monitor ClipCursor failsafe removed",
          {gr: nofail}, "on no monitor")

    # RULE 4: a second parse of MC2_WINDOW_OFFSCREEN.
    gi = "GameOS/gameos/gameos_input.cpp"
    twice = real[gi] + '\nstatic int mc2_selftest_dup(){ return getenv("MC2_WINDOW_OFFSCREEN") != 0; }\n'
    grade("RULE 4 -- MC2_WINDOW_OFFSCREEN parsed twice",
          {gi: twice}, "must be parsed exactly once")
    typed_twice = real[gi] + '\nstatic int mc2_selftest_dup2(){ return mc2::gate::get<"MC2_WINDOW_OFFSCREEN">(); }\n'
    grade("RULE 4 -- MC2_WINDOW_OFFSCREEN read a second time through the typed registry",
          {gi: typed_twice}, "must be parsed exactly once")

    # Comment stripper: prose naming an API must NOT be a hit.
    if victim:
        src = (ROOT / victim).read_text(encoding="utf-8", errors="replace")
        probs = check(read=reader(
            {victim: src + "\n// never call ClipCursor( here\n"}))
        if any("POLICY_FILES" in p and victim in p for p in probs):
            print("  FAIL  stripper: a COMMENT naming ClipCursor was treated as a call")
            failures.append("stripper")
        else:
            print("  PASS  stripper -- comment mention is not a call")

    if failures:
        print(f"[input-capture-visibility] SELFTEST FAILED: {failures}")
        return 1
    print("[input-capture-visibility] selftest OK -- every arm fires")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    problems = check()
    if problems:
        print(f"[input-capture-visibility] FAIL ({len(problems)} problem(s)):")
        for p in problems:
            print("  " + p)
        return 1
    if not args.quiet:
        print(f"[input-capture-visibility] OK -- {len(GUARDED_SITES)} guarded "
              f"chokepoint(s), {len(POLICY_FILES)} policy file(s); no engine "
              f"source captures the pointer or the foreground on a path where "
              f"the window is not user-visible.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
