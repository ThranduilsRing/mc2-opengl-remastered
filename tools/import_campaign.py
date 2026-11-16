"""Import a user-supplied campaign ZIP or installed folder; never run installers."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import struct
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent / "mc2_import"))
import import_tool as legacy
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import console_child
from smoke_lib.child_registry import pid_alive


def fst_entries(path):
    try:
        entries = legacy.fst.fst_entries(str(path))
        if any(off < 8 + len(entries) * 266 or off + comp > len(data)
               for off, comp, real, name, lz, data in entries):
            raise ValueError("entry outside archive")
        with Path(path).open("rb") as stream:
            if stream.read(4) not in (b"\xaf\xec\xdd\xfa", b"\xaf\xec\xdd\xca"):
                raise ValueError("invalid FST signature")
        return entries
    except (struct.error, ValueError) as exc:
        raise ValueError(f"Invalid FST {path}: {exc}") from exc


def safe_path(name):
    name = name.replace("\\", "/")
    parts = PurePosixPath(name).parts
    if (not parts or name.startswith("/") or any(
            p in (".", "..") or re.search(r'[<>:"|?*\x00-\x1f]', p)
            or p.endswith((".", " "))
            or p.split(".")[0].upper() in {"CON", "PRN", "AUX", "NUL",
                *[f"COM{i}" for i in range(1, 10)], *[f"LPT{i}" for i in range(1, 10)]}
            for p in parts)):
        raise ValueError(f"Unsafe content path: {name!r}")
    return "/".join(parts)


def source_files(root):
    """Only inputs used by the converters participate in content identity."""
    result = {}
    flat = (root / "campaign").is_dir() and (root / "missions").is_dir()
    for directory, dirs, files in os.walk(root, followlinks=False):
        if Path(directory) == root and not flat:
            dirs[:] = [name for name in dirs if name.lower() == "data"]
        for name in dirs + files:
            p = Path(directory) / name
            if p.is_symlink() or p.is_junction():
                raise ValueError(f"Linked source paths are unsupported: {p}")
        for name in files:
            p = Path(directory) / name
            rel = safe_path(p.relative_to(root).as_posix())
            if flat:
                rel = "data/" + rel
            if rel.lower().startswith("data/") or ("/" not in rel and
                    (p.suffix.lower() == ".fst" or name.lower() == "mc2xres.dll")):
                key = rel.lower()
                if key in result:
                    raise ValueError(f"Duplicate content path: {rel}")
                result[key] = p
    return result


def digest(files):
    h = hashlib.sha256()
    for name, path in sorted(files.items()):
        h.update(name.encode("utf-8") + b"\0")
        with path.open("rb") as stream:
            file_hash = hashlib.file_digest(stream, "sha256").digest()
        h.update(file_hash)
    return h.hexdigest()


def available_assets(root):
    names = set(source_files(root))
    for path in root.glob("*.fst"):
        for entry in fst_entries(path):
            if entry[3]:
                names.add(safe_path(entry[3]).lower())
    return names


def inspect_source(root):
    files = source_files(root)
    entries = {}
    for name, path in files.items():
        if name.endswith(".fst"):
            for off, comp, real, entry, lz, data in fst_entries(path):
                if not entry:
                    continue
                key = safe_path(entry).lower()
                entries[key] = (off, comp, real, lz, data)
    names = set(files) | set(entries)
    campaigns = []
    for name in sorted(names):
        if not (name.startswith("data/campaign/") and name.endswith(".fit")):
            continue
        raw = files[name].read_bytes() if name in files else legacy.fst.decompress_entry(*entries[name])
        text = raw.decode("latin1")
        first = re.search(r'\[Group0Mission0\](.*?)(?:\[|$)', text, re.S | re.I)
        mission = re.search(r'\bFileName\s*=\s*"([^"/\\]+)"', first[1], re.I) if first else None
        if not mission:
            raise ValueError(f"{name}: missing [Group0Mission0] FileName")
        title = re.search(r'\bCampaignName\s*=\s*"([^"]+)"', text, re.I)
        fallback = Path(name).stem
        if fallback == "campaign":
            fallback = "Carver V (legacy)" if mission[1].lower() == "mc2_01" else mission[1]
        campaigns.append({"fit": Path(name).stem, "name": (title[1] if title else fallback).replace("MechWarriror", "MechWarrior"),
                          "first_mission": mission[1].lower()})
    if not campaigns:
        raise ValueError("No campaign FIT found in data/campaign or FSTs. Select the campaign root, not data/.")
    return files, names, campaigns


def fill_campaign_object_slots(root, out, refs):
    """Fill compatibility-table holes from the campaign's authored object definitions."""
    buildings = root / "data/art/buildings.csv"
    base = next((Path(ref) / "objects/object2.pak" for ref in refs
                 if (Path(ref) / "objects/object2.pak").is_file()), None)
    if not buildings.is_file() or base is None:
        return
    sys.path.insert(0, str(Path(__file__).resolve().parent / "mco_import"))
    from build_object2 import read_pak, write_pak, parse_buildings, find_loose_fit, mech_stub, RAW, NUL
    _, slots = read_pak(str(base))
    changed = False
    for name, kind, fid in parse_buildings(str(buildings)):
        if fid < 1188 or (fid < len(slots) and slots[fid][0] != NUL):
            continue
        definition = find_loose_fit(str(root / "data/objects"), name)
        if kind != "MECH" and definition is None:
            continue
        payload = mech_stub(name) if kind == "MECH" else Path(definition).read_bytes()
        if fid >= len(slots):
            slots.extend([(NUL, b"")] * (fid + 1 - len(slots)))
        slots[fid] = (RAW, payload)
        changed = True
    if changed:
        destination = out / "data/objects/object2.pak"
        destination.parent.mkdir(parents=True, exist_ok=True)
        write_pak(str(destination), slots)


