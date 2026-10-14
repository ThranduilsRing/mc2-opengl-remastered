"""Compare the editor-owned resolver against the independent archive reader without launching an engine."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from console_child import child_kwargs
from tools.mc2x_import.fst import fst_entries, decompress_entry
from run_editor_smoke import fit_fields


def oracle(base, mods, active):
    layers = []
    if active:
        manifest = mods / active / "mod.json"
        dependencies = json.loads(manifest.read_text()).get("dependencies", []) if manifest.exists() else []
        layers = [(mods / active, f"mod:{active}")] + [(mods / dep, f"mod:{dep}") for dep in dependencies]
    layers.append((base, "base-loose"))
    loose = {}
    for root, layer in layers:
        if not (root / "data").exists():
            continue
        for path in sorted((root / "data").rglob("*")):
            relative = path.relative_to(root)
            if path.is_file() and not any(part.startswith('.') for part in relative.parts):
                loose.setdefault(relative.as_posix().lower(), (path, layer))
    archives = {}
    system = fit_fields((base / "system.cfg").read_bytes())
    count = int(system["FastFiles/NumFastFiles"][0].split(" ", 1)[1])
    for i in range(count):
        name = system[f"FastFiles/File{i}"][0].split(" ", 1)[1].strip('"')
        archive = base / name
        if archive.exists():
            for entry in fst_entries(archive):
                archives.setdefault(entry[3].lower().replace(chr(92), '/'), (archive, entry))
    def read(key):
        if key in loose:
            path, layer = loose[key]
            return path.read_bytes(), layer, path
        archive, entry = archives[key]
        data = decompress_entry(entry[0], entry[1], entry[2], entry[4], entry[5])
        if entry[1] == entry[2] and len(data) > 2 and data[0] == 0x78 and (data[0] * 256 + data[1]) % 31 == 0:
            try:
                inflated = zlib.decompress(data)
                if len(inflated) == entry[2]:
                    data = inflated
            except zlib.error:
                pass
        if len(data) != entry[2]:
            raise ValueError(f"oracle decompression size mismatch: {key}")
        return data, "fastfile", archive
    return read


def expected_files(independent):
    """Enumeration and mounted inputs define coverage independently of the probe."""
    expected = {key: independent(key) for key in ("data/art/buildings.csv", "data/objects/object2.pak")}
    rows = expected["data/art/buildings.csv"][0].decode("latin-1").splitlines()
    for line in rows[1:]:
        try:
            row = next(csv.reader([line], strict=True))
        except (csv.Error, StopIteration):
            continue
        if len(row) != 12:
            continue
        stem = row[0].strip()
        if not stem or stem in (".", "..") or any(c in stem for c in ('/', chr(92), ':', chr(0))):
            continue
        key = f"data/objects/{stem.lower()}.csv"
        try:
            expected[key] = independent(key)
        except KeyError:
            pass
    return expected


def compare_resolution(output, report, expected):
    if set(report["files"]) != set(expected):
        missing = sorted(set(expected) - set(report["files"]))
        extra = sorted(set(report["files"]) - set(expected))
        raise ValueError(f"resolver coverage disagreement: missing={missing}; unexpected={extra}")
    manifest = {}
    for key, (contents, layer, path) in expected.items():
        source = report["files"][key]
        if (output / key).read_bytes() != contents or source["layer"] != layer or Path(source["source"]).resolve() != path.resolve():
            raise ValueError(f"resolver/oracle disagreement: {key}")
        manifest[key] = {"sha256": hashlib.sha256(contents).hexdigest(), "layer": layer, "source": str(path)}
    return manifest


def run(exe, base, mods, active, output):
    output.mkdir(parents=True, exist_ok=False)
    command = [str(exe), str(base), str(mods), active or "-", str(output)]
    child = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60, **child_kwargs(redirected=True))
    (output / "probe.log").write_text(child.stdout + child.stderr, encoding="utf-8")
    if child.returncode:
        raise ValueError(f"catalog probe failed ({child.returncode}): {child.stdout}{child.stderr}")
    report = json.loads((output / "catalog.json").read_text())
    independent = oracle(base, mods, active)
    expected = expected_files(independent)
    manifest = compare_resolution(output, report, expected)
    logs = [line for line in child.stdout.splitlines() if line.startswith("[EDITOR_CATALOG_REJECT v1]")]
    if logs != report["rejections"] or len(logs) != len(set(logs)):
        raise ValueError("rejection evidence is missing or duplicated")
    if not any("row=78 reason=unterminated_literal" in line for line in logs):
        raise ValueError("known malformed stock MilCheckPoint entry was accepted")
    counts = {}
    for entry in report["entries"]:
        counts[entry["family"]] = counts.get(entry["family"], 0) + 1
    if counts != report["counts"] or not all(counts.get(f, 0) for f in ("mech", "vehicle", "building", "turret", "tree", "gate", "bridge", "light", "marker")):
        raise ValueError("catalog family census disagrees or misses a supported family")
    anubis = next(entry for entry in report["entries"] if entry["id"] == 53)
    if anubis["name"] != "Anubis":
        raise ValueError("resource-name resolution failed")
    (output / "resolution-oracle.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{active or 'stock'}: families={counts}; rejections={len(logs)}; resolver/oracle={len(manifest)}/{len(expected)} identical", flush=True)
    return report


def prove(exe, base, mods, active, output, include_mod=True):
    stock = run(exe, base, mods, "", output / "stock")
    archive_base = output / "archive-base"
    archive_base.mkdir()
    (archive_base / "system.cfg").write_bytes((base / "system.cfg").read_bytes())
    system = fit_fields((base / "system.cfg").read_bytes())
    count = int(system["FastFiles/NumFastFiles"][0].split(" ", 1)[1])
    for i in range(count):
        name = system[f"FastFiles/File{i}"][0].split(" ", 1)[1].strip('"')
        if (base / name).exists():
            os.link(base / name, archive_base / name)
    independent = oracle(base, mods, "")
    for key in ("data/objects/object2.pak", "data/defs/text/en_us/mc2res_legacy.fit"):
        target = archive_base / key
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(independent(key)[0])
    archived = run(exe, archive_base, mods, "", output / "archived-stock")
    if archived["files"]["data/art/buildings.csv"]["layer"] != "fastfile" or archived["counts"] != stock["counts"]:
        raise ValueError("stock archive fallback did not preserve the family census")
    if not any(v["layer"] == "fastfile" and k.startswith("data/objects/") for k, v in archived["files"].items()):
        raise ValueError("stock variant archive fallback was not exercised")
    if include_mod:
        mod = run(exe, base, mods, active, output / "mod")
        stock_ids = {e["id"]: e for e in stock["entries"]}
        mod_ids = {e["id"]: e for e in mod["entries"]}
        if active == "MC2X-CVE-G":
            if mod_ids[2]["appearance"] != "Starslayer" or mod_ids[2]["family"] != "mech" or mod_ids[2]["definitionLayer"] != "mod:MC2X-CVE-G":
                raise ValueError("known MC2X-CVE-G FitID 2 override missing")
            if 252 in mod_ids or stock_ids[252]["family"] != "tree":
                raise ValueError("MC2X-CVE-G FitID 252 rejection did not replace stock definition")
        stock_pak = (output / "stock/data/objects/object2.pak").read_bytes()
        mod_pak = (output / "mod/data/objects/object2.pak").read_bytes()
        if stock_pak == mod_pak:
            raise ValueError("selected mod did not change catalog definition bytes")
        from editor.tests.legacy_reference import packet
        known = {}
        for object_id in (2, 252):
            before, after = packet(stock_pak, object_id), packet(mod_pak, object_id)
            known[object_id] = {"stockSha256": hashlib.sha256(before).hexdigest(), "modSha256": hashlib.sha256(after).hexdigest(),
                                "changed": before != after, "provenance": f"mod:{active}"}
        if active == "MC2X-CVE-G" and not known[252]["changed"]:
            raise ValueError("MC2X-CVE-G known FitID 252 override did not change decoded definition")
        (output / "known-overrides.json").write_text(json.dumps(known, indent=2) + "\n")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=ROOT / "build64/out/editor/RelWithDebInfo/editor_catalog_probe.exe")
    parser.add_argument("--base", type=Path, default=Path("A:/Games/mc2-opengl/mc2-win64-lane3"))
    parser.add_argument("--mods", type=Path, default=Path("A:/Games/mc2-opengl"))
    parser.add_argument("--mod", default="MC2X-CVE-G")
    parser.add_argument("--stock-only", action="store_true")
    parser.add_argument("--out", type=Path, default=ROOT / "scratch/e5-catalog-proofs" / time.strftime("%Y-%m-%dT%H-%M-%S"))
    args = parser.parse_args()
    try:
        return prove(args.exe, args.base, args.mods, args.mod, args.out, not args.stock_only)
    except (OSError, ValueError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
        print(f"Catalog proof failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
