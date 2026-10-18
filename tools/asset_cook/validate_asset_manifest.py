#!/usr/bin/env python3
"""tools/asset_cook/validate_asset_manifest.py -- Single canonical asset manifest validator.

RULING C1 (asset-cook-pipeline-architecture.md §10 + §12.1):
  This file is the SINGLE canonical validator. The scaffold copy
  (tools/validate_asset_manifest.py) has been deleted; all rules it enforced
  are preserved here under the AUTHORING FORMAT path.

Supports TWO manifest formats, detected by presence/absence of
  "schema": "mc2-asset-manifest-v1"

FORMAT A -- COOK MANIFEST (mc2-asset-manifest-v1):
  Validates cooked static-prop manifests against asset_manifest.schema.json
  (requires jsonschema) plus cross-field coherence the schema cannot express:
    A1. lowercase asset.class / appearanceName / replaces
    A2. replaces == '<class>:<appearanceName>'
    A3. geometry.bounds min <= max per axis
    A4. geometry.lods strictly ascending, lod >= 1
    A5. materials[i].slot == i (no gaps, no reorder)
    A6. alphaClass <-> flags.alpha_test <-> 'a_' prefix agreement
    A7. capabilities.alphaTest == any material alphaClass==1
    A8. capabilities.hasLodChain == (geometry.lods non-empty)
    A9. deps.stockFallback == asset.appearanceName (if present)

FORMAT B -- AUTHORING MANIFEST (scaffold / ASSET-MANIFEST-SCHEMA-SCAFFOLD-1):
  Validates authoring-facing manifests (assetId/source/kind/materials/
  capabilities/lods/textureRefs). No jsonschema dep for this path.
  Ported from the deleted scaffold (tools/validate_asset_manifest.py).

  MATERIAL-AUTHORING-VALIDATION-1 rules (ported from scaffold):
    B1. materials[i]: name (string) + shader (string) required
    B2. materials[i].alphaMode in {opaque, alphaTest, blend}
    B3. alphaTestThreshold only valid when alphaMode='alphaTest'
    B4. alphaTestThreshold in [0, 1]
    B5. doubleSided must be a boolean
    B6. pbr.baseColorFactor: [0,1] scalar or 3-4 element [0,1] array
    B7. pbr.metallicFactor / roughnessFactor in [0, 1]
    B8. textureRefs[i]: slot in known vocabulary + correct colorSpace
        (albedo/emissive = srgb; normal/orm/mask = linear)
    B9. textureRefs slot 'normal' requires capabilities.hasTangents = true
  ASSET-MANIFEST-SCHEMA-SCAFFOLD-1 / ASSET-MANIFEST-0-EXTEND rules (ported):
    B10. Required fields: assetId, source, kind (or type), materials, lods,
         textureRefs, capabilities (object with all six booleans)
    B11. capabilities keys must be exactly the six required booleans
    B12. geometry (optional): meshCount/vertexCount/indexCount/materialSlotCount
         ints; hasNormals/hasTangents bools; bounds {min[3],max[3],radius}
    B13. lods[] entries (optional dict shape): level/vertexCount/triangleCount
         ints, error float
    B14. provenance (optional): tool/toolVersion/generatedAt/sourceHash strings
    B15. generatedOutputs (optional): array of {path:string, kind:string}

Usage:
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json>
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json> --check-files
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json> --deploy-root <dir>
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json> --quiet
  py -3 tools/asset_cook/validate_asset_manifest.py <manifest.json> --expect-fail

Exit 0 = all checks pass. Exit 1 = one or more errors. Exit 2 = usage/tool error.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Schema path for FORMAT A (cook manifests)
# ---------------------------------------------------------------------------
SCHEMA_PATH = Path(__file__).resolve().parent / "asset_manifest.schema.json"

# ---------------------------------------------------------------------------
# FORMAT B constants (authoring manifests)
# ---------------------------------------------------------------------------
_B_REQUIRED_CAPABILITIES = (
    "hasNormals",
    "hasTangents",
    "hasLods",
    "hasImpostor",
    "castsShadow",
    "supportsObjectId",
)

# Texture slot vocabulary and required colorSpace.
# Reconciled with tools/mc2texcook/mc2texcook.py presets and
# RenderCore/MaterialGpu.h. (Rule B8)
_B_SLOT_COLORSPACE: dict[str, str] = {
    "albedo": "srgb",
    "emissive": "srgb",
    "normal": "linear",
    "orm": "linear",
    "mask": "linear",
}

_B_ALPHA_MODES = ("opaque", "alphaTest", "blend")


# ---------------------------------------------------------------------------
# FORMAT A: Cook-manifest coherence checks
# ---------------------------------------------------------------------------

def _is_lower(s: str) -> bool:
    return s == s.lower()


def _cook_coherence_errors(m: dict) -> list[str]:
    """Cross-field checks the JSON schema cannot express (FORMAT A)."""
    errs: list[str] = []

    asset = m.get("asset", {})
    cls = asset.get("class", "")
    appn = asset.get("appearanceName", "")
    replaces = asset.get("replaces", "")

    # A1. lowercase identity
    for label, val in (("class", cls), ("appearanceName", appn), ("replaces", replaces)):
        if val and not _is_lower(val):
            errs.append(
                f"asset.{label} must be lowercase (got {val!r}); "
                "registry normalizeKey lowercases, manifest must emit lowercase"
            )

    # A2. replaces composite
    expected = f"{cls}:{appn}"
    if replaces != expected:
        errs.append(
            f"asset.replaces {replaces!r} != '<class>:<appearanceName>' ({expected!r})"
        )

    geom = m.get("geometry", {})

    # A3. bounds ordering
    b = geom.get("bounds", {})
    bmin, bmax = b.get("min", []), b.get("max", [])
    if len(bmin) == 3 and len(bmax) == 3:
        for k, axis in enumerate("xyz"):
            if bmin[k] > bmax[k]:
                errs.append(
                    f"geometry.bounds.min.{axis} ({bmin[k]}) > max.{axis} ({bmax[k]})"
                )

    # A4. LOD chain strictly ascending, lod >= 1
    lods = geom.get("lods", []) or []
    prev = 0
    for i, lod in enumerate(lods):
        n = lod.get("lod", 0)
        if n <= prev:
            errs.append(
                f"geometry.lods[{i}].lod ({n}) not strictly ascending (prev {prev}); "
                "LOD0==cooked"
            )
        prev = n

    # A5. material slot == index + A6. alphaClass coherence
    mats = m.get("materials", [])
    any_alpha = False
    for i, mat in enumerate(mats):
        slot = mat.get("slot")
        if slot != i:
            errs.append(
                f"materials[{i}].slot ({slot}) != index {i} (no gaps/reorder)"
            )
        ac = mat.get("alphaClass", 0)
        at = bool(mat.get("flags", {}).get("alpha_test", False))
        tn = mat.get("textureName", "")
        has_a = tn.startswith("a_")
        if ac == 1:
            any_alpha = True
        if (ac == 1) != at:
            errs.append(
                f"materials[{i}]: alphaClass={ac} but flags.alpha_test={at} "
                "(must agree)"
            )
        if (ac == 1) != has_a:
            errs.append(
                f"materials[{i}]: alphaClass={ac} but textureName {tn!r} "
                f"{'has' if has_a else 'lacks'} 'a_' prefix "
                "(resolver convention: a_ iff alpha)"
            )

    # A7. capabilities.alphaTest coherence
    caps = m.get("capabilities", {})
    if "alphaTest" in caps and caps["alphaTest"] != any_alpha:
        errs.append(
            f"capabilities.alphaTest ({caps['alphaTest']}) != "
            f"any material alphaClass==1 ({any_alpha})"
        )

    # A8. capabilities.hasLodChain coherence
    if "hasLodChain" in caps and caps["hasLodChain"] != (len(lods) > 0):
        errs.append(
            f"capabilities.hasLodChain ({caps['hasLodChain']}) != "
            f"(geometry.lods non-empty: {len(lods) > 0})"
        )

    # A9. deps.stockFallback
    deps = m.get("deps", {})
    sf = deps.get("stockFallback")
    if sf is not None and sf != appn:
        errs.append(
            f"deps.stockFallback ({sf!r}) != asset.appearanceName ({appn!r})"
        )

    return errs


def _cook_file_errors(
    m: dict, manifest_path: Path, deploy_root: Path | None
) -> list[str]:
    """Optional: referenced files exist (FORMAT A)."""
    errs: list[str] = []
    mdir = manifest_path.resolve().parent
    root = deploy_root.resolve() if deploy_root else mdir

    geom = m.get("geometry", {})
    cooked = geom.get("cooked")
    if cooked and not (mdir / cooked).exists():
        errs.append(f"geometry.cooked not found: {mdir / cooked}")
    src = geom.get("source")
    if src and not (root / src).exists():
        errs.append(f"geometry.source not found: {root / src}")

    for i, mat in enumerate(m.get("materials", [])):
        for tier, p in (mat.get("albedo_ktx2") or {}).items():
            if not (root / p).exists():
                errs.append(
                    f"materials[{i}].albedo_ktx2.{tier} not found: {root / p}"
                )
    return errs


def validate_cook_manifest(
    m: dict, manifest_path: Path, check_files: bool, deploy_root: Path | None
) -> list[str]:
    """Validate a FORMAT A cook manifest. Returns list of error strings."""
    try:
        import jsonschema
    except ImportError:
        return [
            "jsonschema not installed "
            "(py -3 -m pip install jsonschema) -- required for cook-manifest validation"
        ]

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except OSError as e:
        return [f"cannot read schema {SCHEMA_PATH}: {e}"]

    errors: list[str] = []
    try:
        jsonschema.validate(m, schema)
    except jsonschema.ValidationError as e:
        loc = "/".join(str(x) for x in e.absolute_path) or "<root>"
        errors.append(f"schema: {loc}: {e.message}")

    if isinstance(m, dict):
        errors.extend(_cook_coherence_errors(m))
        if check_files:
            errors.extend(_cook_file_errors(m, manifest_path, deploy_root))

    return errors


# ---------------------------------------------------------------------------
# FORMAT B: Authoring-manifest validation (ported from scaffold)
# ---------------------------------------------------------------------------

def _is_number(v) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _in_unit(v) -> bool:
    return _is_number(v) and 0.0 <= float(v) <= 1.0


def _b_validate_geometry(g, errs: list) -> None:
    """ASSIMP-IMPORTER-PHASE-0 output contract shape check (Rule B12)."""
    if not isinstance(g, dict):
        errs.append("'geometry' must be an object")
        return
    for key in ("meshCount", "vertexCount", "indexCount", "materialSlotCount"):
        if key in g and not (isinstance(g[key], int) and not isinstance(g[key], bool)):
            errs.append(f"geometry.{key} must be an integer")
    for key in ("hasNormals", "hasTangents"):
        if key in g and not isinstance(g[key], bool):
            errs.append(f"geometry.{key} must be a boolean")
    if "bounds" in g:
        b = g["bounds"]
        if not isinstance(b, dict):
            errs.append("geometry.bounds must be an object")
        else:
            for axis in ("min", "max"):
                if axis in b:
                    v = b[axis]
                    if not (
                        isinstance(v, list)
                        and len(v) == 3
                        and all(_is_number(x) for x in v)
                    ):
                        errs.append(f"geometry.bounds.{axis} must be a 3-number array")
            if "radius" in b and not _is_number(b["radius"]):
                errs.append("geometry.bounds.radius must be a number")


def _b_validate_lods(lods, errs: list) -> None:
    """lods[] entries: plain or LOD-stat objects (Rule B13)."""
    for i, lod in enumerate(lods):
        if not isinstance(lod, dict):
            continue
        for key in ("level", "vertexCount", "triangleCount"):
            if key in lod and not (
                isinstance(lod[key], int) and not isinstance(lod[key], bool)
            ):
                errs.append(f"lods[{i}].{key} must be an integer")
        if "error" in lod and not _is_number(lod["error"]):
            errs.append(f"lods[{i}].error must be a number")


def _b_validate_texture_refs(refs, errs: list) -> set:
    """Require slot + path; validate colorSpace convention (Rules B8, B14).

    Returns the set of slots seen (used for the normal->tangent cross-check B9).
    """
    slots_seen: set[str] = set()
    for i, ref in enumerate(refs):
        if not isinstance(ref, dict):
            errs.append(f"textureRefs[{i}] must be an object")
            continue
        slot = ref.get("slot")
        if not isinstance(slot, str) or not slot:
            errs.append(
                f"textureRefs[{i}] missing/empty required string 'slot'"
            )
        else:
            slots_seen.add(slot)
            if slot not in _B_SLOT_COLORSPACE:
                errs.append(
                    f"textureRefs[{i}].slot '{slot}' unknown "
                    f"(expected one of {sorted(_B_SLOT_COLORSPACE)})"
                )
        if not isinstance(ref.get("path"), str) or not ref.get("path"):
            errs.append(
                f"textureRefs[{i}] missing/empty required string 'path'"
            )

        # B8. colorSpace must match slot convention.
        if "colorSpace" in ref:
            cs = ref["colorSpace"]
            if cs not in ("srgb", "linear"):
                errs.append(
                    f"textureRefs[{i}].colorSpace must be 'srgb' or 'linear'"
                )
            elif isinstance(slot, str) and slot in _B_SLOT_COLORSPACE and cs != _B_SLOT_COLORSPACE[slot]:
                errs.append(
                    f"textureRefs[{i}] slot '{slot}' requires colorSpace "
                    f"'{_B_SLOT_COLORSPACE[slot]}', got '{cs}'"
                )

        # Optional cooked-asset metadata shape.
        if "vkFormat" in ref and not (
            isinstance(ref["vkFormat"], int) and not isinstance(ref["vkFormat"], bool)
        ):
            errs.append(f"textureRefs[{i}].vkFormat must be an integer")
        if "mips" in ref and not (
            isinstance(ref["mips"], int) and not isinstance(ref["mips"], bool)
        ):
            errs.append(f"textureRefs[{i}].mips must be an integer")
        if "dims" in ref:
            d = ref["dims"]
            if not (
                isinstance(d, list)
                and len(d) == 2
                and all(isinstance(x, int) and not isinstance(x, bool) for x in d)
            ):
                errs.append(f"textureRefs[{i}].dims must be a 2-integer array")
        if "format" in ref and not isinstance(ref["format"], str):
            errs.append(f"textureRefs[{i}].format must be a string")
    return slots_seen


def _b_validate_materials(materials, errs: list) -> None:
    """MATERIAL-AUTHORING-VALIDATION-1: authoring metadata (Rules B1-B7)."""
    for i, mat in enumerate(materials):
        if not isinstance(mat, dict):
            errs.append(f"materials[{i}] must be an object")
            continue
        # B1. name + shader required
        if not isinstance(mat.get("name"), str) or not mat.get("name"):
            errs.append(f"materials[{i}] missing/empty required string 'name'")
        if not isinstance(mat.get("shader"), str) or not mat.get("shader"):
            errs.append(f"materials[{i}] missing/empty required string 'shader'")

        # B2. alphaMode vocabulary
        if "alphaMode" in mat:
            am = mat["alphaMode"]
            if am not in _B_ALPHA_MODES:
                errs.append(
                    f"materials[{i}].alphaMode must be one of {list(_B_ALPHA_MODES)}"
                )
            # B3. alphaTestThreshold only valid with alphaMode='alphaTest'
            if "alphaTestThreshold" in mat and am != "alphaTest":
                errs.append(
                    f"materials[{i}].alphaTestThreshold is only valid "
                    "when alphaMode='alphaTest'"
                )
        # B4. alphaTestThreshold in [0,1]
        if "alphaTestThreshold" in mat and not _in_unit(mat["alphaTestThreshold"]):
            errs.append(
                f"materials[{i}].alphaTestThreshold must be a number in [0,1]"
            )
        # B5. doubleSided bool
        if "doubleSided" in mat and not isinstance(mat["doubleSided"], bool):
            errs.append(f"materials[{i}].doubleSided must be a boolean")

        # B6 + B7. pbr factors
        if "pbr" in mat:
            pbr = mat["pbr"]
            if not isinstance(pbr, dict):
                errs.append(f"materials[{i}].pbr must be an object")
            else:
                bcf = pbr.get("baseColorFactor")
                if bcf is not None:
                    ok = _in_unit(bcf) or (
                        isinstance(bcf, list)
                        and len(bcf) in (3, 4)
                        and all(_in_unit(x) for x in bcf)
                    )
                    if not ok:
                        errs.append(
                            f"materials[{i}].pbr.baseColorFactor must be a [0,1] number "
                            "or a 3-4 element [0,1] array"
                        )
                for key in ("metallicFactor", "roughnessFactor"):
                    if key in pbr and not _in_unit(pbr[key]):
                        errs.append(
                            f"materials[{i}].pbr.{key} must be a number in [0,1]"
                        )


def _b_validate_provenance(p, errs: list) -> None:
    """Rule B14: provenance fields must be strings when present."""
    if not isinstance(p, dict):
        errs.append("'provenance' must be an object")
        return
    for key in ("tool", "toolVersion", "generatedAt", "sourceHash"):
        if key in p and not isinstance(p[key], str):
            errs.append(f"provenance.{key} must be a string")


def _b_validate_generated_outputs(outs, errs: list) -> None:
    """Rule B15: generatedOutputs entries need path + kind strings."""
    if not isinstance(outs, list):
        errs.append("'generatedOutputs' must be an array")
        return
    for i, o in enumerate(outs):
        if not isinstance(o, dict):
            errs.append(f"generatedOutputs[{i}] must be an object")
            continue
        for key in ("path", "kind"):
            if not isinstance(o.get(key), str) or not o.get(key):
                errs.append(
                    f"generatedOutputs[{i}] missing/empty required string '{key}'"
                )


def validate_authoring_manifest(m: dict) -> list[str]:
    """Validate a FORMAT B authoring manifest. Returns list of error strings."""
    errs: list[str] = []

    def req_str(key: str) -> None:
        v = m.get(key)
        if not isinstance(v, str) or not v:
            errs.append(f"missing/empty required string field '{key}'")

    # B10. Required fields
    req_str("assetId")
    req_str("source")

    kind = m.get("kind", m.get("type"))
    if not isinstance(kind, str) or not kind:
        errs.append("missing/empty required field 'kind' (or 'type')")

    for key in ("materials", "lods", "textureRefs"):
        if key not in m:
            errs.append(
                f"missing required field '{key}' "
                "(use an explicit empty array if none)"
            )
        elif not isinstance(m[key], list):
            errs.append(f"'{key}' must be an array")

    # B11. capabilities object + all six booleans
    caps = m.get("capabilities")
    if not isinstance(caps, dict):
        errs.append("missing/invalid required field 'capabilities' (object)")
    else:
        for cap in _B_REQUIRED_CAPABILITIES:
            if cap not in caps:
                errs.append(f"capabilities.{cap} is required")
            elif not isinstance(caps[cap], bool):
                errs.append(f"capabilities.{cap} must be a boolean")
        extra = set(caps) - set(_B_REQUIRED_CAPABILITIES)
        if extra:
            errs.append(f"capabilities has unknown keys: {sorted(extra)}")

    # Validate optional + array sections
    if isinstance(m.get("materials"), list):
        _b_validate_materials(m["materials"], errs)
    if isinstance(m.get("lods"), list):
        _b_validate_lods(m["lods"], errs)

    slots_seen: set[str] = set()
    if isinstance(m.get("textureRefs"), list):
        slots_seen = _b_validate_texture_refs(m["textureRefs"], errs)

    if "geometry" in m:
        _b_validate_geometry(m["geometry"], errs)
    if "provenance" in m:
        _b_validate_provenance(m["provenance"], errs)
    if "generatedOutputs" in m:
        _b_validate_generated_outputs(m["generatedOutputs"], errs)

    # B9. Normal map requires tangents.
    if "normal" in slots_seen and isinstance(caps, dict):
        if caps.get("hasTangents") is not True:
            errs.append(
                "textureRefs has a 'normal' slot but capabilities.hasTangents is not true "
                "(normal mapping requires tangents)"
            )

    return errs


# ---------------------------------------------------------------------------
# Dispatch: detect format and validate
# ---------------------------------------------------------------------------

def _detect_format(m: dict) -> str:
    """Return 'cook' for mc2-asset-manifest-v1, 'authoring' otherwise."""
    if isinstance(m.get("schema"), str) and m["schema"] == "mc2-asset-manifest-v1":
        return "cook"
    return "authoring"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Validate an asset manifest JSON. "
            "Supports cook manifests (mc2-asset-manifest-v1) and "
            "authoring manifests (scaffold format)."
        )
    )
    ap.add_argument("manifest", type=Path, help="Path to the manifest JSON.")
    ap.add_argument(
        "--check-files",
        action="store_true",
        help="Verify referenced glb/ktx2 paths exist (cook manifests only).",
    )
    ap.add_argument(
        "--deploy-root",
        type=Path,
        default=None,
        help="Root for source/texture relative paths (cook manifests only).",
    )
    ap.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress PASS output.",
    )
    ap.add_argument(
        "--expect-fail",
        action="store_true",
        help="Invert: exit 0 only if the manifest is INVALID (for negative fixtures).",
    )
    args = ap.parse_args()

    path: Path = args.manifest
    if not path.exists():
        print(f"ERROR manifest not found: {path}", file=sys.stderr)
        return 2

    try:
        m = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        msg = f"FAIL {path.name}: JSON parse failed: {e}"
        print(msg, flush=True)
        return 0 if args.expect_fail else 1

    if not isinstance(m, dict):
        msg = f"FAIL {path.name}: top-level must be an object"
        print(msg, flush=True)
        return 0 if args.expect_fail else 1

    fmt = _detect_format(m)
    if fmt == "cook":
        errors = validate_cook_manifest(m, path, args.check_files, args.deploy_root)
    else:
        errors = validate_authoring_manifest(m)

    if args.expect_fail:
        if errors:
            if not args.quiet:
                print(
                    f"XFAIL {path.name}: invalid as expected "
                    f"({len(errors)} error(s))",
                    flush=True,
                )
            return 0
        print(
            f"FAIL {path.name}: expected invalid but it validated",
            file=sys.stderr,
            flush=True,
        )
        return 1

    if errors:
        print(f"FAIL {path.name} ({len(errors)} error(s)):", file=sys.stderr, flush=True)
        for e in errors:
            print(f"  - {e}", file=sys.stderr, flush=True)
        return 1

    if not args.quiet:
        if fmt == "cook":
            asset = m.get("asset", {})
            print(
                f"PASS {path.name} format=cook "
                f"id={asset.get('id')} replaces={asset.get('replaces')} "
                f"materials={len(m.get('materials', []))}",
                flush=True,
            )
        else:
            print(
                f"PASS {path.name} format=authoring "
                f"assetId={m.get('assetId')} "
                f"kind={m.get('kind', m.get('type'))} "
                f"materials={len(m.get('materials', []))} "
                f"lods={len(m.get('lods', []))} "
                f"textureRefs={len(m.get('textureRefs', []))}",
                flush=True,
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
