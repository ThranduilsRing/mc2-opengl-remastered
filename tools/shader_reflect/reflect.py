#!/usr/bin/env python3
"""tools/shader_reflect/reflect.py — Shader contract reflection CI (Tier 1.2).

Compiles each shader+variant to SPIR-V via glslangValidator, runs
spirv-cross --reflect, normalizes the output, and compares against
expected/<golden_key>.json files.

Usage:
  py -3 tools/shader_reflect/reflect.py            # compare vs goldens
  py -3 tools/shader_reflect/reflect.py --update   # regenerate goldens
  py -3 tools/shader_reflect/reflect.py --shader shaders/static_prop.frag

Exit 0 = all PASS. Exit 1 = DRIFT / NEW / COMPILE_ERROR / CONTRACT_VIOLATION.
"""
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# shader_common lives in scripts/; add it to path relative to repo root.
_REFLECT_DIR = Path(__file__).resolve().parent
ROOT = _REFLECT_DIR.parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import shader_common
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

EXPECTED_DIR = _REFLECT_DIR / "expected"

# ---------------------------------------------------------------------------
# SHADER_VARIANTS
# Key: repo-relative posix path.
# Value: list of variant dicts with keys:
#   name     - variant name string
#   defines  - extra #define injections (e.g. ["MC2_COALESCE=1"])
#   version  - version_override for build_shader_source (None = use default)
#   rewrites - extra token rewrites applied after per-shader SHADER_TOKEN_REWRITES
#
# Coalesce variants of static_prop.vert use gl_BaseInstanceARB which requires
# GL_ARB_shader_draw_parameters. glslangValidator v12 does not implement ARB-
# suffixed draw-params builtins; the workaround mirrors gos_terrain_water_fast_mdi:
# compile under #version 460 (core) and rewrite the ARB names to core names.
# ---------------------------------------------------------------------------
_V460 = "#version 460\n"
_DRAW_PARAMS_REWRITES = [
    ("gl_BaseInstanceARB", "gl_BaseInstance"),
    ("gl_DrawIDARB", "gl_DrawID"),
]


def _v(name: str, defines: list[str],
       version: str | None = None,
       rewrites: list[tuple[str, str]] | None = None) -> dict:
    return {"name": name, "defines": defines,
            "version": version, "rewrites": rewrites or []}


# Runtime variants are native Vulkan pipelines. Only offline ABI fixtures need a prefix.
SHADER_VARIANTS: dict[str, list[dict]] = {
    "shaders/fixtures/material_gpu_contract.frag": [_v("default", [])],
}

# Macros that gate variant-specific bindings. Used for coverage audit.
KNOWN_VARIANT_MACROS: frozenset[str] = frozenset(
    {"MC2_COALESCE", "MC2_OBJECT_ID_BUFFER"}
)

# ---------------------------------------------------------------------------
# REQUIRED_INVARIANTS — checked after golden comparison.
# CONTRACT_VIOLATION exits 1 even when --update is passed.
# To change a value here, update the spec and this table in the same commit.
# ---------------------------------------------------------------------------
REQUIRED_INVARIANTS: list[dict] = [
    # Active Vulkan props and mechs co-write the shared scene normal target.
    # The deleted GL object-ID variants and PerDrawEntry SSBO have no native equivalent.
    {
        "shader": "shaders/vulkan/color_prop_tier1.frag",
        "variant": "default",
        "check": "output",
        "name": "oGBuffer1",
        "location": 1,
        "type": "vec4",
    },
    {
        "shader": "shaders/vulkan/mech_tier1.frag",
        "variant": "default",
        "check": "output",
        "name": "oGBuffer1",
        "location": 1,
        "type": "vec4",
    },
    # MaterialGpu field offsets — lock struct layout independently of golden.
    # type_name "MaterialGpu" confirmed by bootstrap spirv-cross inspection (Task 3).
    # These survive --update: CONTRACT_VIOLATION fires even on golden regeneration.
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "albedoTex",
        "offset": 0,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "normalTex",
        "offset": 4,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "metallicRoughnessTex",
        "offset": 8,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "emissiveTex",
        "offset": 12,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "flags",
        "offset": 16,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "baseColorFactor",
        "offset": 20,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "metallicFactor",
        "offset": 24,
    },
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "type_member",
        "type_name": "MaterialGpu",
        "member": "roughnessFactor",
        "offset": 28,
    },
    # MaterialGpu array stride — hard gate: --update cannot bless a stride change.
    {
        "shader": "shaders/fixtures/material_gpu_contract.frag",
        "variant": "default",
        "check": "ssbo_member",
        "block": "MaterialTable",
        "member": "materials",
        "offset": 0,
        "array_stride": 32,
    },
]


