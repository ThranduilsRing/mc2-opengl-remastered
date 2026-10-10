<!-- doc-status: current | reviewed:2026-10-11 -->
# Brain & AI 2.14 — Implementation Roadmap

We own this lane. The modder (carver_v_enhanced author) had the idea + a working ABL/FIT reference; we're the ones building it in the engine. This doc replaces the "handoff for someone else" — it's our plan to drive against.

Discipline that's worked: each slice is a default-OFF gate, gate-OFF byte-identical, fingerprint-confirmed validation, hands-on review for any behavior change (caught two agent-missed correctness bugs this way), arc memory keeps the resume pointer, integration follows the merge-verification rules.

## What's shipped (nifty)

| Slice | Gate | Effect |
|---|---|---|
| ABL native-binding guards | `MC2_ABL_ARG_GUARD` / `_RUNTIME_SOFTFAIL` | Soft-fail on null script args (full recon-flagged coverage + commander chain) |
| ABL repro hook | `MC2_ABL_ARG_GUARD_REPRO` | Standing regression proof for the guard |
| Smoke env guardrails | (always-on harness) | Auto-set `MC2_ACTIVE_MOD` from manifest, clear stale `.modindex-cache`, classify load-phase fastfails as `mission_load_fail` not `crash_silent` |
| BRAIN-FIT-SCHEMA-1 | (checker only) | `mission_ai.fit` schema + `scripts/check-brain-fit-schema.py` |
| TASK-SCHEDULER-CORE-1 | `MC2_BRAIN_TASKQ` | Deterministic mission-ephemeral queue, key `(tier, sim_frame_ms, stable_seq, warrior_id)` |
| BRAIN-RUNTIME-1A | `MC2_BRAIN_RUNTIME` | `MechBrainRuntime` struct + mode detect + arbitration **trace only** |
| BRAIN-RUNTIME-1B | `MC2_BRAIN_RUNTIME_APPLY` | First applied behavior: HOLD_TASK → `setGeneralTacOrder(STOP)`; short-circuit lives **inside** runBrain, housekeeping outside the branch |
| DISPATCH-1A | `MC2_BRAIN_DISPATCH` | Load + parse `<m>_specials.fit`; trace recognized/unknown verbs; zero effects |
| DISPATCH-1B | `MC2_BRAIN_DISPATCH_APPLY` | First real TechScript effect: `Brain.CorePower false` → `setGeneralTacOrder(POWERDOWN)`; once-guard + supersede-HOLD = exactly one GENERAL write |
| DISPATCH-1C | `MC2_BRAIN_DISPATCH_FSM_TODO` | Parse + trace `; TODO: manual ABL line: state/trans/transBack/endstate;` comments → structured `[BRAIN_DISPATCH_FSM_TODO]` inventory of FSM logic the modder's auto-conversion dropped |

**Forbidden-call guard (still locked):** dispatch may call ONLY `setGeneralTacOrder` (for POWERDOWN). Still forbidden: `setPlayerTacOrder`, `setAlarmTacOrder`, `requestHelp`, `requestTarget`, `calcTacOrder`, `coreMoveTo`, `setMainGoal`, `clearCurTacOrder`, any movement/attack/OPORD-advance/commander function. Every new effect verb must justify what (if any) order function it adds.

## What's left (planned slices)

Ordered for low-risk first → behavior-bearing → architectural.

### Near-term (extends the proven pattern)

**1D — Per-unit Var namespace + minimal Var store.** The other deferred hazard from the dispatch recon. `Var.Set "name" value` without explicit scope is treated as **unit-scoped** (auto-prefixed with warrior id); `Var.Set "name" value scope=Mission` is mission-global (read-mostly; flagged as nondeterministic-write candidate). Add a small fixed-cap key→value store on `MechBrainRuntime` (mission-ephemeral, like everything else). `Var.Get` reads it; unknown keys default to 0/false. Gate `MC2_BRAIN_DISPATCH_VAR`, default OFF. Until then, `Var.*` stays UNKNOWN trace. Acceptance: byte-identical gate-OFF; fixture exercises both unit and mission scope; trace shows the namespaced key + scope.

