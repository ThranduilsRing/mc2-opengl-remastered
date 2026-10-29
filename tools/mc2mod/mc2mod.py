"""
tools/mc2mod/mc2mod.py -- MC2 mod packager / installer (Slice S8 v1)
=====================================================================
Subcommands
-----------
  pack   <mod-dir> --out <dir>
      Produce <id>-<version>.mc2mod (zip) with embedded package.json.
      Dot-prefixed entries excluded (ruling C4).

  verify-lite <package>
      Re-hash every member vs package.json; schema check; exit nonzero on mismatch.

  install <package> --deploy <dir>
      Extract to <deploy>/mods/<id>/, write <deploy>/mods/<id>/.install-receipt.json.
      Refuses to write to canonical deploy roots (hard-coded guard).
      Prints matched-tuple advisory on success.

  uninstall <id> --deploy <dir>
      Remove installed files + empty dirs + receipt using receipt as ground-truth.
      Refuses if any installed file's hash differs from receipt (unless --force).

Contract refs:
  - mod-packaging-deploy-architecture.md §3 (package.json schema)
  - mod-packaging-deploy-architecture.md §4 (install flow)
  - mod-packaging-deploy-architecture.md §5 (receipt, rollback)
  - superpowers-execution-roadmap.md §5 S8 + ruling C6
  - ruling C4: unified dot-prefix exclusion from mod index / packages

Python 3 stdlib only.  No emoji.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

PACKAGE_SCHEMA = "mc2-package/1"
RECEIPT_SCHEMA = "mc2-install-receipt/1"
PACKAGE_EXT = ".mc2mod"

# Shared with the L1 VFS tools, also when invoked as a script.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mod_install import management

# Canonical deploy roots that the installer refuses to touch (ruling: canonical
# guard -- overridable only by --i-know-this-is-canonical which v1 does NOT
# implement).
_CANONICAL_DEPLOY_ROOTS: List[str] = [
    "a:/games/mc2-opengl/mc2-win64-v0.4",
    "a:/games/mc2-opengl/mc2-win64-0.4c",
    "a:/games/mc2-opengl/mc2-win64-v0.4c",
]

# Pinned zip-member timestamp for reproducible packs.  Without this, two pack
# runs over byte-identical payload produce different package sha256s: the
# generated package.json member gets stamped with wall-clock "now", and payload
# members inherit their source-file mtimes.  1980-01-01 is the zip format's
# minimum representable date_time.
_REPRO_ZIP_DATE: Tuple[int, int, int, int, int, int] = (1980, 1, 1, 0, 0, 0)

# ---------------------------------------------------------------------------
# Hashing helpers
# ---------------------------------------------------------------------------

def _sha256_file(path: str) -> str:
    """Return hex SHA-256 of the file at path."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# Hardening helpers (S17 validator hardening)
# ---------------------------------------------------------------------------

# id is used verbatim as the install directory name (<deploy>/mods/<id>/), so it
# must be a safe, lowercase, separator-free token.
_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
# version must be dotted-numeric with an optional pre-release/build suffix.
_VERSION_RE = re.compile(r"^\d+(\.\d+){0,3}([-+][0-9A-Za-z.-]+)?$")


def _version_key(v: str) -> Tuple[int, ...]:
    """Sortable key from a dotted-numeric version (pre-release/build suffix
    dropped). '1.2.0' > '1.1.9'. Non-numeric -> (-1,) so it never wins 'latest'."""
    core = re.split(r"[-+]", str(v), 1)[0]
    try:
        return tuple(int(p) for p in core.split("."))
    except ValueError:
        return (-1,)


