#!/usr/bin/env python3
"""proof_ext - the opt-in LAYOUT-ONLY acceptance for the rewriters' object proof, and the harness / test tree gate.

WHAT: `LayoutMixin` sits in front of run_tidy_headers.Proof. By default it behaves exactly like Proof (plain objhash identity). With `layout_only_ok` an object also passes when
tools/objhash.py classifies it LAYOUT-ONLY (a .data / .bss section reordered, the symbol set and every symbol's bytes and relocation targets identical by name); ORDER-ONLY
and DIFF still fail. `Gate` extends the same proof to the Visual Studio trees (tests/unit, the contract harnesses, the standalone tools) that the Ninja includer map cannot see.
WHY: coordinator ruling 2026-10-11: LAYOUT-ONLY is acceptable only as an explicit flag, and every accepted object is named in the commit message. Nothing here changes a default.
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_headers as rth  # noqa: E402
import run_tidy_safe as rts  # noqa: E402
import vs_trees  # noqa: E402

objhash = rth.objhash


class LayoutMixin:
    """Proof comparison with optional LAYOUT-ONLY acceptance. Put it BEFORE run_tidy_headers.Proof in the bases."""

    layout_only_ok = False

    def _digest(self, path):
        return objhash.obj_digest(path, per_symbol=True) if self.layout_only_ok else objhash.obj_digest(path)

    def baseline(self) -> int:
        rc, _ = rts.ninja_objects_targets(self.tree_abs, self.objs, self.jobs)
        if rc:
            return rc
        self.before = {o: self._digest(os.path.join(self.tree_abs, o)) for o in self.objs}
        return 0

    def differs(self, o, now) -> bool:
        before = self.before[o]
        if now is None:
            return True
        if now["code_sha256"] == before["code_sha256"]:
            return False
        if not self.layout_only_ok:
            return True
        verdict, _why = objhash.classify(before, now, before.get("persym"), now.get("persym"))
        return verdict != "LAYOUT-ONLY"

    def bad_objects(self) -> set:
        rc, failed = rts.ninja_objects_targets(self.tree_abs, self.objs, self.jobs)
        bad = {f.replace("\\", "/") for f in failed}
        for o in self.objs:
            if o in bad or o in self.noisy:
                continue
            if self.differs(o, self._digest(os.path.join(self.tree_abs, o))):
                bad.add(o)
        return bad

    def layout_objects(self, objs):
        """-> {object: reason sections} for the objects (already built) that are LAYOUT-ONLY against the baseline; call after the final state is built."""
        out = {}
        for o in sorted(objs):
            now = self._digest(os.path.join(self.tree_abs, o))
            before = self.before.get(o)
            if now and before and now["code_sha256"] != before["code_sha256"]:
                verdict, why = objhash.classify(before, now, before.get("persym"), now.get("persym"))
                if verdict == "LAYOUT-ONLY":
                    out[o] = why
        return out


class Gate:
    """The harness / test trees as a second proof set. snapshot() before any edit, settle() after the Ninja proof chose its kept files."""

    def __init__(self, headers, layout_only_ok=False, trees=vs_trees.TREES):
        self.headers = list(headers)
        self.layout_only_ok = layout_only_ok
        self.map = vs_trees.includer_map(trees)
        self.by_header = {h: set(self.map.get(h, ())) for h in self.headers}
        self.keys = sorted(set().union(*self.by_header.values())) if self.by_header else []
        self.snap_path = os.path.join(rts.ROOT, ".claude", "l2cfg", "vs_gate_snapshot.json")
        self.layout = []

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
            lines, bad, layout = vs_trees.verify(self.snap_path, self.layout_only_ok)
            self.layout = layout
            if not bad:
                print("  vs gate: %d object(s) checked, %d layout-only" % (len(lines), len(layout)), flush=True)
                return kept, rejected
            failing = {ln.split()[1] for ln in lines if ln.startswith("FAIL")}
            culprits = [h for h in kept if self.by_header.get(h, set()) & failing] or list(kept)
            for h in culprits:
                with open(os.path.join(rts.ROOT, h), "wb") as fh:
                    fh.write(orig[h])
                rts.restore_file(h, mtimes[h])
                rejected.append((h, "harness/test objects differ or fail to build: %s" % ", ".join(sorted(self.by_header.get(h, set()) & failing)[:3])))
            kept = [h for h in kept if h not in culprits]
        return kept, rejected