**1E — `BrainSpecial.Call` chaining.** The modder uses `DO TechSpecial.Call "<key>"` heavily to split init/update bodies. Add a chain-call mechanism: the dispatcher recognizes the verb, looks up the named body in a parsed registry (parse-time index of all `TechSpecial { key=... }` blocks), and executes its body inline. Recursion guard (depth limit + visited-set). Gate `MC2_BRAIN_DISPATCH_CALL`. Unlocks a LOT of carver content in one slice. Acceptance: a fixture with init→update chain produces the right trace order; recursion guard fires on a cycle.

**1F-1J — More effect verbs (one per slice).** Each maps to an existing engine callsite. Same `DISPATCH_APPLY=1`-gated pattern, same once-guard discipline, each adds at most ONE forbidden-call exception that must be justified.
- `Brain.CoreAttack <target>` → existing combat tac-order callsite
- `OPORD.CoreGuard` → existing guard order
- `OPORD.CorePatrol <waypoint>` → existing patrol order
- `OPORD.CoreMoveTo <pos>` → existing move-to order
- `Unit.Retreat` → existing retreat tac-order

Order them by least-risky-first (probably Guard before MoveTo before Attack).

### Mid-term (real new mechanism)

**1K — FSM-state DSL.** Now we have the 1C inventory of what the conversion dropped, design and implement the real `Unit.InState`/`Unit.SetState` semantics: per-unit `activeState` field on `MechBrainRuntime`, parse-time state-block grouping, runtime state-gating of verbs, transition action. Probably needs a hand-authored fixture (the modder's auto-converted files don't have it). Acceptance: a multi-state fixture executes only the active state's verbs; SetState transitions deterministically.

**1L — Trace-only families.** `Audio.*`, `Video.*`, `Camera.*`, `TriggerArea.*`, `Debug.*`, `String.*` — most are thin wrappers around existing engine calls (PlaySound, PlayBIK, camera moves, debug print). Group into one or two slices; each verb is a recognized trace + a minimal apply that calls the existing engine fn. Low risk because the engine fns already exist and are stable.

**1M — BRAIN-RUNTIME-1C: Hybrid arbitration.** Today only Enhanced mode short-circuits ABL. Hybrid (ABL owns GENERAL, Brain owns PLAYER/ALARM) requires a different control flow: ABL runs, then the brain runtime writes PLAYER/ALARM slots from drained tasks *without* clobbering ABL's GENERAL write. The recon flagged the write-ordering rule (apply-drain after runBrain). One new effect verb that targets PLAYER (or ALARM) proves the path.

**1N — REQUEST_ORDERS + commander chain.** A unit posts `REQUEST_ORDERS` to its own queue → drain calls `getCommander()->receiveOrdersRequest()` → commander posts an order task into the requester's queue → that task writes a tac-order slot. Recursion guard (commander==self / commander==null fallback). Determinism: all writes inside the deterministic drain. The recon called this out as the "likely real-world crash path" with the existing ABL guard already in place — this slice consumes the guard.

**1O — OPORD slot advancement.** Primary→Secondary→Tertiary sequential slots. When the active OPORD's exit condition fires (or completes), advance to the next. RequestOrdersTask on final completion → up the commander chain → fallback policy (hold position / hold fire / withdraw / loop primary / keep requesting) from the Brain record.

### Architectural / GDD scope-lock items

**1P — BrainArchetype FIT resolver.** Layered overrides: archetype defaults ← per-unit override. The carver references show this isn't strictly needed for basic playable content but is in the GDD's scope-lock list.