def _unsafe_member_path(rel: Any) -> Optional[str]:
    """Return a reason if rel is unsafe to extract under an install root (zip-slip
    / path-traversal guard for cmd_install), else None."""
    if not isinstance(rel, str) or rel == "":
        return "empty or non-string path"
    if rel.startswith("/") or rel.startswith("\\"):
        return "absolute path (leading slash)"
    if "\\" in rel:
        return "backslash separator (use forward slashes)"
    if re.match(r"^[A-Za-z]:", rel):
        return "drive-letter absolute path"
    parts = rel.split("/")
    if ".." in parts:
        return "contains a '..' parent-traversal segment"
    if any(p == "" for p in parts):
        return "empty path segment ('//' or trailing '/')"
    # A drive-letter in ANY segment escapes on Windows: os.path.join(root,
    # "data\\C:\\evil") -> "C:\\evil" (inner absolute component drops the root).
    if any(re.match(r"^[A-Za-z]:", p) for p in parts):
        return "drive-letter in a path segment"
    return None


# ---------------------------------------------------------------------------
# Canonical guard
# ---------------------------------------------------------------------------

def _is_canonical_deploy(deploy_dir: str) -> bool:
    """
    Return True if deploy_dir resolves to one of the canonical game deploys
    that must never be written to by the installer.
    """
    try:
        resolved = Path(deploy_dir).resolve().as_posix().lower()
    except Exception:
        resolved = deploy_dir.replace("\\", "/").lower().rstrip("/")
    for canonical in _CANONICAL_DEPLOY_ROOTS:
        if resolved == canonical.rstrip("/"):
            return True
    return False


# ---------------------------------------------------------------------------
# mod.json reader (minimal -- only id + name + version extracted)
# ---------------------------------------------------------------------------

def _read_mod_json(mod_dir: str) -> Tuple[str, str, str]:
    """
    Return (id, name, version) from <mod_dir>/mod.json.
    version defaults to "0.0.0" if absent (most mods omit it).
    """
    json_path = os.path.join(mod_dir, "mod.json")
    try:
        with open(json_path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        mod_id = str(data.get("id", "")).strip()
        name = str(data.get("name", "")).strip()
        version = str(data.get("version", "0.0.0")).strip()
        if not mod_id:
            mod_id = Path(mod_dir).name
        return mod_id, name, version
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"ERROR: cannot read {json_path}: {exc}") from exc


# ---------------------------------------------------------------------------
# Subcommand: pack
# ---------------------------------------------------------------------------

def _collect_mod_files(mod_dir: str) -> List[Tuple[str, str]]:
    """
    Walk mod_dir, excluding:
      - dot-prefixed files and directories (ruling C4)
      - src/ directory (cook sources, never packaged)
      - out/ directory (regeneratable cook artifacts)
    Returns list of (abs_path, rel_path_forward_slash) sorted by rel_path.
    """
    results: List[Tuple[str, str]] = []
    excluded_dirs = {"src", "out"}
    base = Path(mod_dir)

    for root, dirs, files in os.walk(str(base)):
        root_rel = Path(root).relative_to(base)
        # Prune excluded top-level dirs and all dot-prefixed dirs (ruling C4)
        dirs[:] = [
            d for d in dirs
            if not d.startswith(".")
            and not (root_rel == Path(".") and d in excluded_dirs)
        ]

        for fname in files:
            if fname.startswith("."):
                continue  # ruling C4: exclude dot-prefixed files
            abs_path = os.path.join(root, fname)
            rel = Path(abs_path).relative_to(base)
            rel_fwd = rel.as_posix()
            results.append((abs_path, rel_fwd))

    results.sort(key=lambda x: x[1])
    return results


def _write_zip_member(zf: zipfile.ZipFile, arcname: str, data: bytes) -> None:
    """
    Write one archive member with pinned metadata (timestamp + permissions),
    so the archive bytes depend only on member names + member contents --
    never on wall-clock time or source-file mtimes.  This is what makes
    `pack` reproducible: same payload in, same package sha256 out.
    """
    zinfo = zipfile.ZipInfo(arcname, date_time=_REPRO_ZIP_DATE)
    zinfo.compress_type = zipfile.ZIP_DEFLATED
    zinfo.external_attr = 0o644 << 16  # rw-r--r--, regular file
    zf.writestr(zinfo, data)


