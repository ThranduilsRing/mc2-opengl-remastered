<!-- doc-status: current | reviewed:2026-10-11 -->
# RenderWorld Arc — Status Ledger

**Last update:** 2026-05-24
**HEAD when ledger written:** `35d9e70` (CXX17-3 docs) → gauntlet GREEN
**Sibling doc:** [migration guide](renderworld_migration_guide.md) — contributor onboarding

This is the **status ledger** for the RenderWorld arc. It reframes M3 /
M4 / M5 from "ordinary next implementation slices" to **DECISIONS**, and
records the arc's steady-state shape after the CXX17 stabilization pass.

---

## Slice ledger (10 SHIPPED + 1 standalone-infra)

| Slice | SHA | Shape | Status |
|-------|-----|-------|--------|
| M1 | `842f34f` | Static-prop adapter (5 audited call sites) | SHIPPED |
| M1.5 | `842f34f` | ObjectID buffer substrate (R32_UINT attachment-2) | SHIPPED |
| M1.6 | `db25d67` | Static-prop pickup (Shift+click) | SHIPPED |
| M2-pre | `16e3d53` | `tryGameplayPick` spine extraction (META-FIX) | SHIPPED |
| M2 | `7dc0a6e` | MechRenderAdapter (route-only) | SHIPPED |
| M2.5 | `8f8be64` | Mech ObjectID substrate | SHIPPED |
| M2.6 | `5d413d6` | Mech pickup integration + META-FIX schema rename | SHIPPED |
| M6 | `16a461b` | Firewall audit script (no raw GL from game side) | SHIPPED |
| M3 | `45a7d4b` | Terrain reservation + tripwire | **DECISION: deferred indefinitely** |
| M4 | `d601139` | VFX prohibition + scaffold + CI grep gate | **DECISION: never write attachment-2** |
| M5 | `45a7d4b` (folded into M3) | Overlay enum-comment deferral | **DECISION: deferred indefinitely** |
| CXX17 | `5c03835` (+ `35d9e70` rules) | C++17 standard flip | SHIPPED (infra) |

---

## The four DECISIONS (M3 / M4 / M5 + the firewall lockdown)

### M3 — Terrain

**DECISION: GPU terrain identity is deferred indefinitely. CPU terrain pick remains canonical.**

- `RenderObjectKind::Terrain = 2` enum value RESERVED
- `kTerrainHandleBase = 0x40000` constant RESERVED (no writer)
- Defensive tripwire in `lookupAtPixel`: warns if `kind=Terrain` ever returned
- `Terrain::IsGameSelectTerrainPosition + doMove(wPos)` ground-click path UNCHANGED (canonical)
- `Terrain::worldToTile` returns tile R/C + type + elevation directly; GPU substrate would add no new info
- **If/when an editor consumer emerges** wanting per-quad GPU identity (per-pixel inspect / brush preview / etc.), M3.1 ships then. Use `subKind = Base / Water / Decal / Mine` payload — NOT separate `RenderObjectKind` values for terrain variants.
- Spec: [docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md](superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md)

### M4 — VFX

**DECISION: VFX must NEVER write object IDs. Click-through by design. CI-grep protects attachment-2.**

- `RenderObjectKind::Vfx = 3` enum value RESERVED
- `kVfxHandleBase = 0x80000` constant RESERVED (no writer)
- `scripts/check-vfx-no-objectid.sh` PROHIBITS `layout(location=2) out` in VFX shader basename allowlist
- `scripts/check-vfx-no-objectid.allowlist` expected to remain EMPTY forever
- **Rationale (load-bearing):** GL spec §17.3.6 — integer color attachments don't blend (last-write-wins per fragment). Translucent/additive particle fragments writing the objectID buffer would clobber the mech/static-prop ID underneath even when visually transparent. This breaks M2.6 mech-pick through muzzle flashes, smoke, tracers, impacts. The correct behavior is "VFX is click-through; pick the object underneath."
- **Source-game-object lookup** ("which mech fired this explosion?") stays in game logic — source known at fire-event time; GPU should not rediscover via pixel readback.
- **gosFX dev-override caveat:** `MC2_DISABLE_GOSFX=0` remains broken under unified-projection F1 (per CLAUDE.md known issues). Future VFX work must NOT use that path as a substrate proof.
- Spec: [docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md](superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md)

### M5 — Overlay

**DECISION: Deferred indefinitely. No identity consumer exists; enum slot un-reserved.**

- The `RenderObjectKind` enum block comment now reads: `// Overlay reserved/deferred (see M5 spec for clarification rationale)`
- "Overlay" was overloaded across 7 distinct in-tree meanings (terrain splats, decals, semantic map-tile classifier, MC_OverlayType atlas, env-var gate, AI weight-class metadata, debug viz). None of them have an identity-needing consumer.
- **If/when a use case emerges**, ship as a NEW NAMED SLICE — NOT as "M5 Overlay." Candidates:
  - `HoverKindIndicator` (consumer of M2.6 / M3; cursor-hover kind label)
  - `RenderWorldDebugOverlay` (debug visualization of pick results / heatmap)
  - `M5-perf: overlay/decal GPU port` (perf migration, not a RenderWorld identity slice — separate from the arc)
- Spec: [docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md](superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md)

### M6 — Firewall lockdown (positive decision; SHIPPED)

**DECISION: Discipline enforced by script, not memory.**

- `36544d925:scripts/check-no-raw-gl-from-game.sh` — `code/` and `mclib/` MUST NOT call raw `gl*()` functions (allowlist: `mclib/render_contract.cpp` for diagnostic state queries gated by `MC2_RENDER_CONTRACT_ASSERT=1`).
- Recon proved the hypothesis was already empirically true at HEAD; M6 codifies the clean state into CI.
- Includes a small known false-positive (trailing-comment scan bug — chip filed for fix).