**1Q — Multi-warrior `_ai.fit`.** Today's loader is single-block (per the 1B fixture). Support N Brain blocks per file with deterministic load order. Needs the `FitIniFile::seekBlock` multi-same-name semantics confirmed (flagged in the 1B recon).

**1R — ImGui Brain editor panel.** Tactic-weight sliders, archetype dropdown, switch groups, OPORD slot manager with map-click waypoint placement. Probably its own arc once the runtime is feature-complete.

### Later / lower-priority

- Logistics job network (repair trucks, supply convoys, capturable depots) — leaf feature.
- Infantry capture + sabotage OPORDs — leaf feature, needs infantry combat model touch-up.
- Clan-honor doctrine (Zellbrigen + dezgra switches) — leaf feature, content-driven.
- ABL→TechScript conversion tooling (auto-port `state X` / `trans Y` from legacy ABL into our 1K FSM DSL, replacing the TODO comments).

## Strategic decisions still open

1. **Var.Set scope=Mission policy.** The honest answer is "shared mission vars are nondeterministic across warriors; only safe for read-mostly setup." Either we (a) declare them read-mostly and enforce single-writer in tooling, (b) serialize their writes through the deterministic queue, or (c) allow them as-is and accept MP-replay drift on misuse. Pick before 1D ships writes.
2. **Auto-port tooling vs. hand-port content.** 1C surfaces what the modder's converter dropped (FSM, ABL-side variable declarations). Do we (a) extend the converter to emit our DSL, (b) hand-port the dropped logic into the carver content, or (c) accept content needs both passes? Affects timing of 1K and whether the modder wants to redo his conversion.
3. **First mod-mission goal.** What's the smoke-test target — "an MCO-ClanEagle mission runs with Mode=Enhanced and uses BrainSpecials for the whole mission flow"? That's a concrete milestone for the arc once enough effect verbs are in (probably after 1E-1J).
4. **Multiplayer correctness scope.** Single-player works today (deterministic within one peer). MP requires the determinism keys hold across peers — confirmed at design level but never tested with two clients. Worth a dedicated MP-determinism slice before content ships.

## Process keep

- Recon-first for any slice that adds new mechanism (FSM-state DSL, RequestOrders chain, Hybrid arbitration). Inline build for "extends shipped pattern" slices (more effect verbs, more trace-only families).
- Every behavior-changing slice = isolated worktree, default-OFF, fingerprint-confirmed gate matrix, hands-on review of control flow + forbidden-call guard, HEAD-verify after merge, build-verify after merge.
- Every new effect verb justifies which (if any) order/forbidden function it newly permits. Default-OFF gates compose (no global "everything on" switch).
- Smoke harness gates new env vars in `run_smoke.py` allowlist (subprocess.Popen drops env otherwise — bit us once).
- Arc-memory file (`memory/brain-ai-2.14-techscript-arc.md`) is the resume pointer; update it on every merge.

## Pickup points for any session

If a session needs to pick up: read this doc + `memory/brain-ai-2.14-techscript-arc.md` + the most recent `brain-runtime-*.md` / `techscript-special-dispatch-*.md` in `docs/render-backend-seams/`. The next slice is whatever's next in the "What's left" section above. The discipline rules in this doc + `memory/integration-merge-verification-rules.md` are non-negotiable.

---

## Addendum (2026-06-24): Mod-campaign stress findings

Recons completed: `techscript-call-chain-recon-1.md` and `mod-campaign-brain-stress-recon-1.md`. Three things change the picture:

### Critical content gap
**Only `carver_v_enhanced` ships in the BrainSpecial format the dispatcher reads.** Every other mod campaign (MCO, MC2X, MC2-Exodus) has raw `.abl` scripts and runs through the legacy ABL path. The modder pre-converted carver via tooling we don't have. So:
- Today's runtime is proven by carver + synthetic fixtures; that's the legitimate state.
- For MCO/MC2X to actually exercise the dispatcher, we need either: (a) an ABL→BrainSpecial converter slice (substantial), (b) hand-port one stress mission per campaign as we need it (cheap, targeted), or (c) defer mod-campaign stress until later. Recommendation: **(b) for the next several slices; queue (a) as its own arc** once the runtime is feature-complete enough that the converter target is stable.