def pack_mod(mod_dir: str, out_dir: Path) -> Path:
    """Pack a mod directory into <out_dir>/<id>-<version>.mc2mod and return the
    package path. Shared by cmd_pack and cmd_publish. Reproducible: all members
    written via _write_zip_member (pinned timestamps/permissions)."""
    if (Path(mod_dir) / ".scratch").exists():
        raise ValueError(f"{Path(mod_dir).name}: .scratch exists; run tools/mod_scratch.py promote/discard")
    mod_id, name, version = _read_mod_json(mod_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    package_path = out_dir / f"{mod_id}-{version}{PACKAGE_EXT}"

    files_meta: List[Dict[str, Any]] = []
    collected = _collect_mod_files(mod_dir)
    for abs_path, rel_fwd in collected:
        if rel_fwd == "package.json":
            continue  # will be replaced by a freshly generated one
        files_meta.append({
            "path": rel_fwd,
            "sha256": _sha256_file(abs_path),
            "size": os.path.getsize(abs_path),
        })

    package_json_bytes = json.dumps({
        "schema": PACKAGE_SCHEMA,
        "id": mod_id,
        "name": name,
        "version": version,
        "files": files_meta,
        "dependencies": [{"id": dep, "version": "*"} for dep in
                         json.loads((Path(mod_dir) / "mod.json").read_text(encoding="utf-8"))
                         .get("dependencies", [])],
    }, indent=2).encode("utf-8")

    # Write zip.  All members go through _write_zip_member so timestamps and
    # permissions are pinned (reproducible pack: identical payload bytes =>
    # identical package sha256, regardless of when or where you run pack).
    # Members are read fully into memory; mod payloads are data files, small
    # enough that this is fine for v1.
    with zipfile.ZipFile(str(package_path), "w", compression=zipfile.ZIP_DEFLATED) as zf:
        # Write generated package.json first
        _write_zip_member(zf, "package.json", package_json_bytes)
        # Write all other mod files
        for abs_path, rel_fwd in collected:
            if rel_fwd == "package.json":
                continue  # replaced above
            with open(abs_path, "rb") as fh:
                _write_zip_member(zf, rel_fwd, fh.read())
    return package_path


def cmd_pack(args: argparse.Namespace) -> int:
    mod_dir = str(Path(args.mod_dir).resolve())
    if not os.path.isdir(mod_dir):
        print(f"ERROR: mod directory not found: {mod_dir}", file=sys.stderr)
        return 1
    out_dir = Path(args.out).resolve() if args.out else Path(mod_dir).parent
    package_path = pack_mod(mod_dir, out_dir)
    mod_id, _name, version = _read_mod_json(mod_dir)
    # count matches the archived file set (package.json is regenerated, not counted)
    file_count = sum(1 for _abs, rel in _collect_mod_files(mod_dir) if rel != "package.json")
    print(f"pack: {package_path}")
    print(f"  id={mod_id}  version={version}  "
          f"files={file_count}  sha256={_sha256_file(str(package_path))}")
    return 0


# ---------------------------------------------------------------------------
# Subcommand: verify-lite
# ---------------------------------------------------------------------------

def _load_package_json_from_zip(zf: zipfile.ZipFile) -> Dict[str, Any]:
    """Extract and parse package.json from an open ZipFile."""
    try:
        raw = zf.read("package.json")
    except KeyError:
        raise SystemExit("ERROR: package.json missing from archive")
    try:
        return json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"ERROR: package.json is malformed JSON: {exc}") from exc


