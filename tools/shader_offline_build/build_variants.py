#!/usr/bin/env python3
"""build_variants.py — OFFLINE-SHADER-VARIANT-BUILD-1

Compile deployable OpenGL SPIR-V artifacts for the pilot shader families, WITHOUT
changing any runtime code. This is the offline-bake half of the SPIR-V seam
identified in spirv-consumer-pilot-recon-1.md; the runtime consumer
(compile_shader branch + glSpecializeShader) is a SEPARATE later slice.

Per (pilot, variant, stage):
  - build the exact source the engine compiles (shader_common.build_shader_source
    = #version prefix + flattened #includes + inventory define-set);
  - compile to Vulkan SPIR-V with `glslangValidator -V --target-env vulkan1.3`
    and DELIBERATELY NOT `--auto-map-bindings` (so explicit layout(binding=)
    decorations are preserved for runtime correctness — reflect.py's auto-mapped
    .spv is reflection-only and must NOT be used at runtime);
  - reflect with `spirv-cross --reflect`;
  - emit a stable artifact name derived from shaderVariantId and a sidecar JSON
    (base, stage, defines, specializationParams, reflected ubo/ssbo/sampler
    bindings, source + spirv hash);
  - compare reflected UBO/SSBO bindings against binding-slot-occupancy.json.

Outputs go to shaders/spv/ (auto-deployed by deploy_payload.py's recursive walk).

Usage:
  py -3 tools/shader_offline_build/build_variants.py [--root R] [--quiet]
  py -3 tools/shader_offline_build/build_variants.py --check   # verify, write nothing
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import shader_common  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
import slang_compile  # noqa: E402

PILOTS = ROOT / "tools" / "shader_offline_build" / "pilots.json"
BINDING_OCC = ROOT / "docs" / "render-backend-seams" / "binding-slot-occupancy.json"


import re as _re
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

def sha256(text_or_bytes) -> str:
    h = hashlib.sha256()
    h.update(text_or_bytes if isinstance(text_or_bytes, bytes)
             else text_or_bytes.encode("utf-8"))
    return h.hexdigest()


def canon_source(s: str) -> str:
    """Path-independent, newline-normalized form for a STABLE source hash.

    build_shader_source emits `#line N // <absolute path>` markers whose path
    varies per worktree; the .spv glslang produces is unaffected, but a raw
    hash would drift across checkouts. Strip the path label (keep the line
    number) and normalize newlines so source_sha256 is portable."""
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    s = _re.sub(r"(?m)^(#line\s+\d+)\s*//.*$", r"\1", s)
    return s


def variant_id(base: str, defines: list[str]) -> str:
    """Stable shaderVariantId = hash of base + sorted normalized define-set."""
    key = base + "|" + ";".join(sorted(defines))
    return sha256(key)[:12]


def artifact_name(base, stage, variant, vid):
    return f"{base}.{stage}.{variant}.{vid[:8]}.spv"


def sidecar_name(base, stage, variant):
    return f"{base}.{stage}.{variant}.json"


def occupancy_slots(path=BINDING_OCC):
    """Return native buffer descriptors from the Vulkan binding census."""
    if not path.exists():
        return None
    occ = json.load(open(path, encoding="utf-8")).get("occupancy", {})
    out = set()
    for key, sites in occ.items():
        match = _re.fullmatch(r"set(\d+):binding(\d+)", key)
        if match:
            descriptor_set, binding = map(int, match.groups())
            for site in sites:
                ns = {"uniform": "UBO", "buffer": "SSBO"}.get(site["type"])
                if ns:
                    out.add((ns, descriptor_set, binding, site["role"], site["file"]))
    return out


def reflect(spv: Path, spirv_cross: str, slang_source=None, root=ROOT):
    raw = json.loads(subprocess.run(
        [spirv_cross, str(spv), "--reflect"],
        capture_output=True, text=True, check=True,
        **console_child.child_kwargs(redirected=True)).stdout)
    if slang_source is not None:
        errors = slang_compile.validate_reflection(raw, slang_source, root)
        if errors:
            raise ValueError('; '.join(errors))
    def blocks(key):
        return [{"name": b.get("name"), "binding": b.get("binding"),
                 "set": b.get("set")} for b in raw.get(key, [])]
    samplers = [{"name": t.get("name"), "type": t.get("type"),
                 "location": t.get("location"), "binding": t.get("binding")}
                for t in raw.get("textures", [])]
    # Slang/HLSL-style separate image + sampler objects (absent from GLSL output,
    # so existing sidecars are unchanged).
    for key in ("separate_images", "separate_samplers"):
        samplers += [{"name": t.get("name"), "type": t.get("type"),
                      "location": t.get("location"), "binding": t.get("binding"),
                      "set": t.get("set")} for t in raw.get(key, [])]
    return {"ubos": blocks("ubos"), "ssbos": blocks("ssbos"),
            "samplers": samplers,
            "inputs": [{"name": i.get("name"), "location": i.get("location")}
                       for i in raw.get("inputs", [])],
            "outputs": [{"name": o.get("name"), "location": o.get("location")}
                        for o in raw.get("outputs", [])]}


PACKAGE_SCHEMA_VERSION = 1

# --- Deterministic shader package metadata -----------------------------------
# Canonical, portable content hashes over the deployed artifact set. Used by
# BOTH this generator and scripts/check-shader-package.py (imported), so the
# hashing stays in lockstep by construction. All inputs are content-based
# (canonicalized JSON / portable source+spirv hashes) => identical across
# worktrees and machines.
def _canon_json(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def package_hashes(spv_dir: Path, index_records: list) -> dict:
    idx = sorted(({"key": r["key"], "artifact": r["artifact"]}
                  for r in index_records), key=lambda r: r["key"])
    sidecars, sources = {}, []
    for p in sorted(spv_dir.glob("*.json")):
        if p.name in ("spirv_index.json", "spirv_package.json"):
            continue
        d = json.load(open(p, encoding="utf-8"))
        sidecars[p.name] = d
        sources.append([d.get("base"), d.get("stage"), d.get("variant"),
                        d.get("source_sha256")])
    return {
        "spirv_index_sha256": sha256(_canon_json(idx)),
        "sidecars_sha256": sha256(_canon_json({k: sidecars[k] for k in sorted(sidecars)})),
        "source_set_sha256": sha256(_canon_json(sorted(sources))),
    }


def package_variant_matrix(cfg: dict) -> list:
    out = []
    for pilot in cfg["pilots"]:
        base = pilot["program"]
        rows = []
        for v in pilot["variants"]:
            name, defines = v["name"], sorted(v.get("defines", []))
            vid = variant_id(base, defines)
            rows.append({
                "name": name, "defines": defines, "variantId": vid,
                "stages": {st: artifact_name(base, st, name, vid)
                           for st in pilot["stages"]},
            })
        out.append({"base": base, "variants": rows})
    return out


def _tool_version(exe) -> str:
    if not exe:
        return "not-found"
    try:
        r = subprocess.run([exe, "--version"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        line = (r.stdout or r.stderr).splitlines()
        return line[0].strip() if line else "unknown"
    except Exception:
        return "unknown"


def build_package(cfg, spv_dir, index_records, glslang, spirv_cross, slangc=None):
    tools = {
        "glslangValidator": _tool_version(glslang),
        "spirv_cross": _tool_version(spirv_cross),
        "compile_flags": "-V --target-env vulkan1.3 --auto-map-locations (explicit descriptor bindings)",
    }
    if slangc:  # Slang also emits native Vulkan SPIR-V.
        tools["slangc"] = slang_compile.slangc_version(slangc)
        tools["slang_compile_flags"] = (
            f"-target spirv -profile {slang_compile.DEFAULT_PROFILE} "
            "-matrix-layout-column-major (-entry/-D/profile per sidecar)")
    return {
        "schema_version": PACKAGE_SCHEMA_VERSION,
        "slice": "SHADER-ARTIFACT-PACKAGE-METADATA-1",
        "generator": "tools/shader_offline_build/build_variants.py",
        "source_hash_canonicalization":
            "portable transitive source bundle with normalized newlines",
        "tools": tools,
        "runtime": {
            "env_gate": "none; offline verification subset of the native Vulkan shaders",
            "required_extension": "Vulkan 1.3",
            "fallback_policy": "native pipeline initialization failure; no GL fallback",
            "fatal_gate": "shader and pipeline initialization must succeed",
        },
        "pilot_families": [p["program"] for p in cfg["pilots"]],
        "excluded_families": [
            "other native Vulkan pipelines are baked by CMake",
        ],
        "variant_matrix": package_variant_matrix(cfg),
        "hashes": package_hashes(spv_dir, index_records),
    }


def _emit_record(args, fails, built, index, cfg, spv_dir, base, stage, vname,
                 defines, vid, art, src_hash, spirv_hash, refl, index_key, extra):
    """Sidecar + index record for a validated native stage."""
    sidecar = {
        "base": base, "stage": stage, "variant": vname,
        "defines": defines, "specializationParams": [],
        "artifact": art, "spv_dir": cfg.get("spv_dir", "shaders/spv"),
        "source_sha256": src_hash, "spirv_sha256": spirv_hash,
        "bindings": {"ubos": refl["ubos"], "ssbos": refl["ssbos"],
                     "samplers": refl["samplers"]},
        "interface": {"inputs": refl["inputs"], "outputs": refl["outputs"]},
        "shaderVariantId": vid,
    }
    sidecar.update(extra)
    sc_path = spv_dir / sidecar_name(base, stage, vname)
    if args.check:
        if not sc_path.exists():
            fails.append(f"{art}: sidecar missing {sc_path.name}")
        elif json.load(open(sc_path, encoding="utf-8")) != sidecar:
            fails.append(f"{art}: compiled shader or metadata drift vs sidecar")
    else:
        sc_path.write_text(json.dumps(sidecar, indent=2), encoding="utf-8")
    index.append({"key": index_key(base, stage, defines), "artifact": art,
                  "variantId": vid, "base": base, "stage": stage,
                  "defines": sorted(defines)})
    built.append(art)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--check", action="store_true",
                    help="verify existing artifacts match; write nothing")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--pilots", default=None,
                    help="alternate pilots manifest (tests; .slang stages allowed)")
    args = ap.parse_args()
    root = Path(args.root) if args.root else ROOT

    glslang = shader_common.find_tool("glslangValidator")
    spirv_cross = shader_common.find_tool("spirv-cross")
    if not glslang or not spirv_cross:
        print("[build_variants] FAIL: glslangValidator/spirv-cross not found "
              "(need Vulkan SDK on PATH or $VULKAN_SDK)", file=sys.stderr)
        return 2

    cfg = json.load(open(args.pilots or PILOTS, encoding="utf-8"))
    slangc = None
    if any(slang_compile.is_slang(rel)
           for p in cfg["pilots"] for rel in p["stages"].values()):
        slangc = slang_compile.find_slangc()
        if not slangc:
            print("[build_variants] FAIL: slangc not found (Vulkan SDK >= "
                  f"{slang_compile.min_version()} on PATH or $VULKAN_SDK)", file=sys.stderr)
            return 2
        verr = slang_compile.check_version(slangc)
        if verr:
            print(f"[build_variants] FAIL: {verr}", file=sys.stderr)
            return 2
    spv_dir = root / cfg.get("spv_dir", "shaders/spv")
    occ = occupancy_slots(root / BINDING_OCC.relative_to(ROOT))
    if not args.check:
        spv_dir.mkdir(parents=True, exist_ok=True)

    def index_key(base, stage, defines):
        # Stable package identity:
        # base|stage|";".join(sorted(defines)).
        return f"{base}|{stage}|" + ";".join(sorted(defines))

    fails, built, index = [], [], []
    for pilot in cfg["pilots"]:
        base = pilot["program"]
        for variant in pilot["variants"]:
            vname, defines = variant["name"], variant.get("defines", [])
            vid = variant_id(base, defines)
            for stage, rel in pilot["stages"].items():
                art = artifact_name(base, stage, vname, vid)
                spv_path = spv_dir / art
                if slang_compile.is_slang(rel):
                    src_path = root / rel
                    try:
                        src_hash = sha256(slang_compile.source_bundle(src_path))
                    except (OSError, ValueError) as e:
                        fails.append(f'{art}: {e}')
                        continue
                    entry = pilot.get("entries", {}).get(stage, "main")
                    prof = pilot.get("spirv", slang_compile.DEFAULT_PROFILE)
                    with tempfile.TemporaryDirectory() as td:
                        out = spv_path if not args.check else Path(td) / art
                        ok, diag = slang_compile.compile_slang(
                            slangc, src_path, stage, out, entry, prof, defines)
                        if not ok:
                            fails.append(f"{art}: slangc failed: {diag[:600]}")
                            continue
                        spirv_hash = sha256(out.read_bytes())
                        try:
                            refl = reflect(out, spirv_cross, src_path, root)
                        except ValueError as e:
                            fails.append(f'{art}: {e}')
                            out.unlink(missing_ok=True)
                            continue
                    _emit_record(args, fails, built, index, cfg, spv_dir, base, stage,
                                 vname, defines, vid, art, src_hash, spirv_hash, refl,
                                 index_key, extra={"language": "slang", "entry": entry,
                                                   "spirvProfile": prof})
                    continue
                src_path = root / rel
                src_hash = sha256(slang_compile.source_bundle(src_path))
                # Compile the actual native source; includes retain their source directories.
                with tempfile.TemporaryDirectory() as td:
                    out = spv_path if not args.check else Path(td) / art
                    r = subprocess.run(
                        [glslang, "-V", "--target-env", "vulkan1.3", "--auto-map-locations",
                         "-S", stage, f"-I{src_path.parent}", f"-I{root / 'shaders'}",
                         f"-I{root / '3rdparty/include'}", *["-D" + d for d in defines],
                         str(src_path), "-o", str(out)],
                        capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
                    if r.returncode != 0 or not out.exists():
                        fails.append(f"{art}: glslang failed: "
                                     f"{(r.stdout + r.stderr).strip()[:300]}")
                        continue
                    spirv_hash = sha256(out.read_bytes())
                    refl = reflect(out, spirv_cross)

                # Match native buffers by source, role, descriptor set and binding.
                for blk in refl["ubos"] + refl["ssbos"]:
                    b = blk.get("binding")
                    if b is None:
                        continue
                    ns = "UBO" if blk in refl["ubos"] else "SSBO"
                    if occ is not None and (ns, blk.get("set", 0), b, blk["name"], rel) not in occ:
                        fails.append(
                            f"{art}: reflected {ns} '{blk['name']}' binding={b} "
                            f"not present in binding-slot-occupancy.json (drift)")

                _emit_record(args, fails, built, index, cfg, spv_dir, base, stage,
                             vname, defines, vid, art, src_hash, spirv_hash, refl,
                             index_key, extra={"language": "glsl", "targetEnv": "vulkan1.3"})

    # Offline subset index; runtime shader modules are baked by CMake.
    idx_path = spv_dir / "spirv_index.json"
    idx_doc = {"slice": "SPIRV-KEYED-VARIANT-CONSUMER-1", "records": index}
    if args.check:
        if not idx_path.exists():
            fails.append("spirv_index.json missing — run build_variants.py")
        else:
            old = json.load(open(idx_path, encoding="utf-8")).get("records", [])
            if {r["key"]: r["artifact"] for r in old} != \
               {r["key"]: r["artifact"] for r in index}:
                fails.append("spirv_index.json out of sync with built variants")
    elif index:
        idx_path.write_text(json.dumps(idx_doc, indent=2), encoding="utf-8")

    # Deterministic package metadata file.
    pkg_path = spv_dir / "spirv_package.json"
    if index:
        pkg = build_package(cfg, spv_dir, index, glslang, spirv_cross, slangc)
        if args.check:
            if not pkg_path.exists():
                fails.append("spirv_package.json missing — run build_variants.py")
            else:
                old = json.load(open(pkg_path, encoding="utf-8"))
                # full compare incl tool versions (this mode re-ran the tools)
                if old != pkg:
                    diff = [k for k in set(old) | set(pkg) if old.get(k) != pkg.get(k)]
                    fails.append(f"spirv_package.json stale (differs in: {sorted(diff)})")
        else:
            pkg_path.write_text(json.dumps(pkg, indent=2), encoding="utf-8")

    if not args.quiet:
        mode = "CHECK" if args.check else "BUILD"
        print(f"[build_variants] OFFLINE-SHADER-VARIANT-BUILD-1 ({mode})")
        print(f"  output dir : {spv_dir.relative_to(root)}")
        for a in built:
            print(f"    {'ok' if a not in [f.split(':')[0] for f in fails] else 'FAIL'}  {a}")
        for f in fails:
            print(f"  FAIL: {f}")
        print(f"  result: {'FAIL' if fails else 'PASS'} "
              f"({len(built)} artifacts, {len(fails)} fail)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