def golden_key(shader_rel: str, variant: str) -> str:
    """Collision-proof key: 'shaders/static_prop.frag' + 'default' ->
    'shaders__static_prop.frag__default'."""
    safe = shader_rel.replace("/", "__").replace("\\", "__")
    return f"{safe}__{variant}"


def golden_path(shader_rel: str, variant: str) -> Path:
    return EXPECTED_DIR / f"{golden_key(shader_rel, variant)}.json"


def _decode(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


def _find_tools() -> tuple[str, str] | None:
    """Return (glslang, spirv_cross) or None if either missing."""
    glslang = shader_common.find_tool("glslangValidator")
    spirv_cross = shader_common.find_tool("spirv-cross")
    if not glslang or not spirv_cross:
        return None
    return glslang, spirv_cross


def _print_tool_versions(glslang: str, spirv_cross: str) -> None:
    """Log tool versions to stdout (not included in goldens)."""
    for label, tool in (("glslangValidator", glslang), ("spirv-cross", spirv_cross)):
        try:
            r = subprocess.run([tool, "--version"], capture_output=True)
            ver = (_decode(r.stdout) + _decode(r.stderr)).strip().splitlines()
            print(f"  {label}: {ver[0] if ver else 'unknown'}")
        except OSError:
            print(f"  {label}: version check failed")


def compile_to_spv(
    shader: Path,
    extra_defines: list[str],
    extra_token_rewrites: list[tuple[str, str]],
    glslang: str,
    version_override: str | None = None,
) -> tuple[bool, str, Path | None]:
    """Compile shader to SPIR-V. Returns (ok, diagnostic, spv_path_or_None).

    spv_path is a temp file the caller must delete. On failure, spv_path is None.
    reflect.py invokes glslangValidator only to obtain SPIR-V for spirv-cross.
    tools/check_vk_shaders.py (row vk_shaders_compile) remains the authoritative compile gate.
    """
    stage = shader_common.STAGE_BY_EXT[shader.suffix]
    try:
        if shader.is_relative_to(ROOT / "shaders/vulkan"):
            # Compile the real native source and its SDK include directives, without a GL prefix.
            src = shader.read_text(encoding="utf-8")
            if extra_defines:
                first, rest = src.split("\n", 1)
                src = first + "\n" + "".join("#define " + d.replace("=", " ", 1) + "\n" for d in extra_defines) + rest
        else:
            src = shader_common.build_shader_source(
                shader, extra_defines, version_override, extra_token_rewrites
            )
    except OSError as e:
        return False, f"read error: {e}", None
    except ValueError as e:
        return False, f"include error: {e}", None

    fd, tmp_name = tempfile.mkstemp(
        suffix=shader.suffix, prefix="_reflect_", dir=str(shader_common.SHADERS)
    )
    tmp = Path(tmp_name)
    spv_path = tmp.with_suffix(tmp.suffix + ".spv")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(src)

        cmd = [
            glslang,
            "-S", stage,
            "--auto-map-locations",
            "--auto-map-bindings",
            "-I" + str(shader.parent),
        ]
        cmd += ["-V", "-R"]
        if "GL_KHR_shader_subgroup" in src:   # mirrors MC2_VK_SHADER_DEFINES_<shader> in GameOS/gameos/CMakeLists.txt
            cmd += ["--target-env", "vulkan1.1"]
        cmd += [str(tmp), "-o", str(spv_path)]

        proc = subprocess.run(cmd, capture_output=True, **console_child.child_kwargs(redirected=True))
        if proc.returncode != 0:
            diag = (_decode(proc.stdout) + _decode(proc.stderr)).strip()
            diag = diag.replace(tmp.name, shader.name).replace(str(tmp), str(shader))
            # Clean up any partial .spv glslangValidator may have written.
            try:
                spv_path.unlink()
            except OSError:
                pass
            return False, diag, None
        return True, "", spv_path
    except Exception as e:
        try:
            spv_path.unlink()
        except OSError:
            pass
        return False, str(e), None
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass


def reflect_spv(spv_path: Path, spirv_cross: str) -> tuple[bool, str, dict]:
    """Run spirv-cross --reflect. Returns (ok, diagnostic, raw_json_dict)."""
    try:
        # Input file first, then --reflect flag (standard spirv-cross CLI shape).
        proc = subprocess.run(
            [spirv_cross, str(spv_path), "--reflect"],
            capture_output=True,
        )
    except OSError as e:
        return False, f"spirv-cross exec error: {e}", {}

    if proc.returncode != 0:
        diag = (_decode(proc.stdout) + _decode(proc.stderr)).strip()
        return False, diag, {}

    raw_text = _decode(proc.stdout)
    try:
        raw = json.loads(raw_text)
        return True, "", raw
    except json.JSONDecodeError as e:
        return False, f"spirv-cross JSON parse error: {e}\n{raw_text[:400]}", {}


def _norm_type(t: str) -> str:
    """Normalize spirv-cross type aliases to canonical form."""
    return {"mat4x4": "mat4", "mat3x3": "mat3", "mat2x2": "mat2"}.get(t, t)


def _norm_stride(v) -> int | None:
    """spirv-cross uses 0 for 'not applicable'; map to null."""
    if v is None or v == 0:
        return None
    return int(v)


def _norm_member(m: dict) -> dict:
    return {
        "array_stride": _norm_stride(m.get("array_stride")),
        "matrix_stride": _norm_stride(m.get("matrix_stride")),
        "name": m["name"],
        "offset": int(m["offset"]),
        "type": _norm_type(m.get("type", "unknown")),
    }


def _norm_block(raw: dict, b: dict) -> dict:
    # spirv-cross --reflect does NOT inline members on UBO/SSBO resources.
    # Members live in raw["types"][str(type_id)]["members"].
    type_id = b.get("type")
    members_list: list[dict] = []
    if type_id is not None:
        type_info = raw.get("types", {}).get(str(type_id), {})
        members_list = type_info.get("members", [])
    members = sorted(
        [_norm_member(m) for m in members_list],
        key=lambda m: (m["offset"], m["name"]),
    )
    return {
        "binding": int(b["binding"]),
        "members": members,
        "name": b["name"],
    }


def _norm_output(o: dict) -> dict:
    return {
        "location": int(o["location"]),
        "name": o["name"],
        "type": _norm_type(o.get("type", "unknown")),
    }


def normalize(
    raw: dict,
    shader_rel: str,
    stage: str,
    variant: str,
    defines: list[str],
) -> dict:
    """Normalize a spirv-cross --reflect JSON dict to our stable schema.

    Sorts all collections so golden diffs are stable across SPIR-V changes
    that reorder internal IDs. sort_keys=True on JSON serialization handles
    the rest.
    """
    ubos = sorted(
        [_norm_block(raw, b) for b in raw.get("ubos", [])],
        key=lambda b: (b["binding"], b["name"]),
    )
    ssbos = sorted(
        [_norm_block(raw, b) for b in raw.get("ssbos", [])],
        key=lambda b: (b["binding"], b["name"]),
    )
    outputs: list[dict] = []
    if stage == "frag":
        outputs = sorted(
            [_norm_output(o) for o in raw.get("outputs", [])],
            key=lambda o: (o["location"], o["name"]),
        )
    return {
        "defines": list(defines),
        "outputs": outputs,
        "shader": shader_rel,
        "ssbos": ssbos,
        "stage": stage,
        "ubos": ubos,
        "variant": variant,
    }


def serialize_contract(contract: dict) -> str:
    """Canonical serialization: sort_keys, 2-space indent, trailing newline."""
    return json.dumps(contract, indent=2, sort_keys=True) + "\n"


def diff_contracts(expected: dict, actual: dict) -> list[str]:
    exp_str = serialize_contract(expected)
    act_str = serialize_contract(actual)
    if exp_str == act_str:
        return []
    return list(
        difflib.unified_diff(
            exp_str.splitlines(keepends=True),
            act_str.splitlines(keepends=True),
            fromfile="expected",
            tofile="actual",
        )
    )


def load_golden(shader_rel: str, variant: str) -> dict | None:
    p = golden_path(shader_rel, variant)
    if not p.exists():
        return None
    return json.loads(p.read_text(encoding="utf-8"))


def save_golden(shader_rel: str, variant: str, contract: dict) -> None:
    p = golden_path(shader_rel, variant)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(serialize_contract(contract), encoding="utf-8")


def check_variant_coverage(
    shader_rel: str, shader: Path, strict: bool
) -> list[str]:
    """Warn if shader uses KNOWN_VARIANT_MACROS but has no SHADER_VARIANTS entry."""
    if shader_rel in SHADER_VARIANTS:
        return []
    try:
        # Use include-expanded source so macros in headers are caught.
        src = shader_common.build_shader_source(shader)
    except (OSError, ValueError):
        return []
    # Strip line comments, then look only inside preprocessor directive lines.
    src_no_comments = re.sub(r"//[^\n]*", "", src)
    found = {
        m for m in KNOWN_VARIANT_MACROS
        if re.search(
            rf"^\s*#\s*(?:if|ifdef|ifndef|elif)\b[^\n]*\b{re.escape(m)}\b",
            src_no_comments,
            re.MULTILINE,
        )
    }
    if not found:
        return []
    prefix = "FAIL" if strict else "WARNING"
    return [
        f"{prefix}: {shader_rel} uses {m} but has no SHADER_VARIANTS entry. "
        f"Add it or this variant's bindings will not be reflected."
        for m in sorted(found)
    ]


def check_invariants(
    reflected: dict[str, dict],
    raw_by_key: dict[str, dict] | None = None,
    allow_partial: bool = False,
) -> list[str]:
    """Check REQUIRED_INVARIANTS against reflected contracts.

    reflected: {f"{shader_rel}/{variant}": contract_dict}
    raw_by_key: {f"{shader_rel}/{variant}": raw_spirv_cross_json}
      Required for "type_member" checks. Pass None to skip those.
    allow_partial: if True, silently skip invariants for shaders not in reflected
      (used when --shader restricts to a subset of shaders).

    Returns list of CONTRACT_VIOLATION messages. An empty list = all pass.
    Invariant failures are NOT bypassable by --update.
    """
    violations: list[str] = []

    for inv in REQUIRED_INVARIANTS:
        shader_rel = inv["shader"]
        variant = inv["variant"]
        key = f"{shader_rel}/{variant}"
        contract = reflected.get(key)

        if contract is None:
            if allow_partial:
                continue
            violations.append(
                f"CONTRACT_VIOLATION: {key} not in reflected results "
                f"(shader not compiled or skipped)"
            )
            continue

        check = inv["check"]

        if check == "output":
            matches = [
                o for o in contract.get("outputs", [])
                if o["name"] == inv["name"]
            ]
            if not matches:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: output '{inv['name']}' not found"
                )
                continue
            o = matches[0]
            if o["location"] != inv["location"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: output '{inv['name']}' "
                    f"location={o['location']}, expected {inv['location']}"
                )
            if o["type"] != inv["type"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: output '{inv['name']}' "
                    f"type={o['type']!r}, expected {inv['type']!r}"
                )

        elif check == "ssbo_member":
            ssbos = [s for s in contract.get("ssbos", []) if s["name"] == inv["block"]]
            if not ssbos:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: ssbo '{inv['block']}' not found"
                )
                continue
            members = [m for m in ssbos[0]["members"] if m["name"] == inv["member"]]
            if not members:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"ssbo member '{inv['block']}.{inv['member']}' not found"
                )
                continue
            m = members[0]
            if m["offset"] != inv["offset"]:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"ssbo member '{inv['block']}.{inv['member']}' "
                    f"offset={m['offset']}, expected {inv['offset']}"
                )
            if "array_stride" in inv:
                # m["array_stride"] is _norm_stride-normalized: 0 -> None.
                # Invariant values must be non-zero real strides; "array_stride": 0
                # would always violate because normalized None != 0.
                if m.get("array_stride") != inv["array_stride"]:
                    violations.append(
                        f"CONTRACT_VIOLATION: {key}: "
                        f"ssbo member '{inv['block']}.{inv['member']}' "
                        f"array_stride={m.get('array_stride')}, "
                        f"expected {inv['array_stride']}"
                    )

        elif check == "type_member":
            # Check a named struct type's member offset via raw spirv-cross JSON.
            # Use this for nested structs (e.g. PerDrawEntry inside entries[]).
            if raw_by_key is None:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: type_member check requires raw_by_key"
                )
                continue
            raw = raw_by_key.get(key, {})
            type_name = inv["type_name"]
            member_name = inv["member"]
            expected_offset = inv["offset"]

            # spirv-cross keys types by numeric ID; scan for name.
            # spirv-cross may emit two entries with the same struct name: one
            # abstract (no member offsets) and one decorated (with offsets,
            # used inside an SSBO/UBO block). We want the decorated version.
            # Prefer the first type whose members all carry "offset" keys.
            found_type: dict | None = None
            for tinfo in raw.get("types", {}).values():
                if tinfo.get("name") != type_name:
                    continue
                members_raw = tinfo.get("members", [])
                if members_raw and all("offset" in m for m in members_raw):
                    found_type = tinfo
                    break
                # Fall back: accept if no better candidate found yet.
                if found_type is None:
                    found_type = tinfo

            if found_type is None:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: type '{type_name}' not found "
                    f"in spirv-cross reflection"
                )
                continue

            members = [m for m in found_type.get("members", [])
                       if m["name"] == member_name]
            if not members:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"type member '{type_name}.{member_name}' not found"
                )
                continue
            actual_offset = int(members[0]["offset"])
            if actual_offset != expected_offset:
                violations.append(
                    f"CONTRACT_VIOLATION: {key}: "
                    f"type member '{type_name}.{member_name}' "
                    f"offset={actual_offset}, expected {expected_offset}"
                )

    return violations


