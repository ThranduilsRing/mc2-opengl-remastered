#pragma once

// Slice-2 Recon Zero instrumentation (MC2_OBJECT_RECON_TRACY=1).
//
// Default off. When the env var is set, accumulates wall-clock time spent in
// the per-frame static-prop update kernels. Surfaces a [OBJECT_RECON v1]
// summary line every 600 frames (and on shutdown). Used to decompose the
// ~2.4 ms `appearanceUpdate` Tracy zone into the sub-stages listed in
// docs/superpowers/explorations/2026-05-02-object-offload-slice2-recon-zero.md
// section 2.
//
// Discipline: per the recon prompt's Tracy-zone-overhead rule (MTPC < 1us =>
// don't add a Tracy zone; use accumulators), this header uses
// chrono::steady_clock deltas accumulated to file-static counters. The
// accumulators are sampled per-call, summed per-frame, drained per-600-frames.
// No Tracy zones are added by this header.
//
// Slice 1's existing Tracy zones (`Bldg::update`, `Tree::update`, etc.)
// continue to provide outer-scope timing; this layer adds intra-zone
// resolution.
//
// Dependencies: <chrono>, <cstdio>. No Tracy linkage.

#include <chrono>
#include <cstdint>

namespace mc2_object_recon {

// True only when MC2_OBJECT_RECON_TRACY is set in the environment at
// process start. Read once and cached. Default build sees `false`.
extern bool g_enabled;

// Accumulator buckets. Reset per frame; aggregated mono total survives.
struct Counters {
    // Outer per-population kernels.
    uint64_t bldg_update_ns       = 0;
    uint64_t bldg_update_calls    = 0;
    uint64_t tree_update_ns       = 0;
    uint64_t tree_update_calls    = 0;
    uint64_t generic_update_ns    = 0;
    uint64_t generic_update_calls = 0;

    // Multi-shape traversal (TG_MultiShape::TransformMultiShape).
    uint64_t mShape_total_ns      = 0;
    uint64_t mShape_calls         = 0;

    // Per-leaf TG_Shape::MultiTransformShape sub-stages.
    uint64_t shape_total_ns       = 0;
    uint64_t shape_alloc_ns       = 0;   // pool grabs
    uint64_t shape_xform_ns       = 0;   // per-vertex screen-space transform
    uint64_t shape_vlight_ns      = 0;   // per-vertex lighting kernel
    uint64_t shape_flight_ns      = 0;   // per-face lighting + listOfTriangles
    uint64_t shape_emit_ns        = 0;   // mc2::textures::ready() queue calls
    uint64_t shape_calls          = 0;
};

extern Counters g_per_frame;
extern Counters g_mono;
extern uint32_t g_frame_with_data; // first frame any counter became non-zero

// Called at process start (lazy init via static-init order is fine; the
// accumulators are zero-default).
void initFromEnv();

// Drain hook — call once per frame from gameosmain after frame counter
// increment. Emits per-frame line under MC2_OBJECT_RECON_TRACY=1; the
// 600-frame summary is unconditional once any non-zero data has been seen
// (matches the [TGL_POOL v1] cadence in tgl.cpp:3457-3472).
void drainPerFrame(uint32_t frameCounter);

// Emit final summary on shutdown (always, regardless of env gate).
void drainOnShutdown();

// Scope helper. Cheap when disabled (one bool check, no clock read).
class Scope {
    uint64_t* accum_;
    uint64_t* calls_;
    std::chrono::steady_clock::time_point t0_;
public:
    explicit Scope(uint64_t* accum, uint64_t* calls = nullptr)
        : accum_(g_enabled ? accum : nullptr),
          calls_(g_enabled ? calls : nullptr)
    {
        if (accum_) t0_ = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (accum_) {
            const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0_).count();
            *accum_ += static_cast<uint64_t>(dt);
            if (calls_) (*calls_)++;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace mc2_object_recon

// Convenience macro: place at the top of a scope to time it.
//   RECON_SCOPE(bldg_update_ns, bldg_update_calls);
// Expands to a stack-only Scope object that no-ops when disabled.
#define MC2_OBJECT_RECON_SCOPE_2(field, calls)                                  \
    ::mc2_object_recon::Scope _mc2_recon_scope_(                                \
        &::mc2_object_recon::g_per_frame.field,                                 \
        &::mc2_object_recon::g_per_frame.calls)
#define MC2_OBJECT_RECON_SCOPE_1(field)                                         \
    ::mc2_object_recon::Scope _mc2_recon_scope_(                                \
        &::mc2_object_recon::g_per_frame.field, nullptr)

// Multiple scopes in the same function need unique names; supply a tag.
#define MC2_OBJECT_RECON_SCOPE_TAGGED(tag, field)                               \
    ::mc2_object_recon::Scope _mc2_recon_scope_##tag(                           \
        &::mc2_object_recon::g_per_frame.field, nullptr)
