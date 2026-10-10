# G2 single truth — Opus adversarial review (2026-10-10; ACCEPT-WITH-FIX: parity holds, restore the ~190 lost WHY comments, codemod must refuse ledger enum gates)

Condensed by the orchestrator. Reviewer ran the old `registry_parse.py` (from 353ae96d5) against both header versions plus `gate_gen --check`, smoke-matrices, smoke-env-inheritance, registered-gate-passthrough, `build_review_pack --selftest`, contract-coverage and the gate pytests (all green; `--rebuild` not run).

## Confirmed
- Parity: 354 entries before/after; `kFeatureTable` 56 rows identical in order/featureId/envVar/kind/defaultOn; `kAuxEnvVars` same 298-row set, now sorted by name (nothing reads its order); `RendererFeature` enum byte-identical; `EditorInspector.cpp:356` and `test_rendercore.cpp:330-405` index by enum → unaffected.
- Signal: `flag_registry_truth` and `registry_parse` invariants protected hand-written rows that no longer exist; header and yaml now come from one JSON and `check_all` diffs the whole header. 51 names that lived only in old prose left the header (stricter, not looser). `gate_default_agreement` still grades registry defaultOn vs raw readers; typed readers covered by `gate_gen --check` for Bool rows; 98 non-Bool registered rows had no default cross-check before either.
- yaml v3: nothing reads the dropped fields (`referenced_at`, `in_smoke_passthrough`, `documented_in_tier1_env_vars`) or the yaml at all in scripts/tools/hooks/MCP.
- Generator: pure function of `gate_table.json` + `feature_only_rows.json` + the hand-written header parts; no fence markers but `check_all` regenerates and diffs the whole file, so hand edits fail `--check`.
- `gate::Token`: `enum class Token : int32_t`; `if (tok)`, `!tok`, `== 1` do not compile; `is<"X","tok">` compile-time; tests assert type and indices.
- `reread`: `resolve()` returns before the CAS for reread rows (`gate.cpp:336`) → re-parse on every read; rows `MC2_ACTIVE_MOD`, `BRAIN_ENGAGE`, `BRAIN_PATROL`, `GLTF_AXIS`, `GLTF_YAW_DEG`; test `test_gate.cpp:509/521`.
- Shortened doc strings: only the inspector tooltip, `build_review_pack` title and env_index/gate_status read them; 346/350 docs verbatim in `gate-history.md`.

## Findings
- **F1 (med-low):** ~190 lines of load-bearing `//` WHY comments inside the old registry tables were deleted (SIM-IDENTITY "defaultOn=true is deliberate… Do not harmonize"; NOFOCUS-HARDEN "Do not tidy to true"; TERRAIN-AMBIENT-FLOOR-2; "MUST STAY LAST"). Fix: one-shot script over `git show 353ae96d5:RenderCore/RendererFeatureRegistry.h` attaching each block above its row in `Gates/gate_table.cpp` (or into gate-history).
- **F4 (premise false):** `FX_FORCE_SPAWN`, `SHADOW_DYNAMIC_PROP_CASTERS`, `SPOTLIGHT_DIRECTION_FIX`, `STATIC_PROP_BUILDING_SHADOW`, `_REBAKE` are Bool rows without tokens though the G0 ledger calls them enum (level values, e.g. 2 = trace); `tools/gate_codemod.py:1016` only notes "OWNER DECISION PENDING" → must hard-fail on ledger `enum`/`divergent`.
- **F3 (low):** `scripts/gate_table_reader.py` catches only `OSError` (not `UnicodeDecodeError`/`JSONDecodeError`); missing `feature_only_rows.json` silently drops ReverseZ (selftest reports 353); "inconsistent" check is count-only.
- **F6/F7:** `docs/gate-history.md` header still claims a generator mode that no longer exists and points at the seed; `giant-file-split-plan.md:37`, `gate-deletion-audit.md:11`, `terrain-gates-plan.md:187,214` still instruct `flag-registry-auto-gen.py` → `py -3 tools/gate_gen.py --write`.
- **F5/F8 (nits):** `gate.h:6` example uses a Bool row (`MC2_TONEMAP_V2`); generator should emit `// [i] EnumName` per kFeatureTable row and check it against the enum's trailing comment; `RendererFeatureRegistry.h:14` comment holds a quoted `"MC2_X"` that `pe_gate_scan --gates-from-registry` picks up; `flag_registry_drift` is a strict subset of `gate_gen_check` (retire).
- Outside G2: board red at HEAD — `gate_default_agreement` flags 4 COMMENT_DISAGREEMENTs from `docs/gate-codemod-g3.md:285-288` (`MC2_TERRAIN_MASK_DISPATCH_*` "default ON" prose vs reader OFF); the planner doc needs the quoted-history marker/`unset=` wording.
- Gap (pre-existing): 46 "default ON/OFF" phrases in `gate_table.cpp` docs are not graded (`Gates/` not in SCAN_DIRS).

Fixes routed to lane 1 (queued with G3).