### Top 3 hardest mod campaigns (future stress fleet)
- **MC2X-TCE** (76 missions) — `OPORD.Capture`/`Repair` (no engine callsite), wolfman 6-fragment include-chain (richest Call-chaining stress).
- **MC2-Exodus** (55 missions, 1023 warrior ABLs) — module-syntax mission brains (different parse path), unsupported `coreSentry`/`coreMoveToObject`.
- **MechCommanderOmnitech** (69 missions) — `coreAttackTactic` + `TACTIC_HIT_AND_RUN`/`JOUST` constants. **Only campaign exercising the 2.14 tactic-weight system** — the tactic-weight slice (currently mid-term-implicit) gets MCO as its stress target.

### Low-hanging fruit (~100% supported after one more verb)
**MCO-Wolf-Dragoons, MCO-dayofheroes, MCO-MercStar, MCO-DesertFox** — only `corePower` (✅) + `coreEject` (not yet). Adding `coreEject` as a small effect verb (insert between 1F-1J as `coreEject` slice) unlocks four mod campaigns of stress coverage (modulo the conversion gap above).

### Novel verbs to anticipate (beyond carver vocabulary)
- `OPORD.Capture`, `OPORD.Repair` (MC2X-TCE)
- `coreSentry`, `coreEscort`, `coreMoveToObject` (semantically distinct from `MoveTo`, MC2-Exodus)
- `coreAttackTactic` + tactic-weight constants (MCO) — ties directly into the 2.14 tactic-weight system
- `coreWait` timer/delay primitive — needs scheduler integration

### Stress mission picks per slice (recon recommendations)
| Slice | Stress mission target |
|---|---|
| 1E TechSpecial.Call chaining | MC2X-TCE wolfman include-chain (after hand-port or converter) |
| 1F-1J effect verbs (Guard/MoveTo/Attack/Retreat) | MCO-ClanEagle (Guard/Patrol stresses) |
| coreEject effect verb (insert before 1G) | MCO-Wolf-Dragoons / dayofheroes (near-100% supported after) |
| Tactic weights | MechCommanderOmnitech (only fleet member exercising tactic constants) |
| 1K FSM DSL | MC2-Exodus module-syntax missions (highest FSM TODO density) |
| 1K capture/repair verbs | MC2X-TCE OPORD.Capture / OPORD.Repair |

