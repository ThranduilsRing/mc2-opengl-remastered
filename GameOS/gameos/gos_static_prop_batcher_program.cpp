// Native prop readiness, registration diagnostics and submission counters.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"

namespace gos_static_prop_batcher_detail {

// --- Step 7.7: strengthened IsCoalesceEnabled() body (plan v3.8). ---
// Returns true only when EVERY precondition for the coalesce draw branch
// is satisfied. False on any precondition lets flush()'s gate fall back
// to the legacy per-type/per-packet loop with no other state change.
// Cheap (all field reads + a few comparisons); safe to call per-frame.
bool IsCoalesceEnabled() {
    coalesce_resetEnvOnce();
    return !s_coalesceEnvDisabled && s_geometryFinalized && s_coalesceLayoutReady
        && s_coalesceEnabled && s_coalesceArmed
        && (s_alphaOffCount != 0 || s_alphaOnCount != 0);
}

// Layer B fallback: types we failed to register (logged once, fall back to CPU path).
std::unordered_map<TG_TypeNodeId, bool> s_failedTypes;

ObjBatcherCounters s_counters;

bool s_objbatcherTrace     = false;
bool s_objbatcherTraceInit = false;
bool s_atexitRegistered    = false;

// [SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.1 baseline. Counts submitMultiShape
// events whose child has isSpotlight==true. Separate state (not in
// ObjBatcherCounters) so the schema stays self-contained and easy to retire.
// First-hit emits one-line stderr always-on so an operator without the env still
// sees confirmation that SpotLight_ children are observed at all.
uint64_t s_spotlightReal_window  = 0;  // resets every 600 frames
uint64_t s_spotlightReal_mono    = 0;  // monotonic since process start
bool     s_spotlightReal_firstHit = false;

std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
std::unordered_set<std::string> s_lateRegisterAllowlist;
bool s_lateRegisterAllowlistLoaded = false;

// Slice 2, Stage 2.A — late-registration recovery signal.
// Set true inside the late-reg branch of submitMultiShape; cleared to false
// at the top of every submitMultiShape call. Read by the public method
// wasLastFailureLateRegistration() so callers (Stage 2.B) can set
// needsFullBakeNextFrame on the owning actor without adding a return-value
// channel to submitMultiShape.
bool s_lastSubmitWasLateReg = false;

// Monotonic counter of times submitMultiShape returned false on the
// late-registration branch (across all frames since process start).
// Added to the [OBJBATCHER v1] summary line.
uint64_t s_late_register_recovery_skips = 0;

// TREE-POP-TAG-1: population of the actor whose leaves submit() is currently
// pushing. Written ONLY while MC2_TREE_DIAG_TRACE is armed (see
// submitMultiShape) and read ONLY by the [TREE_DIAG] record, so the unarmed
// hot path is unchanged. See popShortName() for why this field had to exist.
GpuStaticPropPopulation s_currentSubmitPop = GpuStaticPropPopulation::Legacy;

// 2026-05-10 actor-center fix: parent multishape's world position captured
// at the top of submitMultiShape and read by submit() when populating the
// substrate-record worldCenter. Every leaf of one multishape shares the same
// cull-side center so the GPU frustum sphere-test accepts/rejects all leaves
// together (matches the original one-record-per-actor model). Reset to zero
// at every entry to submitMultiShape.
//
// Coordinate convention: stored in raw MC2 world coords already unswapped
// from Stuff/MLR (.x=east, .y=north, .z=elev), so the consumer in submit()
// can copy directly into rec.worldCenter without re-unswapping.
float s_currentSubmittingActorCenter[3] = {0.0f, 0.0f, 0.0f};
bool  s_currentSubmittingActorCenterValid = false;

// Slice 2 (Stage 2.D.3) — late-registration EVENT counters split by
// allowlist disposition. Incremented in the late-reg branch of
// submitMultiShape() per occurrence (NOT once-per-type — the dedup
// printf logic is unchanged, but every event ticks one of these so the
// true frequency is visible).
//
//   s_lateReg_allowed_events:    nodeId IS in objbatcher_late_register_allowlist.txt
//   s_lateReg_disallowed_events: nodeId is NOT (or empty)
//
// Read by GpuStaticPropBatcher::getAllowedLateRegEventCount() and
// getDisallowedLateRegEventCount().
uint64_t s_lateReg_allowed_events    = 0;
uint64_t s_lateReg_disallowed_events = 0;

// 2026-05-10 diag: per-typeID submit() histogram. MC2_SUBMIT_TYPEHIST=1 to
// enable. Answers H1 from
// docs/superpowers/plans/2026-05-10-substrate-coalesce-detail-and-perf-followup.md:
// "do detail-leaf typeIDs (Litwin_*, Door*, roof tiles) actually reach the
// per-leaf submit() under coalesce, or does the eligibility filter at
// submitMultiShape:2212-2231 strip them first?". Cross-reference the dump's
// non-zero typeIDs against the typeID→name table emitted by [STATIC_PROP_REG]
// at mission_load_byarr. Mirrors the [REGFLUSH_TYPEHIST v1] pattern.
bool                       s_submitTypeHistEnabled = false;
bool                       s_submitTypeHistInit    = false;
std::array<uint64_t, 1024> s_submitTypeHist{};
uint64_t                   s_submitTypeHistCalls   = 0;

void emitSubmitTypeHistDump(std::string_view trigger) {
    if (!s_submitTypeHistEnabled) return;
    // Build a typeID → name lookup by walking s_typeIndex once. n is ≤ a few
    // hundred types; quadratic walk is trivial for a 3-dump cadence.
    mc2::fmt::print_err("[SUBMIT_TYPEHIST v1] trigger={} calls={:d} non-zero buckets:\n",
        std::string(trigger).c_str(), mc2::fmt::as_u64(static_cast<unsigned long long>(s_submitTypeHistCalls)));
    for (size_t t = 0; t < s_submitTypeHist.size(); ++t) {
        if (s_submitTypeHist[t] > 0) {
            const char* name = "<unknown>";
            for (const auto& [shape, idx] : s_typeIndex) {
                if (auto* node = TglNodeRegistry::get().resolve({shape.value}); idx == t && node) {
                    name = node->getNodeId();
                    break;
                }
            }
            // alphaClass tells us which coalesce group this type lives in.
            // packets / indices: if a type has 0 packets or 0 indices, its
            // glMultiDrawElementsIndirect command has cmd.count=0 → no
            // triangles drawn even when instanceCount>0 (H3 of plan 2026-05-10).
            uint32_t aClass = 0xFFu, packets = 0u, indices = 0u, vertCount = 0u;
            if (t < s_types.size()) {
                const auto& ty = s_types[t];
                aClass    = ty.alphaClass;
                packets   = ty.packetCount;
                vertCount = ty.vertexCount;
                for (uint32_t p = 0; p < ty.packetCount; ++p) {
                    indices += s_packets[ty.firstPacket + p].indexCount;
                }
            }
            mc2::fmt::print_err("[SUBMIT_TYPEHIST v1] typeID={:d} name={} count={:d} alphaClass={:d} packets={:d} verts={:d} indices={:d}\n", mc2::fmt::as_size(t), mc2::fmt::safe((name && *name) ? name : "<empty>"), mc2::fmt::as_u64(static_cast<unsigned long long>(s_submitTypeHist[t])), mc2::fmt::as_u32(aClass), mc2::fmt::as_u32(packets), mc2::fmt::as_u32(vertCount), mc2::fmt::as_u32(indices));
        }
    }
    std::fflush(stderr);
}

void emitSubmitTypeHistAtExit() { emitSubmitTypeHistDump("atexit"); }

void emitFinalSummaryAtExit() {
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/true);
}

