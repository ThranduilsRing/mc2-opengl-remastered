#!/usr/bin/env python3
"""proof_ext - the opt-in acceptances of the rewriters' object proof, the ledger of bounded exceptions, and the harness / test tree gate.

WHAT: `LayoutMixin` sits in front of run_tidy_headers.Proof. By default it behaves exactly like Proof (plain objhash identity). Opt-in flags add: `layout_only_ok` (tools/objhash.py
classifies the object LAYOUT-ONLY: a .data / .bss section reordered, symbol set and every symbol's bytes identical by name) and `order_only_ok` (ORDER-ONLY: only symbol-table /
relocation index order differs). DIFF always fails, except an object with an entry in tools/tidy/proof_allow.json (class "unreferenced-data-dropped"): bounded to that object
and to edits of the entry's headers, and re-verified per symbol by objhash.classify_allowed. `Gate` extends the same proof to the Visual Studio trees (tests/unit, the contract
harnesses, the standalone tools) that the Ninja includer map cannot see.
WHY: coordinator rulings 2026-10-11: LAYOUT-ONLY and ORDER-ONLY only as explicit flags with every accepted object named in the commit message; the one codegen exception
(MapGeneratorDialog's unreferenced stb_image tables) is a visible, bounded ledger entry, never a blanket ignore. Nothing here changes a default.
"""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_headers as rth  # noqa: E402
import run_tidy_safe as rts  # noqa: E402
import vs_trees  # noqa: E402

objhash = rth.objhash
ALLOW_FILE = os.path.join(HERE, "proof_allow.json")
ALLOW_CLASS = "unreferenced-data-dropped"


def load_allow(path=ALLOW_FILE):
    """-> the entries of the exception ledger ({object, class, headers, reason})."""
    if not path or not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as fh:
        return [e for e in json.load(fh).get("entries", []) if e.get("class") == ALLOW_CLASS]


class LayoutMixin:
    """Proof comparison with the opt-in acceptances. Put it BEFORE run_tidy_headers.Proof in the bases."""

    layout_only_ok = False
    order_only_ok = False
    allow = ()

    def _per_symbol(self):
        return bool(self.layout_only_ok or self.order_only_ok or self.allow)

    def _digest(self, path):
        return objhash.obj_digest(path, per_symbol=True) if self._per_symbol() else objhash.obj_digest(path)

    def baseline(self) -> int:
        rc, _ = rts.ninja_objects_targets(self.tree_abs, self.objs, self.jobs)
        if rc:
            return rc
        self.before = {o: self._digest(os.path.join(self.tree_abs, o)) for o in self.objs}
        return 0

    def accepted(self, o, now):
        """-> the label of the acceptance that lets a non-identical object pass, or None."""
        before = self.before.get(o)
        if now is None or before is None or not self._per_symbol():
            return None
        verdict, _why = objhash.classify(before, now, before.get("persym"), now.get("persym"))
        if verdict == "LAYOUT-ONLY" and self.layout_only_ok:
            return "LAYOUT-ONLY"
        if verdict == "ORDER-ONLY" and self.order_only_ok:
            return "ORDER-ONLY"
        edited = {h for h in getattr(self, "headers", ()) if self.on_disk.get(h) == self.edited.get(h)}
        for entry in self.allow:
            if entry["object"] == o and edited & set(entry["headers"]) and objhash.classify_allowed(before.get("persym"), now.get("persym"))[0]:
                return "ALLOWED %s" % entry["class"]
        return None

    def differs(self, o, now) -> bool:
        before = self.before[o]
        if now is None:
            return True
        if now["code_sha256"] == before["code_sha256"]:
            return False
        return self.accepted(o, now) is None

    def bad_objects(self) -> set:
        rc, failed = rts.ninja_objects_targets(self.tree_abs, self.objs, self.jobs)
        bad = {f.replace("\\", "/") for f in failed}
        for o in self.objs:
            if o in bad or o in self.noisy:
                continue
            if self.differs(o, self._digest(os.path.join(self.tree_abs, o))):
                bad.add(o)
        return bad

    def accepted_objects(self, objs):
        """-> {object: label} for the built objects that differ from the baseline but were accepted; call after the final state is built."""
        out = {}
        for o in sorted(objs):
            now = self._digest(os.path.join(self.tree_abs, o))
            before = self.before.get(o)
            if now and before and now["code_sha256"] != before["code_sha256"]:
                label = self.accepted(o, now)
                if label:
                    out[o] = label
        return out


