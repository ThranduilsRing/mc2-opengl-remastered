"""List, retain, restore and inspect mod folders. No engine registration writes."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import uuid

from .resolver import ResolverConfig, _index_mod_layer, _read_mod_json, parse_mod_deps
from validate_mod_package import ID, VERSION, file_hash, read_json, validate


def list_mods(args):
    root = Path(args.deploy) / "mods"
    if not root.is_dir():
        print(f"list: {args.deploy}: no mods directory (legacy deploy)")
        return 0
    for path in sorted(root.iterdir()):
        if not path.is_dir() or path.name.startswith("."):
            continue
        pkg = read_json(path / "package.json") if (path / "package.json").exists() else {}
        receipt = (path / ".install-receipt.json").is_file()
        print(f"{path.name}\t{pkg.get('version', 'legacy')}\treceipt={'yes' if receipt else 'no'}")
    return 0


def checked_root(deploy):
    """Mutation commands may only rename physical children of mods/."""
    root = Path(deploy).resolve() / "mods"
    if root.exists() and root.resolve() != root:
        raise ValueError("mods directory must not be a link")
    return root


def physical_child(path, root):
    if not path.resolve().is_relative_to(root.resolve()) or path.is_symlink() or path.resolve() != path.absolute():
        raise ValueError(f"refusing linked/escaped mod path: {path}")


def verified_folder(path, mod_id=None):
    errors = validate(path, check_files=True)
    if errors:
        raise ValueError(f"{path}: {'; '.join(errors)}")
    pkg = read_json(path / "package.json")
    if mod_id is not None and pkg["id"] != mod_id:
        raise ValueError("retained package id does not match requested mod")
    receipt = read_json(path / ".install-receipt.json")
    if (receipt.get("schema") != "mc2-install-receipt/1" or receipt.get("id") != pkg["id"]
            or receipt.get("version") != pkg["version"] or receipt.get("files") != pkg["files"]):
        raise ValueError("receipt does not match package")
    if receipt.get("shared_mutations") or receipt.get("shared_file_mutations"):
        raise ValueError("shared-file mutations cannot be folder-rolled-back")
    # Never move junctions/symlinks, including links nested inside a mod.
    for directory, dirs, files in os.walk(path):
        for name in dirs + files:
            physical_child(Path(directory) / name, path)
    return pkg


def swap_version(root, current, replacement, old_version):
    trash = root / ".trash"
    physical_child(trash, root)
    trash.mkdir(exist_ok=True)
    archived = trash / f"{current.name}-{old_version}-{uuid.uuid4().hex}"
    if current.exists():
        current.rename(archived)
    try:
        replacement.rename(current)
    except OSError:
        if archived.exists():
            archived.rename(current)
        raise
    return archived


def install_retained(args, installer, canonical_guard, verify_archive):
    if canonical_guard(args.deploy):
        raise ValueError("refusing canonical deploy")
    if verify_archive(argparse.Namespace(package=args.package)):
        return 1
    root = checked_root(args.deploy)
    root.mkdir(parents=True, exist_ok=True)
    staging = root / ".staging"
    physical_child(staging, root)
    staging.mkdir(exist_ok=True)
    stage = Path(tempfile.mkdtemp(dir=staging))
    try:
        # The existing installer owns extraction and receipt creation. Its deploy
        # is private staging, so a failed verification cannot damage a live mod.
        result = installer(argparse.Namespace(package=args.package, deploy=str(stage)))
        if result:
            return result
        candidates = list((stage / "mods").iterdir())
        if len(candidates) != 1:
            raise ValueError("expected one staged mod")
        replacement = candidates[0]
        pkg = verified_folder(replacement)
        current = root / pkg["id"]
        physical_child(current, root)
        old = verified_folder(current, pkg["id"])["version"] if current.exists() else "none"
        receipt_path = replacement / ".install-receipt.json"
        receipt = read_json(receipt_path)
        receipt["install_root"] = current.as_posix()
        receipt_path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        swap_version(root, current, replacement, old)
        print(f"installed: {pkg['id']} {pkg['version']} -> {current}")
        return 0
    finally:
        shutil.rmtree(stage)
        if not any(staging.iterdir()):
            staging.rmdir()


def rollback(args, canonical_guard):
    if canonical_guard(args.deploy):
        raise ValueError("refusing canonical deploy")
    if not ID.fullmatch(args.id) or not VERSION.fullmatch(args.to):
        raise ValueError("invalid id/version")
    root = checked_root(args.deploy)
    current = root / args.id
    physical_child(current, root)
    old = verified_folder(current, args.id)
    trash = root / ".trash"
    physical_child(trash, root)
    matches = []
    for path in sorted(trash.iterdir()) if trash.is_dir() else []:
        if not path.is_dir() or not (path / "package.json").is_file():
            continue
        pkg = read_json(path / "package.json")
        if pkg.get("id") == args.id and pkg.get("version") == args.to:
            matches.append(path)
    if len(matches) != 1:
        raise ValueError(f"expected one retained {args.id} {args.to}; found {len(matches)}")
    replacement = matches[0]
    physical_child(replacement, trash)
    verified_folder(replacement, args.id)
    swap_version(root, current, replacement, old["version"])
    print(f"rollback: {args.id} {old['version']} -> {args.to}")
    return 0


def conflict_lines(deploy, active, dependencies=None):
    """Predict through the merged L1 resolver, using live bytes even for legacy mods."""
    # Runtime folder names predate package ids and may contain uppercase letters.
    folder_id = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\Z")
    if not folder_id.fullmatch(active):
        raise ValueError("invalid active mod id")
    root = Path(deploy) / "mods"
    if not (root / active / "data").is_dir():
        raise ValueError("active mod data directory missing")
    effective, _, declared = _read_mod_json(str(root / active / "mod.json"))
    deps = list(declared)
    for dep in dependencies or []:
        if dep not in deps:
            deps.append(dep)
    if any(not folder_id.fullmatch(dep) for dep in deps):
        raise ValueError("invalid dependency id")
    config = ResolverConfig(str(deploy), active_mod=active, dependencies=dependencies, fst_files=[])
    layers = []
    effective = effective or active
    if not os.environ.get("MC2_SCRATCH", "1").startswith("0"):
        layers.append((effective + "#scratch", root / active / ".scratch" / "data"))
    layers.append((effective, root / active / "data"))
    layers.extend((dep, root / dep / "data") for dep in deps)
    providers = {}
    semantic = {}
    lines = []
    for label, data in layers:
        if not data.is_dir():
            if not label.endswith("#scratch"):
                lines.append(f"WARN missing dependency: {label}")
            continue
        index = {}
        _index_mod_layer(index, {}, str(data), label)
        for key, (path, _) in index.items():
            providers[(label, key)] = path
        models = index.get("data/model_overrides/models.json")
        if models:
            overrides = read_json(models[0]).get("overrides", [])
            if not isinstance(overrides, list) or any(not isinstance(e, dict) for e in overrides):
                raise ValueError(f"{models[0]}: overrides must be an array of objects")
            for entry in overrides:
                key = entry.get("replaces") or f"{entry.get('class', '')}:{entry.get('appearanceName', '')}"
                if not isinstance(key, str) or ":" not in key or key == ":":
                    raise ValueError(f"{models[0]}: invalid override key")
                key = key.lower()
                if key in semantic:
                    lines.append(f"WARN models key {key} winner=[{semantic[key]}] loser=[{label}]")
                else:
                    semantic[key] = label
    for key, losers in sorted(config.shadowed.items()):
        path, winner = config.mod_index[key]
        for loser in losers:
            lines.append(f"[mod-dup] {key}  winner=[{winner}]  loser=[{loser}]")
            other = providers.get((loser, key))
            identical = other and file_hash(path) == file_hash(other)
            lines.append(f"{'INFO identical' if identical else 'WARN different'} {key}")
    return lines


def check(args):
    lines = conflict_lines(args.deploy, args.active, parse_mod_deps(args.deps))
    for line in lines:
        print(line)
    if args.log:
        actual = Counter(re.findall(r"\[mod-dup\] [^\r\n]+", Path(args.log).read_text(encoding="utf-8-sig")))
        predicted = Counter(line for line in lines if line.startswith("[mod-dup]"))
        if actual != predicted:
            print(f"FAIL mod-dup parity: missing={dict(predicted - actual)} unexpected={dict(actual - predicted)}")
            return 1
        print(f"PASS mod-dup parity: {sum(predicted.values())} collisions")
    return 0