---

## RenderObjectKind enum — final shape

```cpp
enum class RenderObjectKind : uint16_t {
    StaticProp = 0,    // SHIPPED (M1)
    Mech       = 1,    // SHIPPED (M2)
    Terrain    = 2,    // RESERVED (M3) — no writer; tripwire-protected
    Vfx        = 3,    // RESERVED (M4) — PROHIBITED writers per CI grep
    // Overlay reserved/deferred (see M5 spec for clarification rationale)
};
```

## Handle-range partitioning (20-bit index mask = `0xFFFFF`)

| Kind | Handle base | Slot count | Notes |
|---|---|---|---|
| StaticProp | `0..0xFFFF` | 64k | Max observed mc2_24 = 2641 |
| Mech | `0x10000..0x3FFFF` | 192k | Max observed mc2_24 = 46 |
| Terrain | `0x40000..0x7FFFF` | 262k | Reserved (M3); no writer in v1 |
| Vfx | `0x80000..0xBFFFF` | 262k | Reserved (M4); CI-prohibited writers |
| (future) | `0xC0000..0xFFFFE` | 262k-1 | Free for future kinds |
| Sentinel | `0xFFFFF` | 1 | Reserved bug-bait; never allocate |

**Invariant:** bases ≥ `0x100000` SILENTLY TRUNCATE under the 20-bit mask. Do NOT propose any new base outside the partition.

---

## CI / enforcement layer

Three scripts enforce the contract mechanically:

| Script | What it enforces | Allowlist |
|---|---|---|
| `scripts/check-include-firewall.sh` | SCOPE_DIRS layering (engine TUs don't pull game-side headers) | `scripts/check-include-firewall.allowlist` (one entry: `GameAdapters/MechRenderAdapter.cpp`) |
| `36544d925:scripts/check-no-raw-gl-from-game.sh` | Game-side TUs (`code/`, `mclib/`) don't call raw `gl*()` | `scripts/check-no-raw-gl-from-game.allowlist` (one entry: `mclib/render_contract.cpp`) |
| `scripts/check-vfx-no-objectid.sh` | VFX shaders don't write attachment-2 | `scripts/check-vfx-no-objectid.allowlist` (expected EMPTY forever) |

Runtime tripwires (defense-in-depth):
- `lookupAtPixel` warns if `kind=Terrain` returned (M3 contract — no writer should produce it)

Self-tests (per-mission, env-gated):
- `[OBJECT_ID_SELFTEST v1]` — M1.5 static-prop substrate canary
- `[MECH_OBJECT_ID_SELFTEST v1]` — M2.5 mech substrate canary
- `[MECH_PICK_SELFTEST v1]` — M2.6 mech pickup canary
- `[GAMEPLAY_PICK v1]` — unified banner showing per-kind enabled/debug/pierce-fog state

---

## Validation gauntlet (last passed 2026-05-24)

7-step gauntlet passed GREEN on 2026-05-24 after CXX17 flip:

1. Full clean RelWithDebInfo build — PASS
2. Tier1 5/5 env-OFF — PASS (avg 140-142 fps)
3. shader_reflect gates — SKIP (Vulkan SDK absent on dev machine; CI environment should run)
4. Material manifest validator — PASS (`scripts/check-material-gpu-mirror.sh: OK`)
5. Firewall + no-raw-GL + VFX scripts — PASS (one pre-existing false-positive chip filed)
6. `[VISIBILITY v1]` log sanity — PASS
7. Object-ID env-ON canaries (M1.5 / M2.5 / M2.6) — PASS 5/5 each; terrain tripwire ZERO hits; `mlr_mech_draws=0` across all 5 missions (Q6 hypothesis holds)

Full report: [docs/superpowers/explorations/2026-05-24-cxx17-stabilization-gauntlet.md](superpowers/explorations/2026-05-24-cxx17-stabilization-gauntlet.md)

---

## What's NOT an "upcoming RenderWorld slice"

Future planners should NOT add these to a "next sprint" backlog:

- "Ship M3 terrain writers." → No. M3 is a DECISION to defer; ship only if an editor consumer materializes.
- "Ship M4 VFX writers." → No, ever. M4 is a DECISION to PROHIBIT; the CI script enforces it.
- "Ship M5 Overlay." → No. M5 is a DECISION to defer indefinitely; if a use case appears, ship a NEW NAMED slice.
- "Migrate raw GL from game side." → No. M6 proved game-side is already clean; the script LOCKS that state.

Future RenderWorld arc work — if any — is one of:

- **M3.1** triggered by an editor consumer needing per-quad GPU identity
- **M2.7** mech-select-on-click (M2.6 was inspect-only; promote to gameplay mutation if desired)
- **A new named slice** (HoverKindIndicator / RenderWorldDebugOverlay) that consumes existing substrate without adding a new kind
- **A perf slice** (`M5-perf: overlay/decal GPU port`) — orthogonal to identity arc

Anything else is a new project. The arc reached steady state on 2026-05-24.

---

## Cross-references

- [Migration guide](renderworld_migration_guide.md) — contributor onboarding (646 lines)
- [CXX17 coding rules](cxx17-coding-rules.md) — language feature usage
- [Gauntlet report (this stabilization)](superpowers/explorations/2026-05-24-cxx17-stabilization-gauntlet.md)
- CLAUDE.md "Active campaigns" — slice-level summaries
- CLAUDE.md "Critical inline rules" — emoji ban, build config, GL conventions, etc.