void accumulateMonotonicAndMaybeEmit(bool forceEmit) {
    s_counters.frame_count++;
    for (int p = 0; p < kPopCount; ++p) {
        s_counters.mono_eligible_actors_by_pop[p]     += s_counters.eligible_actors_by_pop[p];
        s_counters.mono_submitted_instances_by_pop[p] += s_counters.submitted_instances_by_pop[p];
        s_counters.mono_cpu_fallback_by_pop[p]        += s_counters.cpu_fallback_by_pop[p];
    }
    s_counters.mono_submitted_children  += s_counters.submitted_children;
    s_counters.mono_skipped_children    += s_counters.skipped_children;
    s_counters.mono_gpu_drawn_instances += s_counters.gpu_drawn_instances;

    const bool periodic = (s_counters.frame_count % 600 == 0
                           && s_counters.frame_count > 0);
    if (s_objbatcherTrace || periodic || forceEmit) {
        // Slice-1 fallback rate uses ONLY Building+Tree+Generic
        // populations (Legacy excluded — it's the prior killswitch path).
        uint64_t slice1_eligible = 0, slice1_fallback = 0;
        for (int p = 0; p <= 2; ++p) {  // Building, Tree, Generic
            slice1_eligible += s_counters.mono_eligible_actors_by_pop[p];
            slice1_fallback += s_counters.mono_cpu_fallback_by_pop[p];
        }
        const double fb_rate = (slice1_eligible > 0)
            ? static_cast<double>(slice1_fallback) / static_cast<double>(slice1_eligible)
            : 0.0;

        uint64_t total_submitted = 0;
        for (unsigned long p : s_counters.mono_submitted_instances_by_pop) {
            total_submitted += p;
        }

        // COUNTER-HONESTY-1: authoritative registry census, so a reader can never
        // again conclude "no trees in this scene" from submit_trees=0.
        //
        // submit_* counts DYNAMIC submitMultiShape successes only. The moment an
        // actor registers a recipe it switches to the persistent-static replay
        // path (submitCachedInstance, ~line 9525) which deliberately does NOT tick
        // these counters. Steady state for a fully-registered scene is therefore
        // submit_trees=0 with hundreds of trees drawing every frame. That zero has
        // already been misread at least twice in committed status docs, so the
        // line now carries both the honest denominator and an explicit note.
        uint32_t popCensus[5] = {0, 0, 0, 0, 0};
        GpuStaticPropRegistry::getPopulationCensus(popCensus);
        // Which populations are instrumented AT ALL. Without this, a reader
        // cannot tell reg_recipes_generics=0 ("no generic props") from the truth
        // ("nothing ever tags Generic, so this field can only ever be 0" --
        // GenericAppearance has no registerStatic and mints no recipe). Same
        // trap as submit_trees; refusing to repeat it here.
        const uint8_t popMask = GpuStaticPropRegistry::getPopulationsEverTagged();
        char popTagged[24];
        mc2::fmt::to_buf(popTagged, "{}{}{}{}", mc2::fmt::safe((popMask & 1u) ? "B" : ""), mc2::fmt::safe((popMask & 2u) ? "T" : ""), mc2::fmt::safe((popMask & 4u) ? "G" : ""), mc2::fmt::safe((popMask & 8u) ? "L" : ""));
        if (popTagged[0] == '\0') {
            mc2::fmt::to_buf(popTagged, "NONE");
        }

        MC2_DIAGFE(stderr,
               "[OBJBATCHER v1] event=summary frames=%llu "
               "eligible_actors=%llu submitted_instances=%llu "
               "submitted_children=%llu skipped_children=%llu "
               "cpu_fallback=%llu gpu_drawn_instances=%llu "
               // STALE-COUNTER-RETIRE-1: gpu_drawn_instances is incremented ONLY on the
               // legacy/per-type path (line ~7011, CPU-side TypeRangeSsbo.instanceCount).
               // Under the live GPU-authority coalesce-v6 / C1b path the instance counts
               // are GPU-authored into the indirect buffer and never read back, so this
               // reads 0 there. submitted_instances is the honest live-path signal.
               "gpu_drawn_note=legacy_path_only_v6_uses_submitted "
               "fallback_rate=%.4f "
               // COUNTER-HONESTY-1: submit_* are DYNAMIC-PATH-ONLY. Registered
               // props replay via submitCachedInstance() which does not tick them,
               // so submit_trees=0 means "no dynamic tree submits", NEVER "no trees".
               // reg_recipes_* below is the authoritative live-registry census.
               // Two separate lies in one field name, both named here:
               //  - "instances" is a misnomer: this counter ticks ONCE per
               //    successful submitMultiShape (an ACTOR). Per-leaf instances
               //    are submitted_children. See the increment site (once, after
               //    the child loop) vs submitted_children (inside it).
               //  - it only ever sees the DYNAMIC path; registered props replay
               //    via submitCachedInstance() which does not tick it.
               "submit_note=actors_not_instances;dyn_path_only_registered_props_replay_uncounted "
               "submit_buildings=%llu submit_trees=%llu "
               "submit_generics=%llu submit_legacy=%llu "
               "reg_recipes_buildings=%u reg_recipes_trees=%u "
               "reg_recipes_generics=%u reg_recipes_legacy=%u "
               "reg_recipes_untagged=%u "
               // A zero for a population NOT listed here is not a measurement.
               "reg_pops_ever_tagged=%s "
               "reg_census_trustworthy=%s "
               "late_register_recovery_skips=%llu "
               // LIGHTROW-FRESHNESS-1. stale_light_rows_regathered > 0 means the
               // dynamic path was handed prior-frame light slots on this run --
               // the defect the slice fixes, measured rather than assumed. The
               // other two must read 0; non-zero is an invariant break.
               "stale_light_rows_regathered=%llu "
               "stale_light_rows_unresolvable=%llu "
               "light_rows_out_of_range=%llu\n",
               static_cast<unsigned long long>(s_counters.frame_count),
               static_cast<unsigned long long>(slice1_eligible),
               static_cast<unsigned long long>(total_submitted),
               static_cast<unsigned long long>(s_counters.mono_submitted_children),
               static_cast<unsigned long long>(s_counters.mono_skipped_children),
               static_cast<unsigned long long>(slice1_fallback),
               static_cast<unsigned long long>(s_counters.mono_gpu_drawn_instances),
               fb_rate,
               static_cast<unsigned long long>(s_counters.mono_submitted_instances_by_pop[0]),
               static_cast<unsigned long long>(s_counters.mono_submitted_instances_by_pop[1]),
               static_cast<unsigned long long>(s_counters.mono_submitted_instances_by_pop[2]),
               static_cast<unsigned long long>(s_counters.mono_submitted_instances_by_pop[3]),
               popCensus[0], popCensus[1], popCensus[2], popCensus[3],
               popCensus[4], popTagged,
               // "no" the moment any live recipe carries the 0xFF unset default:
               // the breakdown is then incomplete and must not be read as a census.
               (popCensus[4] == 0u) ? "yes" : "no_untagged_recipes_present",
               static_cast<unsigned long long>(s_late_register_recovery_skips),
               static_cast<unsigned long long>(s_counters.mono_stale_light_row_regathered),
               static_cast<unsigned long long>(s_counters.mono_stale_light_row_unresolvable),
               static_cast<unsigned long long>(s_counters.mono_light_row_out_of_range));
        std::fflush(stderr);
    }

    // [SPOTLIGHT_REAL_TRACE v1] summary line, env-gated. Post-T3.2 the
    // increment site is gone (spotlight children are unconditionally skipped
    // at submit, the bit-2 flag is no longer emitted). Window/mono counters
    // therefore print 0 — that 0 IS the substitutive evidence (criterion #2:
    // zero static-prop draws contain spotlight-tagged packets). Demoted, not
    // deleted, per the Debug Instrumentation Rule.
    if (s_spotlightRealTrace && periodic) {
        mc2::fmt::print_err("[SPOTLIGHT_REAL_TRACE v1] event=summary site=static_prop_batcher frames={:d} window_spotlight_submits={:d} mono_spotlight_submits={:d}\n", mc2::fmt::as_u64(static_cast<unsigned long long>(s_counters.frame_count)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_spotlightReal_window)), mc2::fmt::as_u64(static_cast<unsigned long long>(s_spotlightReal_mono)));
        std::fflush(stderr);
    }
    if (periodic) {
        s_spotlightReal_window = 0;
    }

    for (int p = 0; p < kPopCount; ++p) {
        s_counters.eligible_actors_by_pop[p]     = 0;
        s_counters.submitted_instances_by_pop[p] = 0;
        s_counters.cpu_fallback_by_pop[p]        = 0;
    }
    s_counters.submitted_children  = 0;
    s_counters.skipped_children    = 0;
    s_counters.gpu_drawn_instances = 0;
}

void loadLateRegisterAllowlistOnce() {
    if (s_lateRegisterAllowlistLoaded) return;
    s_lateRegisterAllowlistLoaded = true;
    FILE* f = fopen("data/objbatcher_late_register_allowlist.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && *p != '\n' && *p != '\r' && *p != '#') ++p;
        *p = '\0';
        char* start = line;
        while (*start == ' ' || *start == '\t') ++start;
        char* end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (*start == '\0') continue;
        s_lateRegisterAllowlist.insert(start);
    }
    fclose(f);
}

}  // namespace gos_static_prop_batcher_detail