def asset_bytes(layers, name):
    """Resolve a loose or archived asset, campaign before dependencies before base."""
    for layer in layers:
        path = layer / name
        if path.is_file():
            return path.read_bytes()
        for archive in layer.glob("*.fst"):
            for off, comp, real, entry, lz, data in fst_entries(archive):
                if entry.replace("\\", "/").lower() == name:
                    return legacy.fst.decompress_entry(off, comp, real, lz, data)
    return None


def validate_mc2x_tables(root, files, campaigns, layers):
    """Check ownership and packet occupancy; this does not prove appearance correctness."""
    table_name = "data/objects/object2.pak"
    # A full source must supply its own table; mission-only addons may inherit compat.
    own = asset_bytes([root], table_name)
    full = "mc2xres.dll" in files or all(n in files for n in ("art.fst", "misc.fst", "tgl.fst"))
    packed = asset_bytes(layers[:1], table_name)
    if (full or own is not None) and (own is None or packed != own):
        raise ValueError("Missing or different source object table data/objects/object2.pak in campaign pack; re-import it.")
    for dep in layers[1:-1]:
        if asset_bytes([dep], table_name) is None:
            raise ValueError(f"Missing mc2x-compat object table: {dep / table_name}; re-import the full base.")
    raw = packed if packed is not None else asset_bytes(layers[1:], table_name)
    if raw is None:
        raise ValueError("Missing effective object table data/objects/object2.pak")
    if len(raw) < 8:
        raise ValueError("Invalid object table data/objects/object2.pak")
    first = struct.unpack_from("<I", raw, 4)[0]
    if first < 8 or first % 4 or first > len(raw):
        raise ValueError("Invalid object table data/objects/object2.pak")
    count = first // 4 - 2
    slots = struct.unpack_from(f"<{count}I", raw, 8)
    for campaign in campaigns:
        name = f"data/missions/{campaign['first_mission']}.fit"
        mission = asset_bytes(layers, name)
        if mission is None:
            raise ValueError(f"Missing first-mission asset: {name}")
        ids = {int(n) for n in re.findall(rb"\bObjectNumber\s*=\s*(-?\d+)", mission, re.I)}
        unresolved = []
        for number in sorted(ids):
            if not 0 <= number < count:
                unresolved.append(number)
                continue
            entry = slots[number]
            start = entry & 0x1fffffff
            end = slots[number + 1] & 0x1fffffff if number + 1 < count else len(raw)
            if entry >> 29 == 7 or not first <= start < end <= len(raw):
                unresolved.append(number)
        if unresolved:
            raise ValueError(f"{name}: unresolved object packets: {unresolved}")


