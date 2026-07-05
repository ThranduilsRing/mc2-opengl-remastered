#!/usr/bin/env python3
# scripts/test_smoke_lane_lock.py
"""Unit test for SMOKE-LATENCY-WINS-1 win #2: per-lane smoke.lock keying.

Proves _lane_lock_token (scripts/run_smoke.py) derives a lane token that:
  - is STABLE for the same (lease_folder, exe_path) inputs
  - DIFFERS across distinct leased folders (so concurrent lanes get distinct
    lock files and never collide)
  - is filesystem-safe (no '.', no path separators, no spaces)
  - falls back sanely to the exe's parent dir name when no lease is held
    (--no-lease), so two --no-lease runs against the SAME exe still serialize

No process is launched; pure function under test, synthetic inputs only.
Run:  py -3 scripts/test_smoke_lane_lock.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.run_smoke import _lane_lock_token

FAKE_SHORT_NAMES = {
    r"A:/Games/mc2-opengl/mc2-win64-v0.4": "0.4",
    r"A:/Games/mc2-opengl/mc2-win64-v0.4c": "0.4c",
    r"A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0": "0.5-testing",
}


def _fake_short_name(folder: str) -> str:
    return FAKE_SHORT_NAMES.get(folder, folder)


def run():
    failures = []

    def check(name, got, expected):
        if got == expected:
            print(f"  PASS {name}: {got!r}")
        else:
            print(f"  FAIL {name}: got {got!r}, expected {expected!r}")
            failures.append(name)

    def check_true(name, cond, detail=""):
        if cond:
            print(f"  PASS {name}")
        else:
            print(f"  FAIL {name} ({detail})")
            failures.append(name)

    # T1: two leased-folder invocations with DIFFERENT folders -> different tokens.
    tok_04 = _lane_lock_token(
        r"A:/Games/mc2-opengl/mc2-win64-v0.4", "unused", _fake_short_name)
    tok_04c = _lane_lock_token(
        r"A:/Games/mc2-opengl/mc2-win64-v0.4c", "unused", _fake_short_name)
    check_true("T1 distinct lanes -> distinct tokens", tok_04 != tok_04c,
               detail=f"{tok_04} vs {tok_04c}")

    # T2: same leased folder -> same token (stable / deterministic).
    tok_04_again = _lane_lock_token(
        r"A:/Games/mc2-opengl/mc2-win64-v0.4", "unused", _fake_short_name)
    check("T2 same lane -> same token (stable)", tok_04_again, tok_04)

    # T3: short name containing '.' is sanitized to a filesystem-safe token.
    check_true("T3 no '.' in token", "." not in tok_04, detail=tok_04)
    check_true("T3 token is alnum/-/_ only",
              all(c.isalnum() or c in "-_" for c in tok_04), detail=tok_04)

    # T4: space-containing short name (0.5-testing folder) sanitizes cleanly.
    tok_space = _lane_lock_token(
        r"A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0",
        "unused", _fake_short_name)
    check_true("T4 no space in token", " " not in tok_space, detail=tok_space)

    # T5: no lease (e.g. --no-lease) falls back to the exe's parent dir name,
    # and two invocations against the SAME exe path collide (serialize) as
    # they must -- this preserves pre-existing single-mutex safety for the
    # --no-lease path where the lease system provides no isolation at all.
    same_name_fn = lambda f: f  # not used on the None-lease path
    tok_noleaseA = _lane_lock_token(
        None, r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe", same_name_fn)
    tok_noleaseB = _lane_lock_token(
        None, r"A:/Games/mc2-opengl/mc2-win64-v0.4/mc2.exe", same_name_fn)
    check("T5 --no-lease same exe -> same token (still serializes)",
          tok_noleaseB, tok_noleaseA)

    # T6: no lease, DIFFERENT exe parent dirs -> different tokens (two distinct
    # --no-lease targets can still run concurrently).
    tok_noleaseC = _lane_lock_token(
        None, r"A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe", same_name_fn)
    check_true("T6 --no-lease distinct exe dirs -> distinct tokens",
              tok_noleaseA != tok_noleaseC,
              detail=f"{tok_noleaseA} vs {tok_noleaseC}")

    print()
    print("=" * 60)
    if failures:
        print(f"FAILED: {failures}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(run())