# Result tags (printed per shader/variant).
_TAG_PASS    = "PASS"
_TAG_DRIFT   = "DRIFT"
_TAG_NEW     = "NEW"
_TAG_SKIP    = "SKIP"
_TAG_COMPILE = "COMPILE_ERROR"
_TAG_WARN    = "WARNING"
_TAG_FAIL    = "FAIL"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Shader contract reflection CI (Tier 1.2)."
    )
    ap.add_argument(
        "--update",
        action="store_true",
        help="Regenerate expected/*.json goldens. Rejected when $CI is set.",
    )
    ap.add_argument(
        "--shader",
        action="append",
        metavar="PATH",
        help="Reflect only this shader (repeatable). Implies all variants.",
    )
    ap.add_argument(
        "--variant",
        metavar="NAME",
        help="Restrict to this variant name (use with --shader).",
    )
    ap.add_argument(
        "--strict-variants",
        action="store_true",
        help="Promote variant-coverage WARNINGs to FAILs.",
    )
    args = ap.parse_args()

    # CI guard: reject --update in CI environments.
    if args.update and os.environ.get("CI"):
        print(
            "reflect: --update is not allowed in CI environments. "
            "Run locally and commit the updated expected/*.json files.",
            file=sys.stderr,
        )
        return 1

    tools = _find_tools()
    if tools is None:
        sdk = os.environ.get("VULKAN_SDK", "<unset>")
        print(
            "reflect: cannot find glslangValidator and/or spirv-cross.\n"
            f"  $VULKAN_SDK = {sdk}\n"
            "  Install the Vulkan SDK and ensure $VULKAN_SDK/Bin/ is populated.",
            file=sys.stderr,
        )
        return 1
    glslang, spirv_cross = tools

    print("reflect: tool versions:")
    _print_tool_versions(glslang, spirv_cross)


    # Build target list.
    if args.shader:
        targets = [Path(s).resolve() for s in args.shader]
        for t in targets:
            if t.suffix not in shader_common.STAGE_BY_EXT:
                print(f"reflect: unknown stage for {t}", file=sys.stderr)
                return 1
    else:
        targets = shader_common.discover_shaders()

    passed = skipped = compile_errors = drifted = new_goldens = warnings = 0
    all_diffs: list[str] = []
    # reflected: keyed by "shader_rel/variant" -> normalized contract
    reflected: dict[str, dict] = {}
    # raw_by_key: keyed by "shader_rel/variant" -> raw spirv-cross JSON
    # Required for type_member invariant checks on nested structs.
    raw_by_key: dict[str, dict] = {}
    # Collect --update writes; only commit to disk after invariant checks pass.
    pending_updates: list[tuple[str, str, dict]] = []

    for shader in targets:
        shader_rel = shader.relative_to(ROOT).as_posix()
        skip_reason = shader_common.SKIP_SHADERS.get(shader.name)
        if skip_reason:
            skipped += 1
            print(f"[{_TAG_SKIP}] {shader_rel}: {skip_reason}")
            continue

        # Variant coverage audit.
        coverage_msgs = check_variant_coverage(shader_rel, shader, args.strict_variants)
        for msg in coverage_msgs:
            print(msg)
            if msg.startswith("FAIL"):
                drifted += 1
            else:
                warnings += 1

        variants = SHADER_VARIANTS.get(shader_rel, [_v("default", [])])
        if args.variant:
            variants = [v for v in variants if v["name"] == args.variant]
            if not variants:
                print(
                    f"reflect: variant '{args.variant}' not found for {shader_rel}",
                    file=sys.stderr,
                )
                return 1

        for vdef in variants:
            variant_name = vdef["name"]
            extra_defines = vdef["defines"]
            label = f"{shader_rel} [{variant_name}]"

            ok, diag, spv_path = compile_to_spv(
                shader, extra_defines, vdef["rewrites"], glslang,
                version_override=vdef["version"],
            )
            if not ok:
                compile_errors += 1
                print(f"[{_TAG_COMPILE}] {label}")
                print(f"  {diag.splitlines()[0] if diag else '(no diagnostic)'}")
                all_diffs.append(f"--- {label} (COMPILE_ERROR) ---\n{diag}\n")
                continue

            assert spv_path is not None
            try:
                ok2, diag2, raw = reflect_spv(spv_path, spirv_cross)
            finally:
                try:
                    spv_path.unlink()
                except OSError:
                    pass

            if not ok2:
                compile_errors += 1
                print(f"[{_TAG_COMPILE}] {label}: spirv-cross failed")
                all_diffs.append(f"--- {label} (REFLECT_ERROR) ---\n{diag2}\n")
                continue

            stage = shader_common.STAGE_BY_EXT[shader.suffix]
            contract = normalize(raw, shader_rel, stage, variant_name, extra_defines)
            key = f"{shader_rel}/{variant_name}"
            reflected[key] = contract
            raw_by_key[key] = raw

            if args.update:
                # Stash; write only after invariant checks pass.
                pending_updates.append((shader_rel, variant_name, contract))
                print(f"[UPDATE] {label}")
                passed += 1
                continue

            expected = load_golden(shader_rel, variant_name)
            if expected is None:
                new_goldens += 1
                print(
                    f"[{_TAG_NEW}] {label}: no golden found. "
                    f"Run --update to bootstrap."
                )
                continue

            diff = diff_contracts(expected, contract)
            if diff:
                drifted += 1
                first = next(
                    (l.strip() for l in diff if l.startswith(("+", "-"))
                     and not l.startswith(("---", "+++")))
                    , "(see diff)"
                )
                print(f"[{_TAG_DRIFT}] {label}: {first}")
                all_diffs.append(
                    f"--- {label} (DRIFT) ---\n" + "".join(diff) + "\n"
                )
            else:
                passed += 1
                print(f"[{_TAG_PASS}] {label}")

    # Invariant checks — run even on --update to catch bypasses.
    # allow_partial when --shader restricts to a subset: don't flag other
    # shaders as CONTRACT_VIOLATION just because they weren't compiled.
    violations = check_invariants(reflected, raw_by_key,
                                  allow_partial=bool(args.shader))
    for v in violations:
        print(v, file=sys.stderr)

    # Write goldens only when --update AND no invariant violations.
    # If violations exist, goldens are NOT written — bad values never land on disk.
    if args.update:
        if violations:
            print(
                f"\nreflect: {len(violations)} invariant violation(s) detected — "
                "goldens NOT written. Fix CONTRACT_VIOLATION(s) above before "
                "updating goldens.",
                file=sys.stderr,
            )
        else:
            for sr, vn, ct in pending_updates:
                save_golden(sr, vn, ct)

    total = passed + skipped + compile_errors + drifted + new_goldens
    mode = "Vulkan"
    action = "updated" if args.update else "checked"
    print(
        f"\nreflect: {passed} {action}, {drifted} drifted, "
        f"{new_goldens} new, {compile_errors} compile errors, "
        f"{skipped} skipped, {warnings} warnings [{mode}]"
    )

    if all_diffs and not args.update:
        print("\n" + "=" * 72)
        for d in all_diffs:
            print(d)

    if violations:
        print(f"\n{len(violations)} invariant violation(s) — see CONTRACT_VIOLATION above.")
        return 1
    if compile_errors or drifted or new_goldens:
        return 1
    if args.strict_variants and warnings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
