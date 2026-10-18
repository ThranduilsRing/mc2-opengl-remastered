<!-- doc-status: current | reviewed:2026-10-17 -->
# Step 4 B8 notes (B8a and B8b landed s22; B8c blocked)

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

## Outcome (lane 1, session 22, 2026-10-17)

- B8a.1 1bfca7d31: `Effect::m_children` is `std::vector<EffectPtr>` (one owner; a finished child is reset in place and erased after the
  loop, so Execute/Draw/Kill order is spawn order); the MLR cloud of Card, CardCloud, PertCloud, PointCloud, ShardCloud and Tube is a
  `unique_ptr` member. Doctest in `vfx_lifecycle_tests` (counted children: destroyed once on parent destroy, once when finished, never by Kill;
  cloud create/destroy x8). ABI guard: sizeof(Effect) 208 -> 200.
- B8a.2 57d5d23ee: `EffectLibrary::MakeEffect` returns `gosFX::EffectPtr`; the 30 effect members of Artillery, Carnage, BldgAppearance,
  MissionInterfaceManager, GVAppearance, TerrainObject, WeaponBolt, Mech3DAppearance are `EffectPtr` (83 delete+null pairs -> reset()).
  BUGS row 64: Mech3DAppearance never freed criticalSmoke, waterWake, helicopterDustCloud.
- B8b 2dc3a2ffc: the 8 host callbacks and ABLi_init's 8 allocator parameters are deleted; `ABL_alloc`/`ABL_free` (ablscan.h) serve three
  `std::pmr::unsynchronized_pool_resource`s (symbol, code, stack) owned from ABLi_init to the end of ABLi_close; zero-filled blocks; the
  runtime stack is `std::vector<StackItem>`; the host's three UserHeaps and the s13 ledgers are gone. Doctest `ABLArena/...` in the order seam.
- STAY BY DESIGN (WHY): the class-registry factories (`X::Make`, `Effect::Factory`) return the new raw object: they are registry function
  pointers and MakeEffect is the one place that gives the result an owner. `EffectCloud__Particle::m_effect` stays raw: particles live in the
  cloud's zeroed byte block, no constructor or destructor runs on a slot (freed by hand in DestroyParticle / ~EffectCloud). MLRShape
  Attach/DetachReference (10) is the MLR intrusive refcount boundary. PointLight's `m_light` is owned by gosFX::LightManager. ABL module
  instances, ABLFile, UserFile keep class operator new/delete on the C++ allocator (owned one by one; instance ownership is B8c).
  The `X__Specification` factories feed `mc2::particles::SpecLibrary` (the spec owner); `new ClassData` per class is the RegisteredClass
  registry entry, deleted in its TerminateClass; `Event` copies belong to their spec's `m_events` chain (plugs deleted with the spec).
- NARROWER than planned: one pool per kind for the whole environment, not per module (allocation sites carry no module identity).
  ScopedLeakCheck was not used for gosFX: vfx_lifecycle_tests cannot link the probe's operator new (the mc2fx stubs' `new(HGOSHEAP)` is
  malloc and Stuff::MemoryBlock frees it with delete[]); counted destructors prove single ownership instead.
- B8c still BLOCKED on the ABL save round-trip (unchanged).