def cmd_verify_lite(args: argparse.Namespace) -> int:
    pkg_path = args.package
    if not os.path.isfile(pkg_path):
        print(f"ERROR: package not found: {pkg_path}", file=sys.stderr)
        return 1

    errors: List[str] = []

    with zipfile.ZipFile(pkg_path, "r") as zf:
        pkg = _load_package_json_from_zip(zf)

        # Schema check
        schema = pkg.get("schema", "")
        if schema != PACKAGE_SCHEMA:
            errors.append(f"schema mismatch: got '{schema}', want '{PACKAGE_SCHEMA}'")

        required_fields = ("id", "name", "version", "files")
        for field in required_fields:
            if field not in pkg:
                errors.append(f"missing required field: '{field}'")

        files_list = pkg.get("files", [])
        if not isinstance(files_list, list):
            errors.append("'files' is not a list")
            files_list = []

        # Re-hash every declared member
        zip_names = set(zf.namelist())
        for entry in files_list:
            rel = entry.get("path", "")
            declared_sha = entry.get("sha256", "")
            declared_size = entry.get("size", -1)

            if rel not in zip_names:
                errors.append(f"declared file missing from archive: {rel}")
                continue

            data = zf.read(rel)
            actual_sha = _sha256_bytes(data)
            actual_size = len(data)

            if actual_sha != declared_sha:
                errors.append(
                    f"sha256 mismatch for {rel}: declared={declared_sha[:16]}... "
                    f"actual={actual_sha[:16]}..."
                )
            if actual_size != declared_size:
                errors.append(
                    f"size mismatch for {rel}: declared={declared_size} actual={actual_size}"
                )

        # Check for members not declared in files[] (package.json itself is excluded)
        declared_paths = {e.get("path", "") for e in files_list}
        for name in zip_names:
            if name == "package.json":
                continue
            if name not in declared_paths:
                errors.append(f"archive member not declared in files[]: {name}")

        # --- Hardening (S17) -------------------------------------------------
        # id safety: it becomes the install dir name (<deploy>/mods/<id>/).
        mod_id = pkg.get("id", "")
        if isinstance(mod_id, str) and mod_id and not _ID_RE.match(mod_id):
            errors.append(
                f"unsafe id '{mod_id}': must match {_ID_RE.pattern} "
                "(lowercase token, no spaces/separators/'..'; used as a dir name)")

        # version must be dotted-numeric (semver-ish).
        version = pkg.get("version", "")
        if not (isinstance(version, str) and _VERSION_RE.match(version)):
            errors.append(f"version '{version}' is not dotted-numeric (e.g. 1.0.0)")

        # dependencies, if present, must be a list.
        if "dependencies" in pkg and not isinstance(pkg.get("dependencies"), list):
            errors.append("'dependencies' is present but is not a list")

        # Per-member path safety (zip-slip / traversal) + duplicate detection.
        seen_paths = set()
        for entry in files_list:
            rel = entry.get("path", "")
            reason = _unsafe_member_path(rel)
            if reason:
                errors.append(f"unsafe member path '{rel}': {reason}")
            if rel in seen_paths:
                errors.append(f"duplicate member path in files[]: {rel}")
            seen_paths.add(rel)

    if errors:
        print(f"verify-lite FAILED: {len(errors)} error(s)")
        for err in errors:
            print(f"  ERROR: {err}")
        return 1

    pkg_sha = _sha256_file(pkg_path)
    print(
        f"verify-lite PASS: id={pkg.get('id')}  version={pkg.get('version')}  "
        f"files={len(files_list)}  pkg_sha256={pkg_sha}"
    )
    return 0


# ---------------------------------------------------------------------------
# Matched-tuple advisory
# ---------------------------------------------------------------------------

