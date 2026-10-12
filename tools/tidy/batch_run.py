#!/usr/bin/env python3
"""batch_run - one proven rewrite batch as a function, and the batch-of-one fallback shared by rewrite_defines.py and rewrite_enums.py.

WHAT: `prove_batch` edits the picked headers, builds, compares every including object (and the harness / test trees with --vs-gate) and restores the files that fail.
`run_alone_fallback` takes the headers a multi-header batch rejected and proves each of them again on its own, so a header that failed only because of the company it
was batched with (an interaction of two edits in one object) is kept after all, and the report says which headers passed alone and which still fail alone.
WHY: three wide headers (tgl.h, vfx.h, dbasegui.h) were reverted in a 5-header batch and passed when run alone; finding that out by hand cost a session of manual re-runs.
The proof standard of the alone run is the batch's own (same flags, same gate); a header that fails alone is a real failure and stays reverted.
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_headers as rth  # noqa: E402
import run_tidy_safe as rts  # noqa: E402
import proof_ext  # noqa: E402


class Outcome:
    """The result of proving one batch: kept / rejected headers, the commit-message note of the accepted objects, the including objects per header."""

    def __init__(self, picked, kept=(), rejected=(), note="", inc=None, gated=False, rc=0):
        self.picked = list(picked)
        self.kept = list(kept)
        self.rejected = list(rejected)
        self.note = note
        self.inc = dict(inc or {})
        self.gated = gated
        self.rc = rc

    def kept_items(self):
        return sum(len(items) for rel, items, _ in self.picked if rel in self.kept)

    def proven_objects(self):
        return len({o for r in self.kept for o in self.inc.get(r, ())})


def prove_batch(a, tree_abs, picked, rewrite, make_proof, solve, tool, sources=None):
    """Edit, build and verify the picked [(rel, items, objs)]. rewrite(rel, items, original_bytes) -> bytes; make_proof(tree_abs, inc, orig, edited, jobs) -> Proof; solve(proof) -> (kept, rejected)."""
    orig = {rel: rth.read_bytes(rel) for rel, _, _ in picked}
    edited = {rel: rewrite(rel, items, orig[rel]) for rel, items, _ in picked}
    mtimes = {rel: os.stat(os.path.join(rts.ROOT, rel)).st_mtime_ns for rel in orig}
    inc = {rel: o for rel, _, o in picked}
    proof = make_proof(tree_abs, inc, orig, edited, a.build_jobs, sources=sources)
    proof_ext.configure(proof, a)
    gate = proof_ext.Gate(sorted(orig), a.layout_only_ok, a.order_only_ok) if a.vs_gate else None
    if gate:
        gate.snapshot()
    if proof.baseline():
        print("%s: the base build of the proof set failed; refusing to edit" % tool, file=sys.stderr)
        return Outcome(picked, inc=inc, rc=2)
    kept, rejected = solve(proof)
    for rel, why in rejected:
        rts.restore_file(rel, mtimes[rel])
        print("REVERT %s  %s" % (rel, why))
    if gate and kept:
        kept, more = gate.settle(kept, orig, mtimes)
        for rel, why in more:
            print("REVERT %s  %s" % (rel, why))
        rejected = list(rejected) + list(more)
        if more:
            proof.apply(set(kept))
            proof_ext.LayoutMixin.bad_objects(proof)
    accepted = proof.accepted_objects({o for r in kept for o in inc[r]})
    note = proof_ext.acceptance_note(accepted, gate.accepted if gate else ()) + proof_ext.noise_note(proof.noisy & {o for r in kept for o in inc[r]})
    return Outcome(picked, kept, rejected, note, inc, bool(gate))


def alone_candidates(picked, rejected):
    """The picked items whose header was rejected, in batch order. Empty for a batch of one header (it already ran alone)."""
    if len(picked) < 2:
        return []
    bad = {rel for rel, _ in rejected}
    return [p for p in picked if p[0] in bad]


def alone_report(passed, failed):
    """-> the report lines: which headers passed alone, which still fail alone and why."""
    lines = []
    if passed:
        lines.append("ALONE FALLBACK: %d header(s) passed alone (they failed only inside the batch): %s" % (len(passed), ", ".join(passed)))
    for rel, why in failed:
        lines.append("ALONE FALLBACK: %s still fails alone: %s" % (rel, why))
    if not passed and not failed:
        lines.append("ALONE FALLBACK: nothing to re-run")
    return lines


def run_alone_fallback(outcome, attempt, report=print):
    """Re-prove every header `outcome` rejected on its own.
    attempt(picked) -> Outcome for a one-header batch. -> (alone, still_rejected): `alone` are the Outcomes that kept their header (the caller commits each),
    `still_rejected` the (rel, why) pairs that failed alone as well. Stops at the first attempt that cannot build its baseline."""
    alone, still = [], []
    todo = alone_candidates(outcome.picked, outcome.rejected)
    if not todo:
        return alone, still
    why_batch = dict(outcome.rejected)
    for item in todo:
        report("ALONE: %s (rejected in the batch: %s)" % (item[0], why_batch.get(item[0], "")[:160]))
        one = attempt([item])
        if one.rc:
            still.append((item[0], "the base build of the proof set failed"))
            break
        if one.kept:
            alone.append(one)
        else:
            still.extend(one.rejected or [(item[0], "rejected")])
    for ln in alone_report([o.kept[0] for o in alone], still):
        report(ln)
    return alone, still