### Process tweaks (per advisor + recon)
- Effect-verb order: **Guard → MoveTo → Attack** (Attack has target resolution risk, MoveTo has pathing risk, Guard is least spicy). Locked.
- **1L split**: `1L-A` trace-only recognition; `1L-B` Audio + Debug apply; `1L-C` Camera + Video + TriggerArea apply (Camera/Video affect mission timing/presentation — don't bundle with debug). Locked.
- **1D Var mission-scope policy**: trace-only this slice; mission writes deferred until a guarded-single-writer (or read-mostly + serialized) design lands. Carver's vars are ALL `scope=Mission`, so under this policy carver chains *correctly* but Var state won't persist — a separate **`1D-M` mission-scope writes** slice unblocks full carver functional execution (no longer "later"; queue near-term after 1E).


---

## Addendum (2026-06-24 #2): On-disk format gap (discovered building 1D)

Sharper restatement of the mod-campaign content gap — discovered while implementing the Var parser.

**`FitIniFile::readIdString` reads `"..."` literals**, which means our `[Body]` block storing each verb line as `st DO0 = "<line>"` cannot contain a quoted arg without ending the outer string at the first inner `"`. The modder writes `DO Var.Set "ScenarioResult" PLAYING scope=Mission` — that's `"..."` inside `"..."`, which FitIniFile truncates.

Our 1D fixture works around it by using bracket syntax `Var.Set [foo] 1`, which the parser accepts as an alternate delimiter. But **carver's actual content uses `"..."` and would not load through our FitIniFile-based reader as-is.**

Three options, in order of investment:
1. **Raw line loader** for the `Body { ... DO ... }` block — bypass FitIniFile for that one sub-block (similar to how 1C scans TODO comments raw with `std::ifstream`). Single-file change; carver content loads as written.
2. **Pre-conversion step** that rewrites carver `"foo"` to `[foo]` at deploy time — keeps the engine simple, adds tooling complexity.
3. **Bracket as the canonical format** + require all content (carver + new) to use it. Worst for compatibility.

Recommendation: **option 1** — write a raw `Body { DO ... }` scanner (the 1C TODO scanner is the model). Schedule as `DISPATCH-LOADER-RAW-1` slice; insert before 1E (`TechSpecial.Call`) because the Call recon found carver uses 31/46 missions worth of Call chains, and we want to read them as the modder wrote them, not via a converter.

Net new slice order (revised):
```
DISPATCH-LOADER-RAW-1   ← NEW: raw-body scanner, no FitIniFile quote-truncation
CALL-CHAIN-1A           ← reads through the new loader
1F coreEject + Guard    ← unlocks 4 MCO low-hanging-fruit campaigns
1D-M mission-scope writes ← unblocks full carver var state
1G..1J other effect verbs
1K FSM DSL
1L-A/B/C trace families
1M/1N/1O architectural
```

Until LOADER-RAW lands, all real-content stress tests need the bracket-syntax workaround (or hand-port). Our synthetic fixtures continue to work either way.


---

## Addendum (2026-06-24 #3): Authority model — engine owns the spec

Disambiguating the multiple "Carver V" things and locking in the authority hierarchy.

**Disambiguation:**
- **Carver V** — the base stock MC2 campaign (loads with no mods active).
- **MCO Carver V** — MCO mod's version of the stock campaign.
- **MC2X CVE-G** — MC2X mod's version.
- **`carver_v_enhanced`** — a modder's framework-sketch fixture (the "TechScript" / `mission_specials.fit` content we'd been analyzing). NON-CANONICAL.

**Authority model (locked):**

```
Canonical spec:     engine-owned BrainSpecial / TechScript DSL
Compatibility corpus: carver_v_enhanced and other mod campaigns
Migration inputs:    legacy ABL, carver-shaped experiments, hand-ports
Runtime contract:    normalized internal BrainSpecialBody, not source-file quirks
```

**Implications:**
- Our fixture format IS the spec. The bracket form `[foo]`, the brace-block DSL, whatever the engine reads — that's normative.
- `carver_v_enhanced` was a napkin sketch from the modder scoping out what the framework could look like. Useful design inspiration; not the law; may even be wrong in places.
- Converter tooling emits OUR spec, not a clone of carver syntax. We pick the spec; mod content adapts to us (manually or via converter).
- The `LOADER-RAW-1` raw scanner is "engine-owned brace/body DSL loads without FitIniFile string-shape limitations" — NOT "real carver content now loads." Carver-shaped fixtures are stress-shape probes, not requirements.
- Mod-campaign stress: hand-port one stress mission per slice into our spec; full converter is a later arc we design.

**Bad pressure this removes:** "every weird upstream phrase must work forever." Replaced with: "every engine-owned fixture must work forever; converters adapt external formats into our shape." That's exactly how you avoid rebuilding ABL chaos under a new name.

**Adjusted acceptance defaults for the rest of the arc:**
- Synthetic canonical fixtures are fully legitimate proofs.
- Carver-shaped fixtures are optional stress-shape probes, not acceptance gates.
- Each effect verb: canonical fixture + one behavior + one slot write + APPLY-off trace-only + APPLY-on effect + justified forbidden-call exception.