def repair_shared_components(existing, root, scratch):
    """Validate shared ammo against its own source, including on unchanged imports."""
    report = existing / "mc2x_import_report.json"
    provenance = json.loads(report.read_text()) if report.is_file() else {}
    origin = Path(provenance.get("source_path", root))
    expected = provenance.get("source_sha256")
    legacy_identity = provenance.get("source_fingerprint")
    supplied_hash = digest(source_files(root))
    # Upgrade a legacy size-based identity using the original source when available.
    if provenance and not expected and origin.is_dir():
        if legacy_identity and legacy.manifest.source_fingerprint(str(origin)) == legacy_identity:
            expected = digest(source_files(origin))
    if provenance:
        def matches(path, fingerprint):
            return (fingerprint == expected if expected else bool(legacy_identity) and
                    legacy.manifest.source_fingerprint(str(path)) == legacy_identity)
        if matches(root, supplied_hash):
            origin = root
        elif not origin.is_dir() or not matches(origin, digest(source_files(origin))):
            raise ValueError("Shared component source is missing or changed; supply the original "
                             "full MC2X-CVE-G base ZIP, EXE or folder and re-import it "
                             f"(recorded source: {origin}).")
    else:
        origin = root
    destination = existing / "data/objects/compbas.csv"
    candidates = source_files(origin) if origin.is_dir() else {}
    raw = None
    archive = candidates.get("misc.fst")
    if archive:
        entries = sorted(fst_entries(archive), key=lambda e: e[3].lower().endswith("mc2xcomp.csv"))
        for off, comp, real, name, lz, data in entries:
            if name.replace("\\", "/").lower() in (
                    "data/objects/compbas.csv", "data/objects/mc2xcomp.csv"):
                raw = legacy.fst.decompress_entry(off, comp, real, lz, data)
    for name in ("data/objects/compbas.csv", "data/objects/mc2xcomp.csv"):
        if name in candidates:
            raw = candidates[name].read_bytes()
    if raw is None:
        if not destination.is_file():
            raise ValueError(f"Missing shared component table {destination}; re-import the full MC2X-CVE-G base first (source: {origin}).")
        return
    provenance.update(source_path=str(origin), source_sha256=digest(candidates))
    report.write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    if destination.is_file() and destination.read_bytes() == raw:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or destination.parent.is_symlink() or destination.parent.is_junction():
        raise ValueError(f"Linked shared component path: {destination}")
    temporary = scratch / "shared-compbas.csv"
    temporary.write_bytes(raw)
    temporary.replace(destination)
    print(f"Repaired shared component table: {destination} from {origin}", file=sys.stderr)


def find_root(folder):
    candidates = [folder] + sorted(p for p in folder.iterdir() if p.is_dir())
    roots = [p for p in candidates if (p / "data").is_dir() or (p / "Data").is_dir()
             or any(p.glob("*.fst"))]
    if not roots:
        roots = [p for p in candidates if (p / "campaign").is_dir() and (p / "missions").is_dir()]
    if len(roots) != 1:
        raise ValueError("Expected one campaign root containing data/ or *.fst; select one campaign folder.")
    return roots[0]