def _print_advisory(deploy_dir: str, mod_id: str, pkg_sha: str, pkg: Dict[str, Any]) -> None:
    """
    Print the matched-tuple advisory after a successful install.
    Advisory only -- never blocks.
    """
    print("")
    print("=== MATCHED-TUPLE ADVISORY ===")

    # exe sha256
    exe_path = os.path.join(deploy_dir, "mc2.exe")
    if os.path.isfile(exe_path):
        exe_sha = _sha256_file(exe_path)
        print(f"  exe_sha256     : {exe_sha}  ({exe_path})")
    else:
        print(f"  exe_sha256     : ABSENT ({exe_path})")

    # shader payload check
    shaders_dir = os.path.join(deploy_dir, "shaders")
    if os.path.isdir(shaders_dir):
        shader_files = [
            f for f in os.listdir(shaders_dir)
            if os.path.isfile(os.path.join(shaders_dir, f))
        ]
        shader_count = len(shader_files)
        if shader_files:
            newest_mtime = max(
                os.path.getmtime(os.path.join(shaders_dir, f)) for f in shader_files
            )
            newest_dt = datetime.datetime.utcfromtimestamp(newest_mtime).strftime(
                "%Y-%m-%dT%H:%M:%SZ"
            )
        else:
            newest_dt = "N/A"
        print(f"  shaders        : {shader_count} files, newest_mtime={newest_dt}")
    else:
        print(f"  shaders        : ABSENT ({shaders_dir})")

    # active mod
    print(f"  active_mod_id  : {mod_id}")

    # package sha256
    print(f"  package_sha256 : {pkg_sha}")

    # registry index
    registry_path = os.path.join(deploy_dir, ".registry", "registry-index.json")
    if os.path.isfile(registry_path):
        reg_sha = _sha256_file(registry_path)
        reg_mtime = datetime.datetime.utcfromtimestamp(
            os.path.getmtime(registry_path)
        ).strftime("%Y-%m-%dT%H:%M:%SZ")
        print(f"  registry_index : present  mtime={reg_mtime}  sha256={reg_sha[:16]}...")
    else:
        print(f"  registry_index : absent")

    print("==============================")
    print("")


# ---------------------------------------------------------------------------
# Subcommand: install
# ---------------------------------------------------------------------------

def _install_staged(args: argparse.Namespace) -> int:
    pkg_path = args.package
    deploy_dir = str(Path(args.deploy).resolve())

    if not os.path.isfile(pkg_path):
        print(f"ERROR: package not found: {pkg_path}", file=sys.stderr)
        return 1

    # Canonical guard
    if _is_canonical_deploy(deploy_dir):
        print(
            f"ERROR: refusing to install into canonical deploy root: {deploy_dir}",
            file=sys.stderr,
        )
        print(
            "  Canonical deploy roots are read-only targets for the installer.",
            file=sys.stderr,
        )
        print(
            "  Use a separate temp/staging deploy directory.",
            file=sys.stderr,
        )
        return 1

    pkg_sha = _sha256_file(pkg_path)

    with zipfile.ZipFile(pkg_path, "r") as zf:
        pkg = _load_package_json_from_zip(zf)

        schema = pkg.get("schema", "")
        if schema != PACKAGE_SCHEMA:
            print(
                f"ERROR: unsupported package schema '{schema}' (expected '{PACKAGE_SCHEMA}')",
                file=sys.stderr,
            )
            return 1

        mod_id = pkg.get("id", "")
        if not mod_id:
            print("ERROR: package.json missing 'id' field", file=sys.stderr)
            return 1

        install_root = os.path.join(deploy_dir, "mods", mod_id)
        os.makedirs(install_root, exist_ok=True)

        files_list: List[Dict[str, Any]] = pkg.get("files", [])

        # Extract all declared files. Guard zip-slip at the point of use: install
        # may run on a package that was never verify-lite'd, so re-check each path
        # AND confirm the resolved destination stays under install_root.
        install_root_real = os.path.realpath(install_root)
        for entry in files_list:
            rel = entry["path"]
            reason = _unsafe_member_path(rel)
            if reason:
                print(f"ERROR: refusing unsafe member path '{rel}': {reason}", file=sys.stderr)
                return 1
            dest = os.path.join(install_root, rel.replace("/", os.sep))
            if os.path.commonpath([os.path.realpath(os.path.dirname(dest) or install_root),
                                    install_root_real]) != install_root_real:
                print(f"ERROR: member '{rel}' escapes install root; aborting", file=sys.stderr)
                return 1
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            data = zf.read(rel)
            with open(dest, "wb") as fh:
                fh.write(data)

        # Also extract package.json itself (it describes what is installed)
        pkg_json_dest = os.path.join(install_root, "package.json")
        pkg_json_bytes = json.dumps(pkg, indent=2).encode("utf-8")
        with open(pkg_json_dest, "wb") as fh:
            fh.write(pkg_json_bytes)

    # Write receipt (C6: receipt records what was installed when from which
    # package hash -- NOT a copy of package.json)
    install_time = datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")
    receipt: Dict[str, Any] = {
        "schema": RECEIPT_SCHEMA,
        "id": mod_id,
        "version": pkg.get("version", ""),
        "installed_at": install_time,
        "package_sha256": pkg_sha,
        "install_root": install_root.replace("\\", "/"),
        "files": [
            {
                "path": e["path"],
                "sha256": e["sha256"],
                "size": e["size"],
            }
            for e in files_list
        ],
    }
    receipt_path = os.path.join(install_root, ".install-receipt.json")
    with open(receipt_path, "w", encoding="utf-8") as fh:
        json.dump(receipt, fh, indent=2)

    print(f"install: {mod_id} v{pkg.get('version', '')} -> {install_root}")
    print(f"  files installed : {len(files_list)}")
    print(f"  receipt         : {receipt_path}")

    _print_advisory(deploy_dir, mod_id, pkg_sha, pkg)
    return 0


