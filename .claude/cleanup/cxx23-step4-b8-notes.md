<!-- doc-status: current | reviewed:2026-10-17 -->
# Step 4 B8 notes (plan + census only; nothing implemented)

WHAT: the next ownership batch per `cxx23-step4-ownership-plan.md` (subsystem rows "gosFX" and "ABL", batch order line):
"B8 gosFX, then ABL heaps and environment (instances after the save fix)". WHY: B7o closed 2026-10-17 (s20); B8 is next.

## Census at 22a0e8228 (grep, `mclib/gosfx/*.cpp`; `mclib/abl*` + `code/ablmc2*`)

| subsystem | raw `new` | raw `delete` | other owners | `unique_ptr` today |
|---|---|---|---|---|
| gosFX | 72 tokens; per class the same 3-4 sites: `X__Specification(stream)` (spec factory), `new ClassData(...)` (class registry), `new MLR*Cloud(n)` (the render cloud an effect owns), `new gosFX::X(spec, flags)` (`X::Make`); plus `Event` copies (effect.cpp:131, :362) | 31 (effect.cpp 4, effectcloud.cpp 4, one or two per cloud class) | 10 MLRShape `AttachReference`/`DetachReference` (intrusive refcount, MLR boundary stays) | 0 |
| ABL | 38 | 28 | 8 host allocator callbacks (`ABL{System,Stack,Code,Symbol}{Malloc,Free}Callback`, installed at ablrtn.cpp:381-388) with 55 malloc + 37 free uses across ablrtn/ablenv/abldbug/ablsymt/ablscan/ablexec/abldecl/ablstmt/ablxstmt/ablexpr | 5 |

## Order inside B8 (narrowest first)

1. B8a gosFX effects: `Effect::Make` results become `unique_ptr<Effect>` (plain `new`, effect.cpp:503, no custom deleter); one owner for
   child effects (today freed twice-capable in `DeletePlugs` from `~Effect`, effect.cpp:483, and in the Kill path, effect.cpp:717).
   Each cloud's MLR cloud (`m_cardCloud` etc.) becomes a member owner. Specs stay owned by `mc2::particles::SpecLibrary`.
   Proof: doctest create/kill/destroy with ScopedLeakCheck; GPU particles sample the CPU simulation, so age/lifetime unchanged:
   day+night A/B captures with an effect on screen, sim pair (effects do not feed SIMID, but weapon hits spawn them).
2. B8b ABL arena: the 8 host callbacks are deleted; the stack is `std::vector<StackItem>`, one `std::pmr` resource per module for
   symbols and code. The arena is not in the save format, so it may move BEFORE the ABL save round-trip fix.
3. B8c ABL instances (environment owns modules; brains are handles): BLOCKED on the ABL save round-trip (plan row (f)): registry
   swap-remove (ablenv.cpp:1423) makes destruction order decide saved order.

Open from s20: `MechWarrior::init` allocates 38 UserHeap blocks per load that survive the next load (warrior_state.cpp:802): not
gosFX/ABL; a B8 side item only if the brains' ABL handles are what holds them.
