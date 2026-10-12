# ABL and hierarchical-brain status — reconciled closure audit

Source snapshot: `97949f0f5`, 2026-10-12. Landing SHAs below are ancestors of this
snapshot. Historical closure evidence is `git show b80c96dc6:.claude/TECHSCRIPT-GAP-CLOSURE-1.md`;
its percentages are historical estimates, not an acceptance metric. This pass
reads source/history only; no engine build, harness or live mission proof was run.

**The runtime foundation landed; complete hierarchical behavior and owned brain
authoring did not. ABL must not be declared frozen on the basis of this foundation.**
Legacy ABL remains present at the snapshot SHA (`mclib/abl.h:37,88,95`,
`code/ablmc2.cpp:300,338-385,597`, `code/mission_init.cpp:911`). Enhanced behavior is
a parallel path with explicit arbitration, not automatic conversion of arbitrary
ABL (`18f20fb69`; `code/warrior.cpp:1358-1363,1445-1476`). The owner requested this
audit before the host-table design (`.claude/cleanup/HANDOFF-LOG.md:17`).

## Settled authority and conversion ruling

Roadmap Addendum #3 (`ce45ebfd9`; `docs/render-backend-seams/brain-arc-implementation-roadmap.md:165-197`)
makes the engine-owned normalized BrainSpecialBody DSL authoritative. Carver-shaped
content is a compatibility/stress corpus. The later closure ledger (`b80c96dc6`,
feature #23) explicitly declines an **in-engine** ABL→AST→IR→FIT converter: external
tools emit the engine spec. This audit preserves that ruling. External migration
tooling can be incomplete without becoming an engine/freeze requirement; editable
engine authoring, persistence and behavioral contracts are separate engine gaps.
The ledger also records GOTO/LABEL as a resolved spec delta: structured FSM, call
and latched flow over a deterministic queue, not an instruction-pointer VM.

Content boundary at `97949f0f5`: `git ls-files data` filtered for `.abl`/`.abx`
returns zero tracked files. Legacy interpreter sources (`mclib/abl*`) and the
host adapters (`code/ablmc2*`) were audited; canonical authored Brain fixtures
are under `tests/fixtures/brain_runtime/`. This is not mod-campaign execution
coverage or a claim that the external archives contain no ABL scripts.

## Landed mechanisms and current evidence

Each row identifies landed code, not a claim that its old test result is current.
The closure ledger's post-arc list supersedes the older roadmap's planned rows.

| Mechanism | Landing SHA(s) | Current source / proof surface |
|---|---|---|
| Brain runtime lifecycle and Enhanced ABL short-circuit | `7f11b6f40`, `89f95e876` / merge `18f20fb69` | `code/mech_brain_runtime.h:120-139`; `code/warrior.cpp:1358-1363,1445-1476`. Hybrid is still not applied arbitration. |
| Declarative Brain/archetype/OPORD parser and consumer | `60aff369e`, `76246bc78` | `code/brain_missionfit_oporbd.cpp:154,297,377-402`; per-warrior runtime consumed in `code/warrior.cpp:1636-1648`. |
| Root specials raw DSL loader / mission cache | `01f3c7575`, `9a9faa5fa` | `code/brain_special_dispatch.cpp:1781-2090,2312-2444`; quotes no longer depend on FitIni's bracket string shape. |
| Patrol routes, initial kick and finite slot progression | `4c0bd5ba2`, `a7a8699cc`, `6f39b1f27` | `code/brain_special_apply.cpp:299-392`; `code/brain_special_dispatch.cpp:1596-1729`; `code/warrior.cpp:1648`. Successor activation is limited (gap below). |
| Patrol/Guard enemy engagement, detection/range/LOS | `d3a799c7b`, `ffe170088`, `c1542ae0d` | `code/brain_special_dispatch.cpp:1419-1547`; `code/warrior.cpp:1645`. |
| Six root-body order effects; clear-move and target-policy intents | `ee985ba62`, `d55eefcbc`, `88a0c9011`, `79d01e757`, `3f4bab7ff`, `13289413a`; `cb5596b0a`, `6948f4bbd`; A/B harness `35d466387` | `tools/brain_dispatch_harness/README-v2.md:4-17`; recognized verbs `code/brain_special_dispatch.cpp:228-255`; symbol constants `code/brain_symbol_resolve.cpp:14-43`. |
| Deterministic task queue, emit/commit separation, WID-ordered deferred commit, snapshot and fixed cadence | `0c07f8810`, `254c3c029`, `e40598531`, `67f954ccd`, `b04fb6cdb` | `code/brain_special_dispatch.h:234-243`; `code/warrior.cpp:1467-1476,2320-2335`; these are gated mechanisms, not proof of whole-simulation determinism. |
| Unit variables and guarded shared mission variables | `e0f4723e1`, `0ed32225d` | `code/mech_brain_runtime.h:23-85`; `code/brain_special_dispatch.cpp:1102-1124,2469-2490,2565-2620`. |
| FSM current/previous state and conditional transition | `50f3662f7`, `1e02fdd64` | `code/mech_brain_runtime.h:138-147`; `code/brain_special_apply.cpp:445-534`; fixtures `tests/fixtures/brain_runtime/manifest.json:462-478`. |
| Call lookup/recursion guards; called-body trace only | `018e9d7de` | `code/brain_special_dispatch.cpp:1058-1097`; `code/brain_special_dispatch.h:227-228`. |
| Data aliases and stable canonical first-token rewrite | `9bd8ad174` | `code/brain_special_dispatch.cpp:104-135`; `code/brain_special_dispatch.h:33-35`; closure fixture `alias-registry` at `b80c96dc6`. |
| Global reusable specials with mission-local collision precedence | `a5147373b` | `code/brain_special_dispatch.cpp:2153-2193`; `code/brain_special_dispatch.h:37-39`. Library Call/parent targets, not a second entry body. |
| variantOf inheritance, whole-Body override and cycle/depth guard | `089f10994` | `code/brain_special_dispatch.cpp:2195-2270`; `code/brain_special_dispatch.h:41-45`. |
| WAIT / WAIT_UNTIL / STOP | `f9a3a67f4` | `code/brain_special_dispatch.cpp:787-855,1129-1216`; `code/mech_brain_runtime.h:88-104`. Sim-time/Var latched sequence gates; STOP ends this tick; root bodies only; no saved IP. |
| Deterministic raw brace-block writer | `364fbbcce` | `code/fitblockwriter.h:35-73`, `code/fitblockwriter.cpp:21-89`; recorded `--fitwriter-selftest` 13/13 at `b80c96dc6`. Writer round-trip is not an editable-panel implementation. |
| Read-only declarative Brain viewer and existing order editing | `cf892cbd4` (viewer); current presence at `97949f0f5` | `editor/UnitBrainPanel.cpp:294-317,401-413`; waypoint/undo mutation at `:67-77,191-207`. |

## Remaining engine gaps, evidence and acceptance

| Gap | Exact boundary | What proves closure |
|---|---|---|
| Called-body apply and composition | Apply calls the trace-only recursive dispatcher (`code/brain_special_dispatch.cpp:1058-1097`); effect processing remains root-only (`:1129`); explicit CALL-CHAIN-1B deferral (`code/brain_special_dispatch.h:227-228`). | Canonical nested effects/vars/FSM/flow, recursion/depth/cycle guards, stable intent ordering and once semantics; same engine slot writes through live integration. |
| Brain save/load | Runtime and task queue are mission-ephemeral (`code/mech_brain_runtime.h:2-11`, `code/warrior.h:870-871`); WAIT latches explicitly unpersisted (`code/mech_brain_runtime.h:88-104`). Scope includes FSM, unit/mission vars, wait latches, patrol/OPORD cursor, effect guards and queued/pending work; no IP exists. | Uninterrupted vs save/reload continuation has identical state, order and sim traces; malformed content safely rejected. Required before final-release save support can cover Enhanced brains. |
| Generic conditions, IF/ELSE/ENDIF and selectors | Recognition catalog contains FSM/Var/WAIT primitives but no generic IF/selector grammar (`code/brain_special_dispatch.cpp:228-270`); WAIT_UNTIL evaluator is Var equality (`:823-855`), SetStateIf unit-Var equality (`code/brain_special_apply.cpp:475-534`). Historical ledger `b80c96dc6` #3/#5/#6 agrees. | Owned vocabulary, deterministic evaluation and unit/target selection fixtures, malformed-expression rejection and real behavior. CONTACT_CRITERIA symbols alone are not selectors. |
| Hybrid slot arbitration | Only Enhanced selects the apply branch; otherwise legacy ABL executes (`code/warrior.cpp:1358-1363,1445-1476`). Hybrid ownership is computed/described, not applied (`code/mech_brain_runtime.h:106-115`; roadmap `:50`). | ABL GENERAL plus brain PLAYER/ALARM coexist with explicit priority/write order; collision fixture and sim trace. |
| Commander requests and full successor/fallback behavior | Current advance only increments a slot and sets hold/engage, with no successor route storage or commander/final fallback (`code/brain_special_dispatch.cpp:1596-1616`). REQUEST_ORDERS is absent from the recognized catalog (`:228-270`); roadmap contracts `:52-54`. | Null/self commander guards, deterministic request→response ordering, each finite successor route and final hold/withdraw/loop/request policy. Basic slot advancement is already landed above. |
| Remaining verb/domain families and reusable content | Audio/Video/Camera/TriggerArea/Debug/String and Social/Help/logistics/formation/air domains are outside the recognized catalog (`code/brain_special_dispatch.cpp:228-270`); roadmap split `:48,127`; closure #16/#18 at `b80c96dc6`. | One canonical fixture and live behavior per accepted family; core-special pack authored against that implemented vocabulary. No claim that 40+ reusable Specials ship today. |
| Owned editor authoring | Raw ABL and declarative Brain remain read-only (`editor/UnitBrainPanel.cpp:294-317`); live changes cover orders/stance (`:401-413`) rather than tactics/archetypes/switches/three OPORDs/special bodies. Writer exists (`code/fitblockwriter.h:35-73`), but `rg -n 'FitBlockWriter' editor` has zero consumers at the snapshot. | Owned document values and undo, editable Brain fields/slot routes, BrainSpecial line editor, diagnostics and writer-backed save→reopen→runtime round-trip; preserve current order editing. Distinct UnitBrainSpecial block and CampaignSpecial entry authoring remain limited to parser's current entry rules (`code/brain_special_dispatch.h:129-145`; closure #11/#13). |
| Test coverage and shipping defaults | Harness is a fake TacOrderSink (`tools/brain_dispatch_harness/README-v2.md:4-23`); engage/patrol remain default false (`Gates/gate_table.cpp:108,121`), and runtime tick is gated (`code/warrior.cpp:1636-1648`). Recorded 36-fixture/13-writer results at `b80c96dc6` were not rerun here. | Fresh offline fixture/writer proof, real parser/order/lookup/once-guard integration and patrol+engage outcomes, release defaults selected and verified under the current default-ON ruling. Historical default-OFF discipline is not today's shipping rule. |

## Smallest completion or explicit parking path

1. Keep the external-converter ruling fixed. Reconcile canonical fixtures and the
   supported vocabulary first; the gaps above define engine acceptance independent
   of legacy source syntax. Re-run the existing harness/writer checks before design.
2. Finish root/called-body behavioral composition and persistence, then resolve Hybrid
   and commander/OPORD completion contracts. Extend offline proofs with nested effects,
   continued WAIT/Var/FSM state and live order delivery at the boundaries cited above.
3. Complete owned Brain and BrainSpecial authoring using the landed writer; save/reopen
   the authored document and prove runtime consumption. All editor workflows remain
   must-have; a read-only viewer is not closure.
4. Any deferred family, Campaign scope or leaf ServiceJob/infantry feature needs a
   named scope/status in the owner ledger rather than a claim of full spec completion.
   Preserve resolved DSL deltas and external conversion; do not reopen them as blockers.
   A parked broader arc must say ABL remains unfrozen, with the narrower runtime contract
   and proof boundaries above. No new default-OFF shipping gates are proposed.

## Owner question that changes the design

1. Should Hybrid remain a shipped mode with ABL GENERAL and Brain PLAYER/ALARM, or
   should mission content explicitly choose Legacy or Enhanced? This determines the
   arbitration work at `code/warrior.cpp:1358-1476`; conversion authority is already settled.