# ---------------------------------------------------------------------------
# Subcommand: uninstall
# ---------------------------------------------------------------------------

def cmd_uninstall(args: argparse.Namespace) -> int:
    mod_id = args.id
    deploy_dir = str(Path(args.deploy).resolve())
    force = getattr(args, "force", False)

    install_root = os.path.join(deploy_dir, "mods", mod_id)
    receipt_path = os.path.join(install_root, ".install-receipt.json")

    if not os.path.isfile(receipt_path):
        print(f"ERROR: no install receipt found at {receipt_path}", file=sys.stderr)
        print("  Nothing to uninstall (or receipt was deleted).", file=sys.stderr)
        return 1

    with open(receipt_path, "r", encoding="utf-8") as fh:
        receipt = json.load(fh)

    schema = receipt.get("schema", "")
    if schema != RECEIPT_SCHEMA:
        print(
            f"ERROR: unrecognized receipt schema '{schema}'", file=sys.stderr
        )
        return 1

    files_list: List[Dict[str, Any]] = receipt.get("files", [])

    # Check for modifications unless --force
    modified: List[str] = []
    missing: List[str] = []
    for entry in files_list:
        rel = entry["path"]
        dest = os.path.join(install_root, rel.replace("/", os.sep))
        if not os.path.isfile(dest):
            missing.append(rel)
            continue
        actual_sha = _sha256_file(dest)
        if actual_sha != entry["sha256"]:
            modified.append(rel)

    if (modified or missing) and not force:
        print(
            f"ERROR: {len(modified)} modified and {len(missing)} missing files since install.",
            file=sys.stderr,
        )
        if modified:
            for p in modified[:5]:
                print(f"  MODIFIED: {p}", file=sys.stderr)
            if len(modified) > 5:
                print(f"  ... and {len(modified) - 5} more", file=sys.stderr)
        if missing:
            for p in missing[:5]:
                print(f"  MISSING: {p}", file=sys.stderr)
        print("  Use --force to uninstall anyway.", file=sys.stderr)
        return 1

    # Remove installed files
    removed_files = 0
    for entry in files_list:
        rel = entry["path"]
        dest = os.path.join(install_root, rel.replace("/", os.sep))
        if os.path.isfile(dest):
            os.remove(dest)
            removed_files += 1

    # Remove package.json written by installer
    pkg_json_in_install = os.path.join(install_root, "package.json")
    if os.path.isfile(pkg_json_in_install):
        os.remove(pkg_json_in_install)

    # Remove receipt
    if os.path.isfile(receipt_path):
        os.remove(receipt_path)

    # Remove empty directories bottom-up
    removed_dirs = 0
    for root, dirs, files in os.walk(install_root, topdown=False):
        if root == install_root:
            continue
        try:
            os.rmdir(root)
            removed_dirs += 1
        except OSError:
            pass  # not empty, leave it

    # Remove install root if empty
    try:
        os.rmdir(install_root)
        removed_dirs += 1
    except OSError:
        pass

    print(f"uninstall: {mod_id} from {deploy_dir}")
    print(f"  files removed   : {removed_files}")
    print(f"  dirs removed    : {removed_dirs}")
    if modified and force:
        print(f"  WARNING: {len(modified)} modified files were force-removed")
    return 0


