//==========================================================================//
// File:    fx_trace.h                                                       //
// Contents: Neutral fx invocation counter (env-gated, default-off).         //
//           Plan v6 §1: see docs/superpowers/plans/                          //
//           2026-05-20-integrated-gosfx-retirement-gpu-particles-plan.md    //
//                                                                           //
// Three counters, all keyed by char* spec/leaf name:                        //
//   FX_TRACE_SPAWN(name)        - fires at EffectLibrary::Find              //
//                                  (survives A4; re-emitted by particles)   //
//   FX_TRACE_DRAW(name)         - fires at gosFX::Effect::Draw entry        //
//                                  (per-draw oracle; nonzero under A2)      //
//   FX_TRACE_MLR_ENQUEUE(name)  - fires at entry of each gated MLR work-    //
//                                  leaf BEFORE the gate early-return; A2    //
//                                  perf-gate oracle (must match default-off //
//                                  baseline +/- 5%; mlr_total Tracy zone    //
//                                  proves the early-return actually fires). //
//                                                                           //
// Env gate: MC2_FX_TRACE=1 enables stderr histogram dump at mission end +   //
// process atexit. Counters always accumulate (cost: one branch + one        //
// table-read + one atomic increment under env-on; under env-off the macro   //
// short-circuits to a single load of the cached env flag).                  //
//                                                                           //
// One-way dependency rule: gosfx/, mlr/, and particles/ all include this    //
// header; this header NEVER includes anything from those trees. Survives    //
// A4 deletion of gosfx/ + mlr/ unchanged.                                   //
//===========================================================================//

#pragma once

namespace mc2 {
namespace fx_trace {

// Cached at first call to is_enabled(); read once from MC2_FX_TRACE env.
bool is_enabled();

// Per-counter recording entrypoints. name is expected to be a stable string
// pointer (string-literal or interned spec name); the table keys by pointer
// identity OR string-equality (impl handles both).
void record_spawn(const char* name);
void record_draw(const char* name);
void record_mlr_enqueue(const char* leaf_name);

// C10: per-ClassID distribution counter. Called from gosFX::Effect::Start
// (per-Start-event semantics — counts effects that actually spawn).
// class_id is the Stuff::RegisteredClass::ClassID enum value; name is the
// spec name (for first-sighting cross-reference). Maintains:
//   - per-class total Start count
//   - first-seen spec name for each class (one-shot)
void record_class(unsigned int class_id, const char* name);

// Called at mission boundaries (and atexit) to dump and reset.
void dump_and_reset(const char* reason);

} // namespace fx_trace
} // namespace mc2

// Public macros. The is_enabled() check is the fast-path short-circuit
// under default-off (no string-table touch when disabled).
#define FX_TRACE_SPAWN(name) \
    do { if (::mc2::fx_trace::is_enabled()) { ::mc2::fx_trace::record_spawn(name); } } while (0)

#define FX_TRACE_DRAW(name) \
    do { if (::mc2::fx_trace::is_enabled()) { ::mc2::fx_trace::record_draw(name); } } while (0)

#define FX_TRACE_MLR_ENQUEUE(leaf_name) \
    do { if (::mc2::fx_trace::is_enabled()) { ::mc2::fx_trace::record_mlr_enqueue(leaf_name); } } while (0)

#define FX_TRACE_CLASS(class_id, name) \
    do { if (::mc2::fx_trace::is_enabled()) { ::mc2::fx_trace::record_class(static_cast<unsigned int>((class_id)), (name)); } } while (0)