def configure(proof, args):
    """Copy the proof-mode flags of a rewriter's argparse namespace onto a proof object."""
    proof.layout_only_ok = bool(getattr(args, "layout_only_ok", False))
    proof.order_only_ok = bool(getattr(args, "order_only_ok", False))
    proof.allow = [] if (getattr(args, "no_proof_allow", False) or getattr(args, "relaxed_proof", None)) else load_allow()


def mode_text(args):
    parts = []
    if getattr(args, "layout_only_ok", False):
        parts.append("LAYOUT-ONLY accepted (--layout-only-ok)")
    if getattr(args, "order_only_ok", False):
        parts.append("ORDER-ONLY accepted (--order-only-ok)")
    if not getattr(args, "no_proof_allow", False) and not getattr(args, "relaxed_proof", None) and load_allow():
        parts.append("bounded exceptions of tools/tidy/proof_allow.json")
    return "plain objhash identity" + ("; " + "; ".join(parts) + ", rulings 2026-10-11" if parts else "")


def acceptance_note(accepted, gate_accepted=()):
    """The commit-message block that names every object accepted without being identical (empty when there is none)."""
    lines = ["  %s  [%s]" % (o, label) for o, label in sorted(accepted.items())] + ["  %s  [%s]" % (k, lab) for k, lab in gate_accepted]
    if not lines:
        return ""
    return "\n\nObjects accepted without being IDENTICAL (%d; every other including object is identical):\n%s" % (len(lines), "\n".join(lines))


def failure_reason(objects, fail_text):
    """The ledger reason for a header the harness / test gate rejected: the failing objects AND what the build or the comparison said about them (the first two, 500 characters)."""
    names = ", ".join(objects[:3])
    said = " || ".join("%s: %s" % (o, fail_text[o]) for o in objects[:2] if fail_text.get(o))
    return "harness/test objects differ or fail to build: %s%s" % (names, " -- " + said[:500] if said else "")


class Gate:
    """The harness / test trees as a second proof set. snapshot() before any edit, settle() after the Ninja proof chose its kept files."""

    def __init__(self, headers, layout_only_ok=False, order_only_ok=False, trees=vs_trees.TREES):
        self.headers = list(headers)
        self.layout_only_ok = layout_only_ok
        self.order_only_ok = order_only_ok
        self.map = vs_trees.includer_map(trees)
        self.by_header = {h: set(self.map.get(h, ())) for h in self.headers}
        self.keys = sorted(set().union(*self.by_header.values())) if self.by_header else []
        self.snap_path = os.path.join(rts.ROOT, ".claude", "l2cfg", "vs_gate_snapshot.json")
        self.accepted = []

    def snapshot(self) -> bool:
        if not self.keys:
            return True
        doc = vs_trees.snapshot(self.headers, self.snap_path)
        print("  vs gate: %d harness/test object(s) hashed (%d did not build before the edit)" % (len(doc["objects"]), len(doc["failed_before"])), flush=True)
        return True

    def settle(self, kept, orig, mtimes):
        """Rebuild the harness / test objects with the kept files edited. -> (kept, [(file, why)]). A file whose objects fail is restored; repeated until clean."""
        rejected = []
        if not self.keys:
            return list(kept), rejected
        kept = list(kept)
        for _ in range(4):
            if not kept:
                break
            lines, bad, accepted = vs_trees.verify(self.snap_path, self.layout_only_ok, self.order_only_ok)
            self.accepted = accepted
            if not bad:
                print("  vs gate: %d object(s) checked, %d accepted without being identical" % (len(lines), len(accepted)), flush=True)
                return kept, rejected
            failing = {ln.split()[1] for ln in lines if ln.startswith("FAIL")}
            fail_text = {ln.split()[1]: ln.split(None, 2)[2] if len(ln.split(None, 2)) > 2 else "" for ln in lines if ln.startswith("FAIL")}
            culprits = [h for h in kept if self.by_header.get(h, set()) & failing] or list(kept)
            for h in culprits:
                with open(os.path.join(rts.ROOT, h), "wb") as fh:
                    fh.write(orig[h])
                rts.restore_file(h, mtimes[h])
                rejected.append((h, failure_reason(sorted(self.by_header.get(h, set()) & failing), fail_text)))
            kept = [h for h in kept if h not in culprits]
        return kept, rejected
