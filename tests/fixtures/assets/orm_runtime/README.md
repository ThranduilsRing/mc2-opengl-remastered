<!-- doc-status: stale | reviewed:2026-10-11 -->
# ORM runtime manifest fixture

`manifest.json` is a schema-valid asset manifest for the MC2 static-prop ORM
(runtime) feature. It exercises the slot-aware texture-ref path: an `albedo`
slot (`colorSpace: srgb`) and an `orm` slot (`colorSpace: linear`).

It has no `normal` slot, so `capabilities.hasTangents` is `false`
(the validator's normal->tangent cross-check only fires for a `normal` slot).

## Validate

```
py -3 tools/validate_asset_manifest.py tests/fixtures/assets/orm_runtime/manifest.json
```

Exit 0 = valid.

## Regenerating the cooked ORM texture (real CLI)

`batch_cook.py` is **directory-based** (`--src`/`--dst` are directories, not
files). To cook the ORM sidecar(s) to linear BC7 KTX2:

```
py -3 tools/mc2texcook/batch_cook.py --src <src_dir> --dst <dst_dir> --preset orm --bc7
```

Notes:
- `--preset orm` selects the linear path; with `--bc7` this cooks
  `R8G8B8A8_UNORM` -> `BC7_UNORM` (never sRGB) per the slot-aware cook.
- `--ext` selects the source extension to match (default `.tga`); add
  `--recursive` to walk subdirectories, `--skip-existing` to skip cooked outputs.
- `--bc7` requires the KTX-Software `ktx` CLI (see `--ktx-tool`); without it the
  uncompressed RGBA8 path is used instead.