def import_campaign(source, deploy):
    source, deploy = Path(source).resolve(), Path(deploy).resolve()
    if not source.exists():
        raise ValueError(f"Download or folder not found: {source}")
    if not any((deploy / name).is_file() for name in ("mc2.exe", "mc2")):
        raise ValueError(f"Game folder has no mc2 executable: {deploy}; use --deploy with your game folder.")
    installed_source = source.parent == deploy / "mods"
    if source.is_dir() and (source == deploy or source in deploy.parents or
                            (deploy in source.parents and not installed_source)):
        raise ValueError("Source and game folder must be separate, with neither inside the other.")
    mods = deploy / "mods"
    if mods.is_symlink() or mods.is_junction():
        raise ValueError("Linked mods directory is unsupported.")
    mods.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".campaign-import-", dir=mods) as scratch:
        scratch = Path(scratch)
        (scratch / ".import-owner.json").write_text(json.dumps({"pid": os.getpid(), "started": time.time()}), encoding="utf-8")
        if source.is_file():
            unpacked = scratch / "source"
            unpacked.mkdir()
            if not zipfile.is_zipfile(source):
                extractor = shutil.which("innoextract") if source.suffix.lower() == ".exe" else None
                if not extractor and source.suffix.lower() == ".exe" and (deploy / "innoextract.exe").is_file():
                    extractor = str(deploy / "innoextract.exe")
                if not extractor:
                    raise ValueError("Expected a ZIP or campaign folder. For Inno Setup EXEs put innoextract on PATH (see IMPORT-CAMPAIGNS.md), or install into a separate folder and import that folder. Extract 7z/RAR downloads first.")
                # background-launch: allow innoextract console archive reader only; never runs the installer or engine
                run = subprocess.run([extractor, "--extract", "--output-dir", str(unpacked), str(source)],
                                     capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
                if run.returncode:
                    raise ValueError("innoextract could not extract this installer: " + run.stderr[-1200:])
            else:
                seen = set()
                with zipfile.ZipFile(source) as archive:
                    for entry in archive.infolist():
                        name = safe_path(entry.filename)
                        if name.lower() in seen or (entry.external_attr >> 16) & 0o170000 == 0o120000:
                            raise ValueError(f"Duplicate or linked ZIP entry: {name}")
                        seen.add(name.lower())
                        target = unpacked / name
                        if entry.is_dir():
                            target.mkdir(parents=True, exist_ok=True)
                        else:
                            target.parent.mkdir(parents=True, exist_ok=True)
                            with archive.open(entry) as src, target.open("wb") as dst:
                                shutil.copyfileobj(src, dst)
            root = find_root(unpacked)
        else:
            root = find_root(source)
        if (root / "campaign").is_dir() and (root / "missions").is_dir():
            normalized = scratch / "normalized"
            shutil.copytree(root, normalized / "data", symlinks=True)
            root = normalized
        files, names, campaigns = inspect_source(root)
        identity = json.dumps(campaigns, sort_keys=True)
        slug = re.sub(r"[^a-z0-9]+", "-", campaigns[0]["name"].lower()).strip("-")[:45] or "campaign"
        mod_id = slug + "-" + hashlib.sha256(identity.encode()).hexdigest()[:8]
        target = mods / mod_id
        fingerprint = digest(files)
        if target.exists():
            receipt = target / ".campaign-import.json"
            if target.is_symlink() or target.is_junction() or not receipt.is_file():
                raise ValueError(f"Refusing to replace unmanaged mod: {target}")
            saved = json.loads(receipt.read_text())
            if not isinstance(saved, dict) or not isinstance(saved.get("files"), dict):
                raise ValueError(f"Invalid import receipt: {receipt}")
            if saved.get("source_sha256") != fingerprint:
                raise ValueError(f"Different version already installed: {target}. Keep your saves and move that mod aside before importing.")
            installed = {p.relative_to(target).as_posix() for p in target.rglob("*")
                         if p.is_file() and not p.name.startswith(".")}
            if installed != set(saved["files"]):
                raise ValueError(f"Installed mod has changed or is incomplete: {target}")
            for rel, expected in saved["files"].items():
                p = target / safe_path(rel)
                if not p.is_file() or p.is_symlink() or hashlib.sha256(p.read_bytes()).hexdigest() != expected:
                    raise ValueError(f"Installed mod has changed or is incomplete: {p}")
            metadata = json.loads((target / "mod.json").read_text())
            dependencies = metadata.get("dependencies", [])
            if "mc2x-compat" in dependencies:
                validate_mc2x_tables(root, files, campaigns,
                                    [target] + [mods / safe_path(d) for d in dependencies] + [deploy])
            for dependency in dependencies:
                dep = mods / safe_path(dependency)
                if dep.is_symlink() or dep.is_junction() or not (dep / "data").is_dir():
                    raise ValueError(f"Missing {dependency}; import the full compatibility base first.")
                if dependency == "mc2x-compat":
                    repair_shared_components(dep, root, scratch)
            if "mco-compat" in metadata.get("dependencies", []) and saved.get("logistics_art_version") != LOGISTICS_ART_VERSION:
                import_logistics_art(root, target)
                saved["logistics_art_version"] = LOGISTICS_ART_VERSION
                saved["files"] = {p.relative_to(target).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in target.rglob("*") if p.is_file() and not p.name.startswith(".")}
                receipt.write_text(json.dumps(saved), encoding="utf-8")
                return {"status": "installed", "mod": mod_id, "campaigns": campaigns}
            return {"status": "unchanged", "mod": mod_id, "campaigns": campaigns}
        probe = legacy.classify(str(root))
        if probe["action"] == "reject":
            raise ValueError(probe["reject_reason"])
        compat = probe.get("needs_compat")
        staged = scratch / "install"
        (staged / "mods").mkdir(parents=True)
        refs = [str(deploy / "data")]
        pending = []
        if compat:
            existing = mods / compat
            if existing.is_symlink() or existing.is_junction():
                raise ValueError(f"Linked compatibility directory: {existing}")
            if (existing / "data").is_dir():
                if compat == "mc2x-compat":
                    repair_shared_components(existing, root, scratch)
                refs.insert(0, str(existing / "data"))
            else:
                plan = legacy._plan(str(root), probe, str(deploy))
                build = next((s for s in plan if compat in s["targets"]), None)
                if not build:
                    raise ValueError(f"Missing {compat}. Import the full Omnitech 628 or MC2X CVE-G install first.")
                build_args = legacy._subst(build["args"], str(staged), str(deploy))
                if compat == "mco-compat":
                    buildings = deploy / "data/art/buildings.csv"
                    if not buildings.is_file():
                        buildings = scratch / "stock-buildings.csv"
                        for archive in deploy.glob("*.fst"):
                            for off, comp, real, name, lz, data in fst_entries(archive):
                                if safe_path(name).lower() == "data/art/buildings.csv":
                                    buildings.write_bytes(legacy.fst.decompress_entry(off, comp, real, lz, data))
                                    break
                            if buildings.is_file():
                                break
                    if not buildings.is_file():
                        raise ValueError("Missing stock Buildings.csv in base data/ and FSTs; repair the game install first.")
                    build_args += ["--stock-buildings", str(buildings)]
                rc = legacy._run_step(build, build_args)
                if rc not in (0, None):
                    raise ValueError(f"Compatibility converter failed: {rc}")
                if compat == "mc2x-compat":
                    repair_shared_components(staged / "mods" / compat, root, scratch)
                pending.append(compat)
                refs.insert(0, str(staged / "mods" / compat / "data"))
        out = staged / "mods" / mod_id
        args = ["--source", str(root), "--refs", *refs, "--out", str(out), "--deploy", str(deploy)]
        if "mission.fst" not in files:
            args += ["--fsts"]
        rc = legacy._run_step({"kind": "transfer"}, args)
        if rc not in (0, None):
            raise ValueError(f"Campaign transfer failed: {rc}")
        if compat == "mco-compat":
            fill_campaign_object_slots(root, out, refs)
            import_logistics_art(root, out)
        if compat == "mc2x-compat":
            validate_mc2x_tables(root, files, campaigns,
                                [out] + [Path(ref).parent for ref in refs])
        available = available_assets(out) | available_assets(deploy)
        for ref in refs[:-1] if len(refs) > 1 else []:
            available |= available_assets(Path(ref).parent)
        for campaign in campaigns:
            required_files = [f"data/campaign/{campaign['fit']}.fit"] + [
                f"data/missions/{campaign['first_mission']}.{suffix}" for suffix in ("fit", "pak")]
            for required in required_files:
                if required not in available:
                    raise ValueError(f"Missing first-mission asset: {required} after importing {source.name}. "
                                     "Re-extract that campaign installer and import its full required base "
                                     "(MCOsetup628.exe for Omnitech; MC2X-CVE-G for Wolfman).")
        if not (out / "data" / "campaign").is_dir():
            raise ValueError("Converter produced no campaign; original download may be incomplete.")
        for fit in (out / "data/campaign").glob("*.fit"):
            raw = fit.read_bytes()
            if b"MechWarriror" in raw:
                fit.write_bytes(raw.replace(b"MechWarriror", b"MechWarrior"))
        metadata = {"id": mod_id, "name": " / ".join(c["name"] for c in campaigns),
                    "description": "Imported community campaigns: " + ", ".join(c["name"] for c in campaigns),
                    "author": "Original campaign authors (see the supplied campaign credits)",
                    "credits": "Original campaign authors retain all content credits; imported from a user-supplied download.",
                    "type": "campaign", "version": "1.0", "dependencies": [compat] if compat else []}
        (out / "mod.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
        hashes = {p.relative_to(out).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in out.rglob("*") if p.is_file()}
        (out / ".campaign-import.json").write_text(json.dumps({"source_sha256": fingerprint, "files": hashes,
            "logistics_art_version": LOGISTICS_ART_VERSION}), encoding="utf-8")
        for item in pending + [mod_id]:
            (staged / "mods" / item).rename(mods / item)
        return {"status": "installed", "mod": mod_id, "compat": compat, "campaigns": campaigns}

def auto_import(deploy):
    """Discover raw installs; persist progress and source stamps outside their folders."""
    deploy = Path(deploy).resolve()
    mods = deploy / "mods"
    if mods.is_symlink() or mods.is_junction():
        raise ValueError("Linked mods directory is unsupported.")
    mods.mkdir(exist_ok=True)
    status = mods / ".campaign-auto.json"
    def remove_readonly(func, path, exc):
        os.chmod(path, 0o700)
        func(path)
    for staged in mods.glob(".campaign-import-*"):
        if staged.is_dir() and not staged.is_symlink() and not staged.is_junction():
            marker = staged / ".import-owner.json"
            if marker.is_symlink() or not marker.is_file():
                continue
            try:
                owner = json.loads(marker.read_text(encoding="utf-8"))
                pid, started = owner["pid"], owner["started"]
                if type(pid) is not int or pid <= 0 or not isinstance(started, (int, float)):
                    continue
                if time.time() - started <= 24 * 3600 and pid_alive(pid):
                    continue
            except (OSError, ValueError, KeyError, TypeError):
                continue
            shutil.rmtree(staged, onexc=remove_readonly)
    try:
        previous = json.loads(status.read_text(encoding="utf-8")) if status.is_file() else {}
        if not isinstance(previous, dict):
            previous = {}
    except (ValueError, OSError):
        previous = {}
    state = {"running": True, "message": "Looking for community campaigns...", "sources": {}}

    def publish(message):
        state["message"] = message
        temporary = status.with_suffix(".tmp")
        temporary.write_text(json.dumps(state, indent=2), encoding="utf-8")
        temporary.replace(status)

    publish(state["message"])
    candidates = []
    for source in sorted(mods.iterdir()):
        if (not source.is_dir() or source.name.startswith(".") or
                (source / "mod.json").exists() or (source / ".campaign-import.json").exists()):
            continue
        roots = [source] + sorted(p for p in source.iterdir() if p.is_dir())
        archives = any(list(root.glob("*.fst")) for root in roots)
        campaigns = any(list((root / "data/campaign").glob("*.fit")) or
                        list((root / "Data/campaign").glob("*.fit")) or
                        list((root / "campaign").glob("*.fit")) for root in roots)
        known = re.search(r"mcosetup|omnitech|desert.?fox|carver|mw2.?merc|exodus", source.name, re.I)
        # A loose single-mission mod is already playable without conversion.
        if not archives and not campaigns and not (known and not (source / "data/missions").is_dir()):
            continue
        candidates.append((not archives, source))
    for _, source in sorted(candidates):
        publish(f"Checking {source.name}...")
        entry = {}
        output = io.StringIO()
        started = None
        try:
            root = find_root(source)
            inputs = source_files(root)
            stamp = hashlib.sha256(json.dumps([
                (name, p.stat().st_size, p.stat().st_mtime_ns)
                for name, p in sorted(inputs.items())]).encode()).hexdigest()
            entry = {"stamp": stamp}
            saved = previous.get("sources", {}).get(source.name, {})
            target = saved.get("mod", "")
            if saved.get("stamp") == stamp and not saved.get("error") and target and (mods / target / ".campaign-import.json").is_file():
                entry = saved
            else:
                publish(f"Importing {source.name} (source files are preserved)...")
                started = time.perf_counter()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                    entry.update(import_campaign(source, deploy))
                print(f"[CAMPAIGN_AUTO] {source.name}: {entry['status']}", flush=True)
        except (Exception, SystemExit) as exc:
            entry["error"] = str(exc) + ("\n" + output.getvalue()[-1600:] if output.tell() else "")
            print(f"[CAMPAIGN_AUTO] {source.name}: {exc}", file=sys.stderr, flush=True)
        if output.tell():
            print(output.getvalue(), flush=True)
        if started is not None:
            entry["import_seconds"] = round(time.perf_counter() - started, 3)
        state["sources"][source.name] = entry
        publish(f"Checked {source.name}")
    state["running"] = False
    publish("Community campaign scan complete.")
    return state



def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", help="Downloaded ZIP or separate campaign install folder")
    parser.add_argument("--auto", action="store_true", help="Import raw campaigns found in the game's mods folder")
    parser.add_argument("--deploy", default=str(Path(__file__).resolve().parents[1]), help="Folder containing mc2.exe")
    args = parser.parse_args(argv)
    try:
        if args.auto:
            auto_import(args.deploy)
            return 0
        if not args.source:
            parser.error("source is required unless --auto is used")
        result = import_campaign(args.source, args.deploy)
        print(json.dumps(result, indent=2))
        print("Open Campaigns & mods, press Refresh, select the campaign and compatibility base, then Start campaign.")
        return 0
    except (ValueError, OSError, zipfile.BadZipFile, RuntimeError, SystemExit) as exc:
        print(f"Import failed: {exc}", file=sys.stderr)
        return 1


LOGISTICS_ART_VERSION = 2


def import_logistics_art(root, out):
    """Keep Omnitech's authored content independent of the stock UI's grid sizes."""
    import csv
    from PIL import Image
    sys.path.insert(0, str(Path(__file__).resolve().parent / "mco_import"))
    from build_mco_compat import build_mech_names_fit, _MECH_ICON_MAP
    objects = out / "data/objects"
    build_mech_names_fit(str(objects), str(out / "data/defs/text/en_us"))
    art = root / "data/art"
    icons = {p.stem.lower(): p for p in (art / "mechicons").glob("*.[tT][gG][aA]")}
    for path in objects.glob("*.csv"):
        rows = list(csv.reader(io.StringIO(path.read_text(encoding="latin1"))))
        name = next((r[1].strip().lower() for r in rows if len(r) > 1 and r[0] == "MechName"), "")
        source = icons.get(_MECH_ICON_MAP.get(path.stem.lower(), name))
        if source:
            dest = out / f"data/art/mechicons/{path.stem.lower()}.png"
            dest.parent.mkdir(parents=True, exist_ok=True)
            Image.open(source).convert("RGBA").save(dest)
    layout = art / "mcl_gn_deploymentteams.fit"
    if not layout.is_file():
        return
    block = re.search(r"(?is)\[PilotIcon\](.*?)(?=\[|\Z)", layout.read_text(encoding="latin1"))
    if not block:
        return
    def value(key):
        return re.search(r"(?im)^\s*\w+\s+" + key + r'\s*=\s*("[^"]+"|\w+)', block[1])[1].strip('"')
    source = art / (value("FileName").lower().removesuffix(".tga") + ".tga")
    if source.is_file():
        image = Image.open(source).convert("RGBA")
        w, h = int(value("Width")), int(value("Height"))
        dest = out / "data/art/pilots"
        dest.mkdir(parents=True, exist_ok=True)
        # Pilot PictureIndex uses twelve columns, including padded source sheets.
        for index in range(12 * (image.height // h)):
            x, y = index % 12 * w, index // 12 * h
            image.crop((x, y, x + w, y + h)).save(dest / f"{index:02}.png")
    print(f"[CAMPAIGN_ART] token={LOGISTICS_ART_VERSION} cache=miss")


if __name__ == "__main__":
    sys.exit(main())
