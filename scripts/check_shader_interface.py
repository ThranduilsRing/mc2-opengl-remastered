#!/usr/bin/env python3
"""check_shader_interface.py -- one shader-interface pass over the baked SPIR-V manifest and the CPU/GLSL mirrors.

One load (pilots.json, every sidecar, spirv_index.json, spirv_package.json, binding-slot-occupancy.json,
tools/shader_schema/manifest.json, scripts/shader_interface_rules.json) feeds these families:

  artifacts   every pilot (variant, stage) has a .spv + sidecar; sidecar source_sha256 == current flattened source;
              buffer reflection carries types; Slang reflection / GPU ABI member layouts match their headers;
              every reflected GLSL descriptor is in binding-slot-occupancy.json; spirv_index.json maps
              base|stage|sorted-defines to the sidecar's artifact.            (was check-spirv-artifacts.py)
  reflection  across a program's variants: fixed-layout elements (vertex attrs, frag outputs, UBO/SSBO/sampler/image
              bindings) never move; interface is monotonic in the define set; descriptors agree with the occupancy
              census; inter-stage varyings link (vert out loc == frag in loc). (was check-spirv-reflection-contract.py)
  package     spirv_package.json: schema version, pilot families, variant matrix, declared .spv + sidecars with
              reflection, content hashes, no undeclared .spv, tool/policy fields.  (was check-shader-package.py)
  schema      tools/shader_schema/manifest.json interfaces: reflected SSBO array stride == expectedSize, required
              goldens present, GLSL std430 size/offsets == expectedSize and C++ offsetof/sizeof asserts.
              A missing GLSL source or struct now FAILS (the old validate.py printed FAIL and exited 0).
                                                                     (was check-shader-schema.sh + validate.py)
  rules       table-driven source lockstep in scripts/shader_interface_rules.json (vegetation GPU cull: no CPU
              per-instance path, CullParams == VegParamsData member list and size, shared include, baked stages).
                                                                     (was check-veg-gpu-cull.py)

Tool-free: never runs glslang/spirv-cross. Exit 0 = clean, 1 = any FAIL.
Usage: py -3 scripts/check_shader_interface.py [--root R] [--families a,b] [--quiet]
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/shader_offline_build"))
import build_variants as bv  # noqa: E402
import slang_compile  # noqa: E402

PILOTS = "tools/shader_offline_build/pilots.json"
BINDING_OCC = "docs/render-backend-seams/binding-slot-occupancy.json"
SCHEMA_MANIFEST = "tools/shader_schema/manifest.json"
REFLECT_EXPECTED = "tools/shader_reflect/expected"
RULES = "scripts/shader_interface_rules.json"
FAMILIES = ("artifacts", "reflection", "package", "schema", "rules")
BUFFER_NS = {"ubos": "UBO", "ssbos": "SSBO", "samplers": "SAMPLER", "images": "IMAGE"}


def _load(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


class Manifest:
    """The single load pass every family reads from."""

    def __init__(self, root: Path):
        self.root = root
        self.cfg = _load(root / PILOTS)
        self.spv_dir = root / self.cfg.get("spv_dir", "shaders/spv")
        self.sources = {(p["program"], st): rel for p in self.cfg["pilots"] for st, rel in p["stages"].items()}
        self.sidecars = {}  # (base, stage, vname) -> meta, or None when missing
        for p in self.cfg["pilots"]:
            for v in p["variants"]:
                for st in p["stages"]:
                    sc = self.spv_dir / f"{p['program']}.{st}.{v['name']}.json"
                    self.sidecars[p["program"], st, v["name"]] = _load(sc) if sc.exists() else None
        self.idx_path = self.spv_dir / "spirv_index.json"
        self.index_records = _load(self.idx_path).get("records", []) if self.idx_path.exists() else None
        self.index = {r["key"]: r["artifact"] for r in self.index_records or []}
        self.pkg_path = self.spv_dir / "spirv_package.json"
        self.pkg = _load(self.pkg_path) if self.pkg_path.exists() else None
        self.occ = bv.occupancy_slots(root / BINDING_OCC)


def _abi_errors(m: Manifest, meta, rel):
    raw = dict(meta.get("bindings", {}), types=meta.get("types", {}))
    out = []
    if (raw.get("ubos") or raw.get("ssbos")) and not raw["types"]:
        out.append("gpu_abi: buffer reflection is missing types")
    if rel.endswith(".slang"):
        try:
            out += slang_compile.validate_reflection(raw, m.root / rel, m.root)
        except (OSError, ValueError) as e:
            out.append(str(e))
    else:
        out += slang_compile.validate_abi(raw, m.root)
    return out


def check_artifacts(m: Manifest):
    fails = []
    for p in m.cfg["pilots"]:
        base = p["program"]
        for v in p["variants"]:
            vname, defines = v["name"], v.get("defines", [])
            for stage, rel in p["stages"].items():
                tag = f"{base}.{stage}.{vname}"
                meta = m.sidecars[base, stage, vname]
                if meta is None:
                    fails.append(f"{tag}: sidecar missing ({m.spv_dir.relative_to(m.root).as_posix()}/{tag}.json)"
                                 " - run build_variants.py")
                    continue
                if not meta.get("artifact") or not (m.spv_dir / meta["artifact"]).exists():
                    fails.append(f"{tag}: .spv artifact missing ({meta.get('artifact')})")
                try:
                    cur = slang_compile.source_bundle(m.root / rel)
                    if meta.get("source_sha256") != hashlib.sha256(cur.encode("utf-8")).hexdigest():
                        fails.append(f"{tag}: source drift - shader changed since bake (rebuild with build_variants.py)")
                except Exception as e:  # noqa: BLE001 - any unreadable source is a FAIL, as before
                    fails.append(f"{tag}: cannot rebuild source: {e}")
                fails += [f"{tag}: {e}" for e in _abi_errors(m, meta, rel)]
                for ns, blk in ([] if rel.endswith(".slang") else bv.descriptors(meta.get("bindings", {}))):
                    b = blk.get("binding")
                    if b is None or m.occ is None or (ns, blk.get("set", 0), b, blk.get("name"), rel) not in m.occ:
                        fails.append(f"{tag}: {ns} '{blk.get('name')}' binding={b} absent from binding-slot-occupancy.json")
                ikey = f"{base}|{stage}|" + ";".join(sorted(defines))
                if m.index_records is None:
                    fails.append(f"{tag}: spirv_index.json missing - run build_variants.py")
                elif m.index.get(ikey) != meta.get("artifact"):
                    fails.append(f"{tag}: index key '{ikey}' -> {m.index.get(ikey)} != sidecar artifact {meta.get('artifact')}")
    return fails


def _elems(meta, kind):
    b, i = meta.get("bindings", {}), meta.get("interface", {})
    if kind in ("outputs", "inputs"):
        return {x["name"]: x.get("location") for x in i.get(kind, [])}
    return {x["name"]: (x.get("set", 0), x.get("binding")) for x in b.get(kind, [])}


def _fixed_layout(stage, kind):
    return kind in BUFFER_NS or (stage, kind) in (("vert", "inputs"), ("frag", "outputs"))


def check_reflection(m: Manifest):
    fails, groups, by_variant = [], {}, {}
    for (base, stage, vname), meta in m.sidecars.items():
        if meta is None:
            continue  # the artifacts family reports missing sidecars
        defs = frozenset(meta.get("defines", []))
        groups.setdefault((base, stage), []).append((defs, meta))
        by_variant.setdefault((base, vname), {"defs": defs})[stage] = meta
    for (base, stage), variants in sorted(groups.items()):
        rel = m.sources[base, stage]
        for _, meta in variants:
            fails += [f"{base}.{stage}: {e}" for e in _abi_errors(m, meta, rel)]
        for kind in ("outputs", "inputs", "ubos", "ssbos", "samplers", "images"):
            present = {}
            for defs, meta in variants:
                for name, val in _elems(meta, kind).items():
                    present.setdefault(name, {})[defs] = val
            for name, by_def in present.items():
                vals = set(by_def.values())
                if _fixed_layout(stage, kind) and len(vals) > 1:
                    fails.append(f"{base}.{stage} {kind} '{name}': inconsistent location/binding across variants "
                                 f"{sorted(map(str, vals))} (fixed-layout element must not move between variants)")
                for d_present in by_def:
                    for d_all, _ in variants:
                        if d_all >= d_present and d_all not in by_def:
                            fails.append(f"{base}.{stage} {kind} '{name}': present for defines {sorted(d_present)} but "
                                         f"MISSING in superset variant {sorted(d_all)} (non-monotonic gating / define removed it)")
                if kind in BUFFER_NS and not rel.endswith(".slang"):
                    b = next(iter(vals)) if len(vals) == 1 else None
                    if b is not None and (m.occ is None or (BUFFER_NS[kind], *b, name, rel) not in m.occ):
                        fails.append(f"{base}.{stage} {BUFFER_NS[kind]} '{name}' binding={b} absent from binding-slot-occupancy.json")
    for (base, vname), st in sorted(by_variant.items()):
        if "vert" in st and "frag" in st:
            vo, fi = _elems(st["vert"], "outputs"), _elems(st["frag"], "inputs")
            for name in sorted(set(vo) & set(fi)):
                if vo[name] != fi[name]:
                    fails.append(f"{base}.{vname} varying '{name}': vert out loc {vo[name]} != frag in loc {fi[name]} (would mis-link)")
    return fails


def check_package(m: Manifest):
    if m.pkg is None:
        return ["spirv_package.json missing - run build_variants.py"]
    pkg, fails = m.pkg, []
    if pkg.get("schema_version") != bv.PACKAGE_SCHEMA_VERSION:
        fails.append(f"schema_version {pkg.get('schema_version')} != generator {bv.PACKAGE_SCHEMA_VERSION}")
    fams = [p["program"] for p in m.cfg["pilots"]]
    if pkg.get("pilot_families") != fams:
        fails.append(f"pilot_families {pkg.get('pilot_families')} != pilots.json {fams}")
    for path in (("tools", "glslangValidator"), ("tools", "spirv_cross"), ("runtime", "required_extension"),
                 ("runtime", "fallback_policy"), ("runtime", "fatal_gate")):
        node = pkg
        for k in path:
            node = node.get(k, {}) if isinstance(node, dict) else {}
        if not node:
            fails.append(f"package missing/empty field {'.'.join(path)}")
    if not pkg.get("excluded_families"):
        fails.append("package missing excluded_families")
    matrix = bv.package_variant_matrix(m.cfg)
    if pkg.get("variant_matrix") != matrix:
        fails.append("variant_matrix does not match pilots.json (regenerate package)")
    declared = set()
    for fam in matrix:
        for v in fam["variants"]:
            for stage, art in v["stages"].items():
                declared.add(art)
                tag = f"{fam['base']}.{stage}.{v['name']}"
                if not (m.spv_dir / art).exists():
                    fails.append(f"{tag}: declared .spv missing ({art})")
                meta = m.sidecars.get((fam["base"], stage, v["name"]))
                if meta is None and (m.spv_dir / f"{tag}.json").exists():
                    meta = _load(m.spv_dir / f"{tag}.json")
                if meta is None:
                    fails.append(f"{tag}: sidecar missing")
                elif "bindings" not in meta or "interface" not in meta:
                    fails.append(f"{tag}.json: reflection not captured (no bindings/interface)")
    if m.index_records is None:
        fails.append("spirv_index.json missing")
    else:
        for k, v in bv.package_hashes(m.spv_dir, m.index_records).items():
            if pkg.get("hashes", {}).get(k) != v:
                fails.append(f"stale hash {k}: package {pkg.get('hashes', {}).get(k)} != recomputed {v}")
    for p in sorted(m.spv_dir.glob("*.spv")):
        if p.name not in declared:
            fails.append(f"undeclared artifact in shaders/spv not in package: {p.name}")
    return fails


# ---- schema: std430 struct ABI vs reflected golden vs C++ asserts -------------------------------------------------
_STD430 = {
    "bool": (4, 4), "int": (4, 4), "uint": (4, 4), "float": (4, 4), "double": (8, 8),
    "vec2": (8, 8), "ivec2": (8, 8), "uvec2": (8, 8),
    "vec3": (12, 16), "ivec3": (12, 16), "uvec3": (12, 16),
    "vec4": (16, 16), "ivec4": (16, 16), "uvec4": (16, 16),
    "mat2": (32, 8), "mat3": (48, 16), "mat4": (64, 16),
}


def _align(off, a):
    return (off + a - 1) & ~(a - 1)


def _golden(m: Manifest, shader, variant):
    return m.root / REFLECT_EXPECTED / (shader.replace("/", "__") + f"__{variant}.json")


def _glsl_struct(text, typ):
    mm = re.search(r"\bstruct\s+" + re.escape(typ) + r"\s*\{([^}]*)\}", text)
    if not mm:
        return None
    fields = []
    for line in mm.group(1).splitlines():
        s = re.sub(r"//.*$", "", line).strip().rstrip(";").strip()
        parts = s.split()
        if len(parts) >= 2:
            fields.append((parts[1].split("[")[0], parts[0]))
    return fields


def _schema_interface(m: Manifest, iface):
    size, fails = iface["expectedSize"], []
    gp = _golden(m, iface["goldenShader"], iface.get("goldenVariant", "default"))
    if not gp.exists():
        fails.append(f"golden not found: {gp.relative_to(m.root).as_posix()}")
    else:
        golden = _load(gp)
        strides = [mb.get("array_stride") for s in golden.get("ssbos", []) for mb in s.get("members", [])]
        if size not in strides:
            fails.append(f"no SSBO with array_stride={size} in {gp.name}"
                         + (f" (found strides: {strides})" if strides else " (no SSBOs)"))
    for req in iface.get("requiredShaders", []):
        if not _golden(m, req, "default").exists():
            fails.append(f"required shader golden missing: {_golden(m, req, 'default').name}")
    glsl = m.root / iface["glslSource"]
    if not glsl.exists():
        return fails + [f"GLSL source not found: {iface['glslSource']}"]
    fields = _glsl_struct(glsl.read_text(encoding="utf-8"), iface["glslType"])
    if fields is None:
        return fails + [f"struct {iface['glslType']} not found in {iface['glslSource']}"]
    cpp = m.root / iface["cppHeader"]
    offsets = {}
    if cpp.exists():
        text = cpp.read_text(encoding="utf-8")
        tp = r"(?:\w+::)?" + re.escape(iface["cppType"])
        sm = re.search(r"static_assert\s*\(\s*sizeof\s*\(\s*" + tp + r"\s*\)\s*==\s*(\d+)", text)
        if sm and int(sm.group(1)) != size:
            fails.append(f"sizeof mismatch: manifest.expectedSize={size} cpp_assert={int(sm.group(1))}")
        offsets = {o.group(1): int(o.group(2)) for o in re.finditer(
            r"static_assert\s*\(\s*offsetof\s*\(\s*" + tp + r"\s*,\s*(\w+)\s*\)\s*==\s*(\d+)", text)}
    else:
        fails.append(f"C++ header not found: {iface['cppHeader']}")
    unknown = [ft for _, ft in fields if ft not in _STD430]
    if unknown:
        return fails + [f"unknown GLSL type(s) in struct: {unknown}"]
    layout, off = {}, 0
    for name, ft in fields:
        fsize, falign = _STD430[ft]
        off = _align(off, falign)
        layout[name] = off
        off += fsize
    total = _align(off, max((_STD430[ft][1] for _, ft in fields), default=4))
    if total != size:
        fails.append(f"GLSL std430 computed size={total} != expectedSize={size}")
    for field, cpp_off in offsets.items():
        if field not in layout:
            fails.append(f"field={field} in cpp offsetof but absent from GLSL struct")
        elif layout[field] != cpp_off:
            fails.append(f"field={field} cppOffset={cpp_off} glslOffset={layout[field]}")
    return fails


def check_schema(m: Manifest):
    mp = m.root / SCHEMA_MANIFEST
    if not mp.exists():
        return [f"manifest not found: {SCHEMA_MANIFEST}"]
    interfaces = _load(mp).get("interfaces", [])
    if not interfaces:
        return ["no interfaces in manifest"]
    return [f"interface={i['name']} {f}" for i in interfaces for f in _schema_interface(m, i)]


# ---- rules: declarative source lockstep (scripts/shader_interface_rules.json) -------------------------------------
def check_rules(m: Manifest):
    rp = m.root / RULES
    if not rp.exists():
        return [f"rules table missing: {RULES}"]
    fails, cache = [], {}

    def text(rel):
        if rel not in cache:
            p = m.root / rel
            cache[rel] = p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None
        return cache[rel]

    for r in _load(rp)["rules"]:
        for rel in r.get("files", []):
            t = text(rel)
            if t is None:
                fails.append(f"{r['id']}: {rel} missing")
                continue
            if r["kind"] == "absent":
                fails += [f"{r['id']}: {rel}: {r['msg']} ({n!r})" for n in r["needles"] if n in t]
            elif r["kind"] == "present":
                fails += [f"{r['id']}: {rel}: {r['msg']} ({n!r})" for n in r["needles"] if n not in t]
            elif r["kind"] == "members":
                mm = re.search(r["block"], t, re.S)
                got = re.findall(r["member"], mm.group(1)) if mm else None
                if got != r["expect"]:
                    fails.append(f"{r['id']}: {rel}: {r['msg']}: {got} != {r['expect']}")
            else:
                fails.append(f"{r['id']}: unknown rule kind {r['kind']!r}")
        if r["kind"] == "glob":
            d = m.root / r["dir"]
            names = [p.name for p in d.iterdir()] if d.is_dir() else []
            for pat in r["patterns"]:
                if not any(fnmatch.fnmatchcase(n, pat) for n in names):
                    fails.append(f"{r['id']}: {r['dir']}/{pat} {r['msg']}")
    return fails


CHECKS = {"artifacts": check_artifacts, "reflection": check_reflection, "package": check_package,
          "schema": check_schema, "rules": check_rules}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=None)
    ap.add_argument("--families", default=",".join(FAMILIES), help="comma list of " + ",".join(FAMILIES))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve() if args.root else ROOT
    fams = [f.strip() for f in args.families.split(",") if f.strip()]
    bad = [f for f in fams if f not in CHECKS]
    if bad:
        ap.error(f"unknown families {bad}")
    m = Manifest(root)
    total = 0
    for fam in fams:
        fails = CHECKS[fam](m)
        total += len(fails)
        for f in fails:
            print(f"[check_shader_interface] {fam}: FAIL {f}")
        if not args.quiet:
            print(f"[check_shader_interface] {fam}: {'FAIL' if fails else 'PASS'} ({len(fails)} fail)")
    print(f"[check_shader_interface] {'FAIL' if total else 'PASS'} families={len(fams)} fails={total}")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