# ---------------------------------------------------------------------------
# Subcommand: publish (S17 publish hook)
# ---------------------------------------------------------------------------

PUBLISH_REGISTRY_SCHEMA = "mc2-mod-registry/1"


def cmd_publish(args: argparse.Namespace) -> int:
    """
    Publish a mod into a LOCAL package registry: pack (if a mod-dir) -> verify-lite
    gate -> place the archive under <registry>/packages/ -> update the registry
    manifest <registry>/registry.json (schema mc2-mod-registry/1). Pure-local, no
    network. The deploy-side index (tools/registry/build_index.py) is the
    complementary install-side view; this is the distribution side.
    """
    registry = Path(args.registry).resolve()
    if _is_canonical_deploy(str(registry)):
        print(f"ERROR: refusing to publish into a canonical deploy root: {registry}",
              file=sys.stderr)
        return 1
    packages_dir = registry / "packages"
    packages_dir.mkdir(parents=True, exist_ok=True)

    # Resolve the package: pack a mod-dir, or use a prebuilt archive.
    packed_here = False
    if args.package:
        src_pkg = Path(args.package).resolve()
        if not src_pkg.is_file():
            print(f"ERROR: package not found: {src_pkg}", file=sys.stderr)
            return 1
    else:
        mod_dir = Path(args.mod_dir).resolve()
        if not mod_dir.is_dir():
            print(f"ERROR: mod directory not found: {mod_dir}", file=sys.stderr)
            return 1
        src_pkg = pack_mod(str(mod_dir), packages_dir)
        packed_here = True

    # verify-lite gate (the hardened validator). Abort publish on any failure.
    if cmd_verify_lite(argparse.Namespace(package=str(src_pkg))) != 0:
        print("publish ABORTED: verify-lite failed", file=sys.stderr)
        if packed_here:
            try:
                os.remove(src_pkg)  # do not leave an invalid package in the registry
            except OSError:
                pass
        return 1

    with zipfile.ZipFile(str(src_pkg), "r") as zf:
        pkg = _load_package_json_from_zip(zf)
    mod_id = pkg.get("id", "")
    version = pkg.get("version", "")

    dest_pkg = packages_dir / f"{mod_id}-{version}{PACKAGE_EXT}"
    if Path(src_pkg).resolve() != dest_pkg.resolve():
        shutil.copy2(str(src_pkg), str(dest_pkg))

    pkg_sha = _sha256_file(str(dest_pkg))
    entry = {
        "id": mod_id,
        "version": version,
        "name": pkg.get("name", ""),
        "package": dest_pkg.name,
        "sha256": pkg_sha,
        "size": os.path.getsize(dest_pkg),
        "dependencies": pkg.get("dependencies", []),
        "published_utc": args.utc,
    }

    manifest_path = registry / "registry.json"
    manifest: Dict[str, Any] = {"schema": PUBLISH_REGISTRY_SCHEMA,
                                "generated_utc": args.utc, "mods": {}}
    if manifest_path.is_file():
        try:
            loaded = json.loads(manifest_path.read_text(encoding="utf-8"))
            if loaded.get("schema") == PUBLISH_REGISTRY_SCHEMA and isinstance(loaded.get("mods"), dict):
                manifest = loaded
                manifest["generated_utc"] = args.utc
        except (OSError, ValueError):
            pass  # corrupt manifest -> rebuild fresh

    mod_rec = manifest["mods"].setdefault(mod_id, {"versions": {}})
    overwrote = version in mod_rec["versions"]
    mod_rec["versions"][version] = entry
    # 'latest' = highest version by dotted-numeric order (NOT most-recently
    # published), so a backport of an older version never clobbers the pointer.
    mod_rec["latest"] = max(mod_rec["versions"], key=_version_key)

    # Atomic write (temp + os.replace) so an interrupted/concurrent publish never
    # truncates registry.json -> the corrupt-read path would silently drop every
    # prior entry on the next run.
    tmp_path = manifest_path.with_name(manifest_path.name + ".tmp")
    tmp_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    os.replace(str(tmp_path), str(manifest_path))

    print(f"publish: {mod_id} v{version} -> {dest_pkg}")
    print(f"  registry        : {manifest_path}"
          + ("  (overwrote existing version)" if overwrote else ""))
    print(f"  package_sha256  : {pkg_sha}")
    return 0


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def cmd_install(args: argparse.Namespace) -> int:
    return management.install_retained(args, _install_staged, _is_canonical_deploy, cmd_verify_lite)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mc2mod",
        description="MC2 mod packager / installer (S8 v1)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="List installed versions and legacy folders")
    p_list.add_argument("--deploy", required=True)
    p_rollback = sub.add_parser("rollback", help="Restore a verified retained version")
    p_rollback.add_argument("id")
    p_rollback.add_argument("--to", required=True)
    p_rollback.add_argument("--deploy", required=True)
    p_check = sub.add_parser("check", help="Predict active-chain path and models.json conflicts")
    p_check.add_argument("--deploy", required=True)
    p_check.add_argument("--active", required=True)
    p_check.add_argument("--deps", default="", help="MC2_MOD_DEPS, comma/semicolon separated")
    p_check.add_argument("--log", help="Compare prediction to a fresh engine mod-dup log")

    # pack
    p_pack = sub.add_parser("pack", help="Pack a mod directory into a .mc2mod archive")
    p_pack.add_argument("mod_dir", help="Path to the mod project directory")
    p_pack.add_argument("--out", metavar="DIR", help="Output directory (default: parent of mod_dir)")

    # verify-lite
    p_verify = sub.add_parser("verify-lite", help="Verify archive integrity vs embedded package.json")
    p_verify.add_argument("package", help="Path to .mc2mod archive")

    # install
    p_install = sub.add_parser("install", help="Install a .mc2mod package into a deploy directory")
    p_install.add_argument("package", help="Path to .mc2mod archive")
    p_install.add_argument("--deploy", required=True, metavar="DIR", help="Deploy root directory")

    # uninstall
    p_uninstall = sub.add_parser("uninstall", help="Uninstall a mod from a deploy directory")
    p_uninstall.add_argument("id", help="Mod id to uninstall")
    p_uninstall.add_argument("--deploy", required=True, metavar="DIR", help="Deploy root directory")
    p_uninstall.add_argument("--force", action="store_true", help="Remove even if files were modified")

    # publish
    p_publish = sub.add_parser("publish", help="Publish a mod into a local package registry")
    src = p_publish.add_mutually_exclusive_group(required=True)
    src.add_argument("--mod-dir", metavar="DIR", help="Mod project dir to pack then publish")
    src.add_argument("--package", metavar="FILE", help="Prebuilt .mc2mod to publish")
    p_publish.add_argument("--registry", required=True, metavar="DIR", help="Registry root directory")
    p_publish.add_argument("--utc", default="", metavar="STAMP",
                           help="published_utc stamp (default empty = byte-stable manifest)")

    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    dispatch = {
        "pack": cmd_pack,
        "verify-lite": cmd_verify_lite,
        "install": cmd_install,
        "uninstall": cmd_uninstall,
        "publish": cmd_publish,
        "list": management.list_mods,
        "rollback": lambda ns: management.rollback(ns, _is_canonical_deploy),
        "check": management.check,
    }
    fn = dispatch.get(args.command)
    if fn is None:
        print(f"ERROR: unknown command '{args.command}'", file=sys.stderr)
        return 1
    try:
        return fn(args)
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
