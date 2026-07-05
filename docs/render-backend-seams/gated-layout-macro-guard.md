# Gated layout-macro guard (GATED-LAYOUT-MACRO-GUARD-1)

CI tripwire: `scripts/check-gated-layout-macros.py`
Aggregated by: `scripts/check-contracts.sh` (label `gated_layout_macros`)
Report artifact: `docs/render-backend-seams/gated-layout-macros.md` (regenerated each run)
Allowlist: `scripts/check-gated-layout-macros.allowlist`

## The bug this generalizes

VULKAN-EDGE-FOG-ISLAND-2b (fix `9cce8e68`). A PRIVATE compile define
(`MC2_VULKAN_ISLAND`) added a non-static data member (`vkFogIsland_`) to
`gosPostProcess` under `#ifdef`, inside a header (`gos_postprocess.h`) included
by TUs belonging to MULTIPLE link targets. The define reached one target's TU
but not another's. The class **layout disagreed across translation units** —
`sizeof(gosPostProcess)` and member offsets differed — so accessors like
`getSceneFBO()` / `getWidth()` / `getHeight()` read the wrong offset and
returned garbage. Classic silent ODR violation: compiles clean, links clean,
misbehaves at runtime only.

The trap: `target_compile_definitions(gameos PUBLIC MC2_VULKAN_ISLAND)` looks
sufficient, but PUBLIC usage-requirements only propagate to targets that
`target_link_libraries(gameos)`. `gameos_main` does **not** link `gameos`, so
it never saw the define — until the fix added it explicitly to `gameos_main`
and `mc2`.

## The hard rule

> No compile define that is set PRIVATE on one target may affect a class LAYOUT
> (non-static data member, virtual method, base-class clause — anything that
> changes `sizeof`/vtable) in a header included by MULTIPLE targets, unless the
> define is propagated to EVERY consuming target (or the gated field is moved
> behind a pImpl / non-layout indirection).

## How to fix a violation

When the checker FLAGs a guard:

1. **Preferred — pImpl the gated field.** Replace the `#ifdef`-gated member with
   an always-present opaque pointer (`struct Impl; Impl* impl_;`) whose
   definition lives in the one gating TU. The header layout no longer depends on
   the macro, so no target can disagree.
2. **Propagate the define to every consuming target.** Add
   `target_compile_definitions(<target> PRIVATE <MACRO>)` for each target whose
   TUs include the header — including ones reached only via the link graph, not
   just direct source membership. Verify with the checker's per-guard
   `consuming` list.
3. **Acknowledged residual (last resort).** If the field is provably layout-safe
   for the uncovered TUs (e.g. option ships default-OFF so the member is absent
   everywhere in shipped builds, and the covered TUs are the only ones that
   access the affected offsets), add a `header:MACRO` line to
   `scripts/check-gated-layout-macros.allowlist` **with a justification comment**.
   Allowlisted guards print as `ACK` (never silently), and the residual risk must
   be documented in the allowlist entry.

## Current-tree status

`MC2_VULKAN_ISLAND` in `gos_postprocess.h` is on the allowlist: post-fix it is
PUBLIC on `gameos` + PRIVATE on `gameos_main` + PRIVATE on `mc2`. That covers
the TUs that construct/access `gosPostProcess` directly. It does NOT reach the
other linked consumers (`mclib`, `renderworld`, `gameadapters`, `gui_runtime`)
because they do not link `gameos`. This is accepted **only** because the option
ships default-OFF (member absent in all TUs → uniform layout). The documented
long-term fix is to pImpl `vkFogIsland_`. See the allowlist entry for the full
residual-risk note.

## Coverage (honest)

**Detects:** single- and multi-line `#ifdef`/`#if defined()` `MC2_*` guards that
wrap a data member, a `virtual` declaration, or a base clause directly inside a
`class`/`struct` body, in headers under `GameOS/`, `RenderCore/`, `mclib/`,
`code/`; and partial/PRIVATE-only macro coverage across the consuming targets
(target membership from direct source lists + a small explicit link map).

**Misses (by design, to stay low-false-positive):**
- Layout changes via macro **expansion** rather than `#ifdef` (a member whose
  *type* is a macro that differs per target).
- Conditional `#pragma pack`, union reordering, or template sizes that depend on
  a macro.
- Headers outside the scanned roots, or pulled in transitively from `3rdparty/`.
- Target membership reached purely through an untracked interface-include chain
  may be under-counted (the coarse dir→target map + link map are explicit, not
  derived from a configured CMake build).

A PASS means "no guard matched the detectable layout-hazard pattern," not a
proof of ODR safety. Treat it as a tripwire for the common case, not a theorem.

## Running

```bash
py -3 scripts/check-gated-layout-macros.py            # human report, exit nonzero on violation
py -3 scripts/check-gated-layout-macros.py --quiet    # for the aggregate gate
py -3 scripts/check-gated-layout-macros.py --negative-test   # self-test: proves it catches the pre-fix EdgeFog bug
```
