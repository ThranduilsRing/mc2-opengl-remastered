#!/usr/bin/env python3
"""run_tidy_headers - phase 2 of the clang-tidy safe set: rewrite HEADERS and prove every translation unit that includes them.

WHAT: for a batch of headers, (1) find every object that includes each header (ninja's dependency log), (2) hash those objects, (3) run the
safe checks on one including TU with diagnostics restricted to the batch's headers (clang-tidy -line-filter), (4) rebuild every affected
object, (5) compare each with tools/objhash.py. A header whose includers are all identical is kept; any header whose includers differ, or fail
to compile, is reverted and the rest are re-proved.
WHY: a header edit reaches every TU that includes it, so the proof set is the union of those TUs, not one object. The batch is sized by that
union (--max-objs) so a rewrite of a widely included header does not turn into a full rebuild.

  py -3 tools/tidy/run_tidy_headers.py --tree build-alltargets --dir mclib --max-objs 300
  py -3 tools/tidy/run_tidy_headers.py --tree build-alltargets mclib/sortedsocket.hpp mclib/stuff/link.hpp
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_safe as rts  # noqa: E402
import pick_batch  # noqa: E402

objhash = rts.objhash
console_child = rts.console_child
ROOT = rts.ROOT


def includer_map(tree_abs):
    """-> {repo-relative header: set(object targets relative to the tree)} from `ninja -t deps`."""
    import configure_ninja as cn
    out = subprocess.run([cn.NINJA, "-C", tree_abs, "-t", "deps"], capture_output=True, text=True, errors="replace",
                         **console_child.child_kwargs(redirected=True)).stdout
    by_header, cur = {}, None
    for line in out.splitlines():
        if not line.strip():
            cur = None
        elif not line.startswith(" "):
            cur = line.split(":", 1)[0].replace("\\", "/")
        elif cur is not None:
            hp = os.path.normpath(os.path.join(tree_abs, line.strip())).replace("\\", "/")
            try:
                rel = os.path.relpath(hp, ROOT).replace("\\", "/")
            except ValueError:      # a system header on another drive
                continue
            if not rel.startswith(".."):
                by_header.setdefault(rel, set()).add(cur)
    return by_header


def target_to_source(tree_abs):
    """-> {object target (relative to the tree): repo-relative source}."""
    with open(os.path.join(tree_abs, "compile_commands.json"), encoding="utf-8") as fh:
        db = json.load(fh)
    out = {}
    for e in db:
        obj = os.path.normpath(os.path.join(e["directory"], e["output"]))
        src = e["file"] if os.path.isabs(e["file"]) else os.path.join(e["directory"], e["file"])
        out[os.path.relpath(obj, tree_abs).replace("\\", "/")] = os.path.relpath(os.path.normpath(src), ROOT).replace("\\", "/")
    return out


HEADER_SITE = pick_batch.SITE


def candidate_headers(dirs, includers, dirty):
    """Tracked headers under `dirs` with a rewritable site, not excluded or dirty, that some object includes."""
    out = subprocess.run(["git", "-C", ROOT, "ls-files"] + [d.rstrip("/") + "/" for d in dirs], capture_output=True, text=True,
                         **console_child.child_kwargs(redirected=True)).stdout.split()
    cands = []
    for rel in out:
        if not rel.lower().endswith((".h", ".hpp", ".inl")) or rts.is_excluded(rel) or rel in dirty or rel not in includers:
            continue
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as fh:
            if pick_batch.has_site(fh.read()):
                cands.append((len(includers[rel]), os.path.getsize(os.path.join(ROOT, rel)), rel))
    cands.sort()
    return cands


def line_filter(headers):
    return json.dumps([{"name": os.path.join(ROOT, h).replace("/", "\\")} for h in headers])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("headers", nargs="*")
    ap.add_argument("--dir", action="append", default=[])
    ap.add_argument("--tree", default="build-alltargets")
    ap.add_argument("--max-objs", type=int, default=300, help="stop adding headers once the union of includers exceeds this")
    ap.add_argument("--max-headers", type=int, default=50)
    ap.add_argument("--max-header-includers", type=int, default=200,
                    help="skip a header included by more objects than this (its proof is a near-full rebuild for a handful of sites)")
    ap.add_argument("--done", help="ledger of headers already tried (skipped; picked ones are appended)")
    ap.add_argument("--build-jobs", type=int, default=6)
    ap.add_argument("--dry-run", action="store_true", help="list the batch and its proof set, change nothing")
    args = ap.parse_args(argv)

    tree_abs = os.path.join(ROOT, args.tree)
    includers = includer_map(tree_abs)
    t2s = target_to_source(tree_abs)
    dirty = rts.dirty_files()
    done = set()
    if args.done and os.path.exists(args.done):
        with open(args.done, encoding="utf-8") as fh:
            done = {ln.strip() for ln in fh if ln.strip()}

    if args.headers:
        headers = [h.replace("\\", "/") for h in args.headers]
    else:
        headers, union = [], set()
        for n, _sz, rel in candidate_headers(args.dir, includers, dirty):
            if rel in done or len(includers[rel]) > args.max_header_includers:
                continue
            new_union = union | includers[rel]
            if headers and len(new_union) > args.max_objs:
                continue
            headers.append(rel)
            union = new_union
            if len(headers) >= args.max_headers:
                break
    if not headers:
        print("run_tidy_headers: no candidate headers")
        return 0
    if args.done and not args.dry_run:
        with open(args.done, "a", encoding="utf-8", newline="\n") as fh:
            fh.writelines(h + "\n" for h in headers)

    objs = sorted({o for h in headers for o in includers.get(h, ())})
    print(f"run_tidy_headers: {len(headers)} header(s), {len(objs)} object(s) to prove", flush=True)
    if args.dry_run:
        for h in headers:
            print(f"  {h}  ({len(includers.get(h, ()))} objects)")
        return 0

    # Base: the objects must be built from the unedited headers.
    rc, _ = rts.ninja_objects_targets(tree_abs, objs, args.build_jobs)
    if rc:
        print("run_tidy_headers: the base build of the proof set failed; refusing to edit", file=sys.stderr)
        return 2
    before = {o: objhash.obj_digest(os.path.join(tree_abs, o)) for o in objs}
    mtimes = {h: os.stat(os.path.join(ROOT, h)).st_mtime_ns for h in headers}

    # One representative including TU per header (smallest source), diagnostics restricted to the batch's headers.
    exe = rts.clang_tidy_exe()
    # clang-tidy applies no fix-its from a TU it cannot parse (MSVC-only constructs), so try the including TUs smallest first until one
    # rewrites the header (or none does).
    for h in headers:
        tus = sorted((os.path.getsize(os.path.join(ROOT, t2s[o])), t2s[o]) for o in includers.get(h, ()) if o in t2s
                     and not rts.is_excluded(t2s[o]) and t2s[o].lower().endswith((".cpp", ".cc", ".cxx")))
        seen = set()
        for _sz, tu in tus:
            if tu in seen:
                continue
            seen.add(tu)
            if len(seen) > 6:
                break
            cmd = rts.tidy_command(exe, args.tree, tu, fix=True)
            cmd.insert(-1, "--line-filter=" + line_filter([h]))
            cmd = [c for c in cmd if not c.startswith("--header-filter")] + ["--header-filter=.*"]
            subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
            if rts.git("diff", "--numstat", "--", tu).strip():        # the line filter should prevent this; never keep a TU edit
                rts.restore_file(tu, os.stat(os.path.join(ROOT, tu)).st_mtime_ns)
            if rts.git("diff", "--numstat", "--", h).strip():
                break

    changed = [h for h in headers if rts.git("diff", "--numstat", "--", h).strip()]
    for h in changed:
        a, d, _ = rts.git("diff", "--numstat", "--", h).split("\t")
        print(f"EDIT  {h}  +{a}/-{d}  ({len(includers[h])} objects)")
    if not changed:
        print("run_tidy_headers: no rewrites needed")
        return 0

    reverted = []
    for attempt in range(3):
        affected = sorted({o for h in changed for o in includers[h]})
        rc, failed = rts.ninja_objects_targets(tree_abs, affected, args.build_jobs)
        bad_objs = {f.replace("\\", "/") for f in failed}
        for o in affected:
            if o in bad_objs:
                continue
            now = objhash.obj_digest(os.path.join(tree_abs, o))
            if now is None or now["code_sha256"] != before[o]["code_sha256"]:
                bad_objs.add(o)
        if not bad_objs:
            break
        bad_headers = [h for h in changed if includers[h] & bad_objs]
        if not bad_headers:
            print("run_tidy_headers: build failed with no attributable header; reverting the batch", file=sys.stderr)
            bad_headers = list(changed)
        for h in bad_headers:
            rts.restore_file(h, mtimes[h])
            reverted.append((h, f"{len(includers[h] & bad_objs)} of {len(includers[h])} includers differ or fail to compile"))
            changed.remove(h)
        if not changed:
            break
        # objects of the reverted headers rebuild to their original bytes; rebuild them with the kept set next round
        affected_all = sorted({o for h in changed + [r for r, _ in reverted] for o in includers[h]})
        rts.ninja_objects_targets(tree_abs, affected_all, args.build_jobs)
    else:
        print("run_tidy_headers: no stable set after 3 rounds; reverting the batch", file=sys.stderr)
        for h in changed:
            rts.restore_file(h, mtimes[h])
        return 1

    for h, why in reverted:
        print(f"REVERT {h}  {why}")
    print(f"run_tidy_headers: {len(changed)} header(s) proven identical over {len({o for h in changed for o in includers[h]})} "
          f"object(s), {len(reverted)} reverted")
    if changed:
        print("kept (commit by pathspec): " + " ".join(changed))
    return 1 if reverted else 0


if __name__ == "__main__":
    sys.exit(main())
