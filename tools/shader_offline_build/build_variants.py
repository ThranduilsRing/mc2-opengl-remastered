#!/usr/bin/env python3
"""build_variants.py — OFFLINE-SHADER-VARIANT-BUILD-1

Compile deployable OpenGL SPIR-V artifacts for the pilot shader families, WITHOUT
changing any runtime code. This is the offline-bake half of the SPIR-V seam
identified in spirv-consumer-pilot-recon-1.md; the runtime consumer
(compile_shader branch + glSpecializeShader) is a SEPARATE later slice.

Per (pilot, variant, stage):
  - build the exact source the engine compiles (shader_common.build_shader_source
    = #version prefix + flattened #includes + inventory define-set);
  - compile to OpenGL SPIR-V with `glslangValidator -G --auto-map-locations`
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

PILOTS = ROOT / "tools" / "shader_offline_build" / "pilots.json"
BINDING_OCC = ROOT / "docs" / "render-backend-seams" / "binding-slot-occupancy.json"


import re as _re

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


def occupancy_slots():
    """Return {('SSBO'|'UBO', slot)} present in binding-slot-occupancy.json."""
    if not BINDING_OCC.exists():
        return None
    occ = json.load(open(BINDING_OCC, encoding="utf-8")).get("occupancy", {})
    out = set()
    for key in occ:  # keys look like "SSBO:2" / "UBO:3"
        ns, _, slot = key.partition(":")
        if slot.isdigit():
            out.add((ns, int(slot)))
    return out


def reflect(spv: Path, spirv_cross: str):
    raw = json.loads(subprocess.run(
        [spirv_cross, str(spv), "--reflect"],
        capture_output=True, text=True, check=True).stdout)
    def blocks(key):
        return [{"name": b.get("name"), "binding": b.get("binding"),
                 "set": b.get("set")} for b in raw.get(key, [])]
    samplers = [{"name": t.get("name"), "type": t.get("type"),
                 "location": t.get("location"), "binding": t.get("binding")}
                for t in raw.get("textures", [])]
    return {"ubos": blocks("ubos"), "ssbos": blocks("ssbos"),
            "samplers": samplers,
            "inputs": [{"name": i.get("name"), "location": i.get("location")}
                       for i in raw.get("inputs", [])],
            "outputs": [{"name": o.get("name"), "location": o.get("location")}
                        for o in raw.get("outputs", [])]}


PACKAGE_SCHEMA_VERSION = 1

# --- SHADER-ARTIFACT-PACKAGE-METADATA-1 -------------------------------------
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
        r = subprocess.run([exe, "--version"], capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        line = (r.stdout or r.stderr).splitlines()
        return line[0].strip() if line else "unknown"
    except Exception:
        return "unknown"


def build_package(cfg, spv_dir, index_records, glslang, spirv_cross):
    return {
        "schema_version": PACKAGE_SCHEMA_VERSION,
        "slice": "SHADER-ARTIFACT-PACKAGE-METADATA-1",
        "generator": "tools/shader_offline_build/build_variants.py",
        "source_hash_canonicalization":
            "canon_source: strip '#line N // <path>' labels + normalize CRLF->LF",
        "tools": {
            "glslangValidator": _tool_version(glslang),
            "spirv_cross": _tool_version(spirv_cross),
            "compile_flags": "-G --auto-map-locations (NOT --auto-map-bindings)",
        },
        "runtime": {
            "env_gate": "MC2_SHADER_SPIRV=1 (default OFF)",
            "required_extension": "GL_ARB_gl_spirv || GL 4.6",
            "fallback_policy": "program-atomic GLSL fallback (any stage miss -> whole program GLSL)",
            "fatal_gate": "MC2_SHADER_SPIRV_FATAL=1 (assert instead of fallback; default OFF)",
        },
        "pilot_families": [p["program"] for p in cfg["pilots"]],
        "excluded_families": [
            "shadow_mech (shadow_mech.vert + shadow_instanced.frag) — separate program, stays GLSL",
            "all non-pilot shader programs — stay GLSL",
        ],
        "variant_matrix": package_variant_matrix(cfg),
        "hashes": package_hashes(spv_dir, index_records),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--check", action="store_true",
                    help="verify existing artifacts match; write nothing")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    root = Path(args.root) if args.root else ROOT

    glslang = shader_common.find_tool("glslangValidator")
    spirv_cross = shader_common.find_tool("spirv-cross")
    if not glslang or not spirv_cross:
        print("[build_variants] FAIL: glslangValidator/spirv-cross not found "
              "(need Vulkan SDK on PATH or $VULKAN_SDK)", file=sys.stderr)
        return 2

    cfg = json.load(open(PILOTS, encoding="utf-8"))
    spv_dir = root / cfg.get("spv_dir", "shaders/spv")
    occ = occupancy_slots()
    if not args.check:
        spv_dir.mkdir(parents=True, exist_ok=True)

    def index_key(base, stage, defines):
        # MUST match the runtime consumer's spirvDefineKey canonicalization:
        # base|stage|";".join(sorted(defines)).
        return f"{base}|{stage}|" + ";".join(sorted(defines))

    fails, built, index = [], [], []
    for pilot in cfg["pilots"]:
        base = pilot["program"]
        for variant in pilot["variants"]:
            vname, defines = variant["name"], variant.get("defines", [])
            vid = variant_id(base, defines)
            for stage, rel in pilot["stages"].items():
                src_path = root / rel
                src = shader_common.build_shader_source(src_path, defines)
                src_hash = sha256(canon_source(src))
                art = artifact_name(base, stage, vname, vid)
                spv_path = spv_dir / art
                # compile to a temp source, then to the artifact path
                with tempfile.TemporaryDirectory() as td:
                    tmp = Path(td) / src_path.name
                    tmp.write_text(src, encoding="utf-8")
                    out = spv_path if not args.check else Path(td) / art
                    r = subprocess.run(
                        [glslang, "-G", "--auto-map-locations", "-S", stage,
                         str(tmp), "-o", str(out)],
                        capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                    if r.returncode != 0 or not out.exists():
                        fails.append(f"{art}: glslang failed: "
                                     f"{(r.stdout + r.stderr).strip()[:300]}")
                        continue
                    spirv_hash = sha256(out.read_bytes())
                    refl = reflect(out, spirv_cross)

                # binding-manifest agreement (UBO/SSBO only; samplers are
                # location-based default-uniforms here, not binding-base).
                for blk in refl["ubos"] + refl["ssbos"]:
                    b = blk.get("binding")
                    if b is None:
                        continue
                    ns = "UBO" if blk in refl["ubos"] else "SSBO"
                    if occ is not None and (ns, b) not in occ:
                        fails.append(
                            f"{art}: reflected {ns} '{blk['name']}' binding={b} "
                            f"not present in binding-slot-occupancy.json (drift)")

                sidecar = {
                    "base": base, "stage": stage, "variant": vname,
                    "defines": defines, "specializationParams": [],
                    "artifact": art, "spv_dir": cfg.get("spv_dir", "shaders/spv"),
                    "source_sha256": src_hash, "spirv_sha256": spirv_hash,
                    "bindings": {"ubos": refl["ubos"], "ssbos": refl["ssbos"],
                                 "samplers": refl["samplers"]},
                    "interface": {"inputs": refl["inputs"],
                                  "outputs": refl["outputs"]},
                    "shaderVariantId": vid,
                }
                sc_path = spv_dir / sidecar_name(base, stage, vname)
                if args.check:
                    if not sc_path.exists():
                        fails.append(f"{art}: sidecar missing {sc_path.name}")
                    else:
                        old = json.load(open(sc_path, encoding="utf-8"))
                        if old.get("source_sha256") != src_hash:
                            fails.append(f"{art}: source hash drift vs sidecar")
                else:
                    sc_path.write_text(json.dumps(sidecar, indent=2), encoding="utf-8")
                # index record ("key" before "artifact" — the consumer pairs by
                # order). Keep this canonicalization in lockstep with the C++
                # spirvDefineKey() in shader_builder.cpp.
                index.append({"key": index_key(base, stage, defines),
                              "artifact": art, "variantId": vid,
                              "base": base, "stage": stage,
                              "defines": sorted(defines)})
                built.append(art)

    # Deployed variant index (one file the runtime consumer reads).
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

    # SHADER-ARTIFACT-PACKAGE-METADATA-1: deterministic package metadata file.
    pkg_path = spv_dir / "spirv_package.json"
    if index:
        pkg = build_package(cfg, spv_dir, index, glslang, spirv_cross)
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
