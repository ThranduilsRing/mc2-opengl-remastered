"""Inspect, promote or discard mods/<mod>/.scratch (layer 0).

Usage: py -3 tools/mod_scratch.py diff|promote|discard <mod> [--force]
<mod> is an id under ./mods or a path to a mod directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys

try:
    from tools.mod_install.resolver import load_config, _index_mod_layer, _read_mod_json, normalize_key
    from tools import fs_links
except ModuleNotFoundError:
    from mod_install.resolver import load_config, _index_mod_layer, _read_mod_json, normalize_key
    import fs_links


def scratch_files(mod: Path) -> list[Path]:
    root = mod / ".scratch"
    if root.is_symlink() or (root.exists() and root.resolve().parent != mod.resolve()):
        raise ValueError("scratch must be a directory inside the mod")
    files = []
    for folder, dirs, names in os.walk(root / "data"):
        for name in dirs + names:
            if fs_links.is_link_or_junction(Path(folder) / name):
                raise ValueError("scratch must not contain symlinks")
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        files.extend(Path(folder) / n for n in sorted(names) if not n.startswith("."))
    return files


def run(action: str, mod: Path, force: bool = False) -> None:
    mod = mod.resolve()
    if not mod.is_dir():
        raise ValueError(f"mod directory not found: {mod}")
    files = scratch_files(mod)
    scratch = mod / ".scratch"
    if action == "discard":
        if scratch.exists():
            shutil.rmtree(scratch)
        return
    project, duplicates = {}, {}
    _index_mod_layer(project, duplicates, str(mod / "data"), mod.name)
    planned = []
    for source in files:
        relative = source.relative_to(scratch)
        key = normalize_key(relative.as_posix())
        target = Path(project[key][0]) if key in project else mod / relative
        if not target.resolve().is_relative_to(mod):
            raise ValueError(f"destination escapes mod: {relative}")
        status = "new" if not target.exists() else (
            "same" if source.read_bytes() == target.read_bytes() else "changed")
        planned.append((source, target, key))
        if action == "diff":
            print(f"{status} {relative.as_posix()}")
    if action == "diff":
        return
    config = load_config(str(mod.parent.parent), mods_root=str(mod.parent), active_mod=mod.name)
    active_id = _read_mod_json(str(mod / "mod.json"))[0] or mod.name
    active_ids = {active_id, active_id + "#scratch"}
    conflicts = []
    seen = set()
    for _, _, key in planned:
        owners = ([config.mod_index[key][1]] if key in config.mod_index else [])
        owners += config.shadowed.get(key, [])
        if key in seen or any(owner not in active_ids for owner in owners):
            conflicts.append(key)
        seen.add(key)
    if conflicts and not force:
        raise ValueError("[mod-dup] " + ", ".join(conflicts) + "; use --force to promote")
    for source, target, _ in planned:
        target.parent.mkdir(parents=True, exist_ok=True)
        source.replace(target)
    if planned:
        os.utime(mod / "data", None)
    # Remove empty scratch directories, retaining hidden/unpromoted content.
    if scratch.exists():
        for folder, _, _ in os.walk(scratch, topdown=False):
            if not any(Path(folder).iterdir()):
                Path(folder).rmdir()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("diff", "promote", "discard"))
    parser.add_argument("mod")
    parser.add_argument("--force", action="store_true", help="promote despite [mod-dup] conflicts")
    args = parser.parse_args(argv)
    mod = Path(args.mod)
    if not mod.is_dir():
        mod = Path("mods") / args.mod
    try:
        run(args.action, mod, args.force)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
