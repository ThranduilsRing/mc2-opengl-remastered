#!/usr/bin/env python3
# scripts/tests/test_exe_fingerprint_layout.py
"""Operand-recovery tests for scripts/exe_fingerprint.py (FINGERPRINT-DETACHED-1).

WHAT THIS PINS
--------------
The .rdata layout the fingerprint reader depends on. The four pooled operands
{src, built, branch, sha} sit immediately before the printf format literal, and
the reader recovers them by scanning backwards. The bug this file exists to
prevent: the linker packs small binary constants into the alignment gaps
BETWEEN pooled literals, and a tokenizer that splits on NUL alone welds that
filler onto the following literal, fails its all-printable test, and silently
DELETES an operand -- after which the positional window slides and a perfectly
good binary reports "layout-unrecognized" / NO-FINGERPRINT.

CAPTURED, NOT IMAGINED
----------------------
CAPTURED_TAIL below is a verbatim 160-byte transcript of the bytes preceding
the format literal in

    A:/Games/mc2-terrain-ambient-floor/build64/RelWithDebInfo/mc2.exe
    (exe sha256 f6d4537e9dda265620ce7b01d0620dd8895ae7b2f7d8be476b9f4022f4ee37f5)

which failed to fingerprint on 2026-07-31. Note b"\\xe7\\x84\\x11U" sitting where
padding should be, immediately before the branch literal. The four operand
offsets within that transcript are 8-byte aligned relative to its start, so
placing it at offset 0 of a synthetic image reproduces the real alignment
exactly -- no build required.

WHAT THIS DOES **NOT** COVER
----------------------------
It cannot prove that any FUTURE compiler/linker actually emits this layout: it
pins the READER against a captured image, not the WRITER. If MSVC someday pools
the operands somewhere else entirely, or code/mechcmd2.cpp changes the printf,
these tests still pass while real binaries stop parsing. The guards for that are
the format-literal pin (BANNER_FMT) and check-delivery.py's refusal to grade an
install with no readable fingerprint -- both of which fail loudly, not silently.
It also does not exercise cmake/GenerateBuildFingerprint.cmake at all.

Pure in-memory byte scans. No build, no git, no launch: milliseconds.

Run:
  py -3 -m pytest scripts/tests/test_exe_fingerprint_layout.py -v
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.exe_fingerprint import (  # noqa: E402
    BANNER_FMT,
    DIRTY_MARKER,
    extract_from_bytes,
)

# --- the real bytes, transcribed from the failing binary --------------------
CAPTURED_TAIL = (
    b"gine joystickProbe\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    b"A:/Games/mc2-terrain-ambient-floor"
    b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    b"2026-07-30T12:33:10\x00"
    b"\xe7\x84\x11U"                       # <-- filler in the alignment gap
    b"claude/terrain-ambient-floor-2\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    b"6b80b97bc8a2\x00\x00\x00\x00"
)

_ALIGN = 8


def _image(src: str, built: str, branch: str, sha: str,
           *, filler: bytes = b"", dirty: bool = False) -> bytes:
    """Synthesize an .rdata-like image with the operands 8-byte aligned.

    `filler` is planted in the gap immediately before the BRANCH operand, which
    is where the real linker put it.
    """
    buf = bytearray(b"\x00" * 64)

    def put(text: str, pre: bytes = b"") -> None:
        while len(buf) % _ALIGN:
            buf.append(0)
        if pre:
            # Occupy one whole alignment slot: NULs then the filler, so the
            # literal still starts on a boundary but is preceded by non-NUL.
            buf.extend(b"\x00" * (_ALIGN - len(pre)))
            buf.extend(pre)
        buf.extend(text.encode())
        buf.append(0)

    put(src)
    put(built)
    put(branch, filler)
    put(sha)
    while len(buf) % _ALIGN:
        buf.append(0)
    blob = bytes(buf) + BANNER_FMT
    if dirty:
        blob += b"\x00" + DIRTY_MARKER + b"\x00"
    return blob


OK = ("A:/Games/mc2-main", "2026-07-31T05:20:31", "claude/nifty-mendeleev",
      "bf4161d27fe6")


# --- the captured regression ------------------------------------------------

def test_captured_failing_binary_now_parses():
    """The exact byte pattern that produced layout-unrecognized in the field."""
    fp = extract_from_bytes(CAPTURED_TAIL + BANNER_FMT)
    assert fp["present"], fp
    assert fp["sha"] == "6b80b97bc8a2"
    assert fp["built"] == "2026-07-30T12:33:10"
    assert fp["src"] == "A:/Games/mc2-terrain-ambient-floor"


def test_captured_branch_is_not_corrupted_by_filler():
    """The last filler byte is 0x55 'U', which IS printable ASCII.

    A reader that merely strips leading non-printables yields "Uclaude/..." --
    a silently wrong branch, which is worse than refusing. Pin the exact value.
    """
    fp = extract_from_bytes(CAPTURED_TAIL + BANNER_FMT)
    assert fp["branch"] == "claude/terrain-ambient-floor-2"
    assert not fp["branch"].startswith("U")


# --- synthetic coverage of the same shape -----------------------------------

def test_clean_layout_parses():
    fp = extract_from_bytes(_image(*OK))
    assert fp["present"], fp
    assert (fp["src"], fp["built"], fp["branch"], fp["sha"]) == OK


def test_filler_before_branch_is_survivable():
    fp = extract_from_bytes(_image(*OK, filler=b"\xe7\x84\x11U"))
    assert fp["present"], fp
    assert fp["branch"] == "claude/nifty-mendeleev"
    assert fp["sha"] == "bf4161d27fe6"


def test_entirely_printable_filler_keeps_sha_trustworthy():
    """DOCUMENTED LIMIT, not an aspiration.

    If the filler bytes are themselves all printable, the token is wholly
    printable and cannot be told from a branch that genuinely starts "ABCD"
    without assuming universal 8-byte alignment -- and assuming that would
    corrupt tightly packed layouts (it broke tests/telemetry's hand-packed
    fixture). So this case is left alone.

    What must NEVER break is the load-bearing field: the sha still parses
    correctly, because src/built/sha are all shape-gated. Only the decorative
    branch string carries the contamination.
    """
    fp = extract_from_bytes(_image(*OK, filler=b"ABCD"))
    assert fp["present"], fp
    assert fp["sha"] == "bf4161d27fe6"
    assert fp["src"] == "A:/Games/mc2-main"
    assert fp["built"] == "2026-07-31T05:20:31"
    assert fp["branch"] == "ABCDclaude/nifty-mendeleev"   # known contamination


def test_tightly_packed_operands_still_parse():
    """Alignment trimming must never touch a clean token. This layout packs the
    operands with single NULs and no padding at all (the shape
    tests/telemetry/test_manifest_schema.py builds); it parsed before this
    change and must keep parsing."""
    blob = (b"MZ\x00pad\x00A:/Games/lane-x\x002026-07-29T13:52:25\x00"
            b"claude/lane-x\x00abc123def456\x00" + BANNER_FMT)
    fp = extract_from_bytes(blob)
    assert fp["present"], fp
    assert fp["sha"] == "abc123def456"
    assert fp["src"] == "A:/Games/lane-x"
    assert fp["branch"] == "claude/lane-x"


def test_detached_head_branch_parses():
    """A detached-HEAD build stamps branch="HEAD". Verified against three real
    detached builds (mc2-refbuild / mc2-cac2 / mc2-vissem2-clean), all of which
    parse; "HEAD" is pooled adjacently like any other branch string. This pins
    that the workflow `git worktree add --detach` stays fingerprintable."""
    fp = extract_from_bytes(_image("A:/Games/mc2-refbuild", "2026-07-28T21:51:08",
                                   "HEAD", "cf97ed524df6"))
    assert fp["present"], fp
    assert fp["branch"] == "HEAD"
    assert fp["sha"] == "cf97ed524df6"


def test_detached_head_branch_with_filler():
    fp = extract_from_bytes(_image("A:/Games/mc2-refbuild", "2026-07-28T21:51:08",
                                   "HEAD", "cf97ed524df6",
                                   filler=b"\xe7\x84\x11U"))
    assert fp["present"], fp
    assert fp["branch"] == "HEAD"


def test_dirty_marker_detected():
    assert extract_from_bytes(_image(*OK, dirty=True))["dirty"] is True
    assert extract_from_bytes(_image(*OK))["dirty"] is False


# --- the validator must stay strict -----------------------------------------
# Loosening a parser is how a wrong commit gets attributed confidently. These
# pin that the shape checks still REFUSE rather than guess.

def test_refuses_when_src_is_not_a_path():
    fp = extract_from_bytes(_image("not-a-path", "2026-07-31T05:20:31",
                                   "claude/x", "bf4161d27fe6"))
    assert not fp["present"]
    assert fp["reason"] == "layout-unrecognized"


def test_refuses_when_sha_is_not_hex():
    fp = extract_from_bytes(_image("A:/Games/mc2-main", "2026-07-31T05:20:31",
                                   "claude/x", "zzzznothex"))
    assert not fp["present"]
    assert fp["reason"] == "layout-unrecognized"


def test_refuses_when_timestamp_is_not_iso():
    fp = extract_from_bytes(_image("A:/Games/mc2-main", "last tuesday",
                                   "claude/x", "bf4161d27fe6"))
    assert not fp["present"]
    assert fp["reason"] == "layout-unrecognized"


def test_refuses_when_branch_operand_is_absent():
    """Three operands where four are expected must NOT be silently accepted by
    sliding an unrelated neighbouring literal into the branch slot."""
    buf = bytearray(b"\x00" * 64)
    for text in ("InitializeGameEngine joystickProbe", "A:/Games/mc2-main",
                 "2026-07-31T05:20:31", "bf4161d27fe6"):
        while len(buf) % _ALIGN:
            buf.append(0)
        buf.extend(text.encode())
        buf.append(0)
    while len(buf) % _ALIGN:
        buf.append(0)
    fp = extract_from_bytes(bytes(buf) + BANNER_FMT)
    assert not fp["present"]
    assert fp["reason"] == "layout-unrecognized"


def test_failure_detail_names_what_it_saw():
    """A bare "layout-unrecognized" sent one investigation chasing a
    detached-HEAD theory for a filler-byte bug. The detail must show the tail."""
    fp = extract_from_bytes(_image("not-a-path", "2026-07-31T05:20:31",
                                   "claude/x", "bf4161d27fe6"))
    assert "bf4161d27fe6" in fp["detail"]
    assert "not-a-path" in fp["detail"]


# --- controls (this module's own house rule: prove the scanner works) -------

def test_banner_absent_is_distinct_from_layout_failure():
    fp = extract_from_bytes(b"\x00" * 512)
    assert not fp["present"]
    assert fp["reason"] == "banner-absent"


def test_format_absent_when_printf_shape_changed():
    blob = b"\x00" * 64 + b"[BUILD_FINGERPRINT v1] sha=%s WHO CHANGED THIS\n\x00"
    fp = extract_from_bytes(blob)
    assert not fp["present"]
    assert fp["reason"] == "format-absent"


def test_single_char_shards_do_not_become_operands():
    """Splitting on non-printables surfaces 1-char shards of neighbouring
    binary constants ('@', 'H', 'U'). If those were treated as operands they
    would shift the window and break a good binary."""
    buf = bytearray(b"\x00" * 32)
    buf.extend(b"\x9f\x40\x00\x00\xe0\x48\x00\x00")   # yields '@' and 'H'
    body = _image(*OK)
    fp = extract_from_bytes(bytes(buf) + body)
    assert fp["present"], fp
    assert fp["branch"] == "claude/nifty-mendeleev"
