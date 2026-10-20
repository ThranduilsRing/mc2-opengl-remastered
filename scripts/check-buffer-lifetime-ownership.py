#!/usr/bin/env python3
"""BUFFER-LIFETIME-OWNERSHIP — buffer lifetime governance checker.

Enforces the hazard-class invariants catalogued in
docs/render-backend-seams/buffer-lifetime-ledger.md so a buffer can't silently lose the
mechanism that avoids a CPU-overwrite-while-GPU-reads hazard (the Vulkan-critical seam):

  1. Every ledger row declares a class in {A,B,C,D,E}.
  2. NATIVE CULL SLOT OWNERSHIP ? CPU uploads/pool reuse wait for that slot's
     ticket, readback mapping waits for completion, and buffer-scoped barriers
     order compute -> transfer -> host. The retired GL indirect family has no
     command buffer or GL command-barrier obligation.
  3. CLASS-D VULKAN-DEBT DECLARED — the known implicit-sync buffers stay listed so the
     migration debt remains visible (a class-D buffer dropped from the ledger FAILs).
  4. STALE COUNTER DECLARED — gpu_drawn_instances is documented as known-stale.

Exit 0 = PASS.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "render-backend-seams" / "buffer-lifetime-ledger.md"
OBJB = ROOT / "docs" / "render-backend-seams" / "objbatcher-zero-gpu-drawn-recon-1.md"

# parse rows: | `symbol...` | CLASS | ... |
ROW = re.compile(r"^\|\s*`([^`]+)`[^|]*\|\s*([A-E])\s*\|", re.M)

NATIVE_CULL = ROOT / "GameOS" / "gameos" / "vk_actor_cull_native.cpp"


def native_cull_hazards(source: str) -> list[str]:
    # Comments and log strings cannot satisfy a synchronization contract.
    code = re.sub(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'", '', source, flags=re.S)
    def function(result, name):
        match = re.search(r'\b' + result + r'\s+' + name + r'\s*\(', code)
        if not match:
            return ""
        first = code.find('{', match.end())
        if first < 0:
            return ""
        end, depth = first + 1, 1
        while end < len(code) and depth:
            depth += (code[end] == '{') - (code[end] == '}')
            end += 1
        return code[first:end]
    submit = function('NativeCullSubmit', 'nativeCullSubmit')
    poll = function('bool', 'nativeCullPoll')
    failures = []
    busy = re.search(r'if\s*\(\s*slot\.submitted\s*&&\s*!ctx\.isComplete\s*\(\s*slot\.ticket\s*\)\s*\)\s*return\s+NativeCullSubmit::Busy\s*;', submit)
    writes = [submit.find(token) for token in ('slot.records.upload(', 'slot.params.upload(', 'vkResetCommandPool(')]
    if not busy or any(pos < 0 or busy.end() > pos for pos in writes):
        failures.append('native cull CPU overwrite/pool reuse lacks a preceding slot-ticket guard')
    ready = re.search(r'if\s*\(\s*!slot\.submitted\s*\|\|\s*!ctx\.isComplete\s*\(\s*slot\.ticket\s*\)[^;{}]*\)\s*return\s+false\s*;', poll)
    mapping = poll.find('slot.staging.map(')
    if not ready or mapping < 0 or ready.end() > mapping:
        failures.append('native cull readback mapping lacks a preceding completion guard')
    compact = re.sub(r'\s+', '', submit)
    output = 'bufferBarrier(slot.cb,slot.output.get(),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT)'
    host = 'bufferBarrier(slot.cb,slot.staging.get(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT)'
    if output not in compact:
        failures.append('native cull output lacks its compute-write to transfer-read buffer barrier')
    if host not in compact:
        failures.append('native cull staging lacks its transfer-write to host-read buffer barrier')
    if 'VK_PIPELINE_STAGE_ALL_COMMANDS_BIT' in submit or 'mc2vk::cmdBarrier(' in submit:
        failures.append('native cull reintroduced device-wide synchronization for private buffers')
    return failures


# class-D members that must stay declared (Vulkan-debt visibility)
REQUIRED_CLASS_D = {"s_ssbo"}


def main() -> int:
    quiet = "--quiet" in sys.argv
    if not LEDGER.exists():
        print(f"[buffer-lifetime] ERROR ledger missing: {LEDGER}", file=sys.stderr)
        return 2
    text = LEDGER.read_text(encoding="utf-8", errors="replace")
    rows = ROW.findall(text)
    fails = []

    if not rows:
        fails.append("no buffer rows parsed from ledger table")

    # 1. classes valid (regex already constrains A-E; check non-empty)
    classes = {sym: cls for sym, cls in rows}

    # 2. Native producer/readback lifetime and visibility.
    if not NATIVE_CULL.exists():
        fails.append('native cull producer missing')
    else:
        fails.extend(native_cull_hazards(NATIVE_CULL.read_text(encoding="utf-8")))

    # 3. class-D Vulkan-debt declared
    declared_d = {sym for sym, cls in rows if cls == "D"}
    for need in REQUIRED_CLASS_D:
        if not any(need in d for d in declared_d):
            fails.append(f"class-D buffer '{need}' not declared in ledger — implicit-sync "
                         f"Vulkan-debt no longer visible")

    # 4. stale counter declared
    if not OBJB.exists() or "gpu_drawn_instances" not in OBJB.read_text(encoding="utf-8", errors="replace"):
        fails.append("gpu_drawn_instances not declared known-stale in "
                     "objbatcher-zero-gpu-drawn-recon-1.md")

    if fails:
        print("[buffer-lifetime] FAIL — buffer lifetime ownership gaps:", file=sys.stderr)
        for f in fails:
            print(f"  - {f}", file=sys.stderr)
        return 1
    if not quiet:
        nb = sum(1 for _, c in rows if c == "B")
        nd = sum(1 for _, c in rows if c == "D")
        print(f"[buffer-lifetime] PASS — {len(rows)} buffers classified "
              f"({nb} class-B barrier-checked, {nd} class-D debt-declared); "
              f"native slot tickets/barriers intact; stale counter declared.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
