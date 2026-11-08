// Ray-tracing infrastructure layout math: acceleration-structure storage and scratch alignment, the compaction decision, the build-phase barrier chain and the
// shader binding table layout (vk_rt_acceleration.h / vk_rt_descriptors.h Vulkan-free halves). No Vulkan link.
#include "doctest.h"

#include <initializer_list>

#define MC2_RT_PURE_ONLY
#include "vk_rt_acceleration.h"
#include "vk_rt_descriptors.h"

using namespace mc2vk::rt;

TEST_CASE("rt layout: alignment and storage sizing") {
    CHECK(alignUp(0, 256) == 0);
    CHECK(alignUp(1, 256) == 256);
    CHECK(alignUp(256, 256) == 256);
    CHECK(alignUp(257, 256) == 512);
    CHECK(alignUp(5, 1) == 5);
    CHECK(asStorageBytes(0) == 256);   // an empty size query still gets a valid buffer
    CHECK(asStorageBytes(1000) == 1024);
    CHECK(kInstanceBytes == 64);
    CHECK(kInstanceAddressAlignment == 16);
}

TEST_CASE("rt layout: scratch is over-allocated so an aligned address always fits") {
    for (uint64_t align : {1ull, 64ull, 128ull, 256ull}) {
        const uint64_t need = 1000;
        const uint64_t bytes = scratchAllocBytes(need, align);
        for (uint64_t base : {0ull, 1ull, 100ull, 4096ull, 4097ull, 123457ull}) {
            const uint64_t aligned = scratchAlignedAddress(base, align);
            CHECK(aligned % align == 0);
            CHECK(aligned >= base);
            CHECK(scratchFits(base, bytes, need, align));
        }
    }
    // The bare size (no slack) does not always fit once the base is unaligned.
    CHECK_FALSE(scratchFits(1, 1000, 1000, 128));
    CHECK(scratchAllocBytes(0, 128) == 129);
}

TEST_CASE("rt layout: batched BLAS scratch sub-ranges are aligned, disjoint and grouped under the budget") {
    const uint64_t need[5] = {100, 0, 300, 600, 50};
    uint64_t off[5] = {};
    uint32_t grp[5] = {};
    // Aligned sizes 128, 0, 384, 640, 128; budget 1024: {0, 1, 2} = 512, then 640 would make 1152 -> new group {3, 4} = 768.
    CHECK(scratchBatchPlan(need, 128, 1024, off, grp) == 768);
    CHECK(off[0] == 0);
    CHECK(off[1] == 128);   // empty: takes no room
    CHECK(off[2] == 128);
    CHECK(off[3] == 0);
    CHECK(off[4] == 640);
    CHECK(grp[0] == 0);
    CHECK(grp[2] == 0);
    CHECK(grp[3] == 1);
    CHECK(grp[4] == 1);
    for (uint64_t o : off) CHECK(o % 128 == 0);
    // One build bigger than the budget still gets a group of its own.
    const uint64_t big[2] = {5000, 10};
    CHECK(scratchBatchPlan(big, 256, 1024, off, grp) == 5120);
    CHECK(grp[0] == 0);
    CHECK(grp[1] == 1);
    CHECK(scratchBatchPlan(std::span<const uint64_t>(big, 0), 256, 1024, off, grp) == 0);
}

TEST_CASE("rt layout: every batched build's scratch address is aligned and inside the reserve, whatever the base") {
    // Odd sizes (none a multiple of the alignment) on the RX 7900 XTX's 128 and a 256 alignment, from an aligned base and from a 4-aligned one.
    const uint64_t need[6] = {1, 129, 4093, 0, 77777, 255};
    for (const uint64_t align : {uint64_t{128}, uint64_t{256}}) {
        uint64_t off[6] = {};
        uint32_t grp[6] = {};
        const uint64_t peak = scratchBatchPlan(need, align, 64 * 1024, off, grp);
        for (const uint64_t base : {uint64_t{0x10000}, uint64_t{0x10004}}) {
            const uint64_t alloc = scratchAllocBytes(peak, align);
            const uint64_t aligned = scratchAlignedAddress(base, align);
            CHECK(scratchFits(base, alloc, peak, align));
            for (size_t i = 0; i < 6; ++i) {
                CHECK((aligned + off[i]) % align == 0);
                CHECK(aligned + off[i] + need[i] <= base + alloc);
                // Disjoint inside a group: the next build of the same group starts at or after this one ends.
                if (i + 1 < 6 && grp[i + 1] == grp[i] && need[i] > 0) CHECK(off[i + 1] >= off[i] + need[i]);
            }
        }
    }
}

TEST_CASE("rt layout: compaction is worth it at 10 percent saved or more") {
    CHECK(worthCompacting(1000, 900));
    CHECK(worthCompacting(1000, 100));
    CHECK_FALSE(worthCompacting(1000, 901));
    CHECK_FALSE(worthCompacting(1000, 1000));
    CHECK_FALSE(worthCompacting(1000, 1200));
    CHECK_FALSE(worthCompacting(0, 0));
    CHECK_FALSE(worthCompacting(1000, 0));
    CHECK(worthCompacting(1000, 500, 50));
    CHECK_FALSE(worthCompacting(1000, 600, 50));
}

TEST_CASE("rt layout: the build-phase barrier chain upload -> BLAS -> TLAS -> consumer") {
    const Barrier up = barrierBetween(Phase::Upload, Phase::Blas);
    REQUIRE(up.valid);
    CHECK(up.srcStage == (kStageTransfer | kStageComputeShader));   // a copy or a compute pass wrote the inputs
    CHECK(up.srcAccess == (kAccessTransferWrite | kAccessShaderWrite));
    CHECK(up.dstStage == kStageAsBuild);
    CHECK((up.dstAccess & kAccessShaderRead) != 0);                  // the build reads vertex / index / instance inputs as SHADER_READ
    CHECK((up.dstAccess & kAccessAsRead) != 0);
    CHECK((barrierBetween(Phase::Upload, Phase::Tlas).dstAccess & kAccessShaderRead) != 0);
    CHECK((barrierBetween(Phase::Blas, Phase::Tlas).dstAccess & kAccessShaderRead) == 0);

    const Barrier blas = barrierBetween(Phase::Blas, Phase::Tlas);
    REQUIRE(blas.valid);
    CHECK(blas.srcStage == kStageAsBuild);
    CHECK(blas.srcAccess == kAccessAsWrite);
    CHECK(blas.dstStage == kStageAsBuild);
    CHECK((blas.dstAccess & kAccessAsRead) != 0);

    const Barrier copy = barrierBetween(Phase::Blas, Phase::Copy);
    REQUIRE(copy.valid);
    CHECK(copy.dstStage == kStageAsBuild);   // the AS-COPY stage needs ray_tracing_maintenance1, which is not enabled
    const Barrier afterCopy = barrierBetween(Phase::Copy, Phase::Tlas);
    REQUIRE(afterCopy.valid);
    CHECK(afterCopy.srcStage == kStageAsBuild);
    CHECK(afterCopy.dstStage == kStageAsBuild);
    for (Phase a : {Phase::Upload, Phase::Blas, Phase::Copy, Phase::Tlas})
        for (Phase c : {Phase::Blas, Phase::Copy, Phase::Tlas, Phase::Consumer}) {
            const Barrier e = barrierBetween(a, c);
            CHECK(((e.srcStage | e.dstStage) & (kStageAsCopy | kStageRayTracingShader)) == 0);
        }

    const Barrier tlas = barrierBetween(Phase::Tlas, Phase::Consumer);
    REQUIRE(tlas.valid);
    CHECK(tlas.srcStage == kStageAsBuild);
    CHECK(tlas.srcAccess == kAccessAsWrite);
    CHECK(tlas.dstStage == kStageComputeShader);   // compute ray query; the ray-pipeline stage is invalid on a query-only device
    CHECK(tlas.dstAccess == kAccessAsRead);
}

TEST_CASE("rt layout: a reversed, repeated or post-consumer edge is invalid") {
    CHECK_FALSE(barrierBetween(Phase::Tlas, Phase::Blas).valid);
    CHECK_FALSE(barrierBetween(Phase::Blas, Phase::Blas).valid);
    CHECK_FALSE(barrierBetween(Phase::Consumer, Phase::Upload).valid);
    CHECK_FALSE(barrierBetween(Phase::Consumer, Phase::Consumer).valid);
    CHECK_FALSE(barrierBetween(Phase::Copy, Phase::Blas).valid);
    for (Phase a : {Phase::Upload, Phase::Blas, Phase::Copy, Phase::Tlas})
        for (Phase b : {Phase::Blas, Phase::Copy, Phase::Tlas, Phase::Consumer})
            CHECK(barrierBetween(a, b).valid == (phaseRank(b) > phaseRank(a)));
}

TEST_CASE("rt layout: the ray-query binding table is valid and starts with the TLAS") {
    CHECK(bindingsAreValid(kSmokeBindings.data(), static_cast<uint32_t>(kSmokeBindings.size())));
    CHECK(kSmokeBindings[0].kind == BindingKind::AccelerationStructure);
    const BindingDesc dup[] = {{0, BindingKind::AccelerationStructure, "a"}, {0, BindingKind::StorageBuffer, "b"}};
    CHECK_FALSE(bindingsAreValid(dup, 2));
    const BindingDesc noAs[] = {{0, BindingKind::StorageBuffer, "a"}};
    CHECK_FALSE(bindingsAreValid(noAs, 1));
    CHECK_FALSE(bindingsAreValid(nullptr, 0));
}

TEST_CASE("rt layout: the scene material probe table matches rt_scene_probe.comp") {
    CHECK(bindingsAreValid(kSceneProbeBindings.data(), static_cast<uint32_t>(kSceneProbeBindings.size())));
    REQUIRE(kSceneProbeBindings.size() == 7);
    CHECK(kSceneProbeBindings[0].kind == BindingKind::AccelerationStructure);
    for (uint32_t b = 1; b <= 4; ++b) CHECK(kSceneProbeBindings[b].kind == BindingKind::StorageBuffer);
    CHECK(kSceneProbeBindings[5].kind == BindingKind::SampledImage);
    CHECK(kSceneProbeBindings[6].kind == BindingKind::SampledImage);
    for (uint32_t b = 0; b < kSceneProbeBindings.size(); ++b) CHECK(kSceneProbeBindings[b].binding == b);
    CHECK(kSceneProbePushBytes == 16);   // uvec2 vbo, uint stride, uint mode
}

TEST_CASE("rt layout: shader binding table regions honour handle and base alignment") {
    // RTX-class limits: handle 32, handle alignment 32, base alignment 64.
    const SbtLayout l = computeSbtLayout(32, 32, 64, 2, 3, 0);
    REQUIRE(l.valid);
    CHECK(l.raygen.offset == 0);
    CHECK(l.raygen.size == 64);
    CHECK(l.raygen.stride == l.raygen.size);
    CHECK(l.miss.offset == 64);
    CHECK(l.miss.stride == 32);
    CHECK(l.miss.size == 64);
    CHECK(l.hit.offset == 128);
    CHECK(l.hit.size == 96);
    CHECK(l.callable.size == 0);
    CHECK(l.totalSize == 224);
    for (const SbtRegion* r : {&l.raygen, &l.miss, &l.hit}) CHECK(r->offset % 64 == 0);

    // A handle size that is not a multiple of the handle alignment rounds the stride up.
    const SbtLayout odd = computeSbtLayout(20, 8, 16, 1, 1, 1);
    REQUIRE(odd.valid);
    CHECK(odd.miss.stride == 24);
    CHECK(odd.raygen.size == 32);
    CHECK(odd.miss.offset == 32);
    CHECK(odd.hit.offset == 64);
    CHECK(odd.callable.offset == 96);
}

TEST_CASE("rt layout: a zero limit, a non power-of-two alignment or an oversize stride is rejected") {
    CHECK_FALSE(computeSbtLayout(0, 32, 64, 1, 1, 0).valid);
    CHECK_FALSE(computeSbtLayout(32, 0, 64, 1, 1, 0).valid);
    CHECK_FALSE(computeSbtLayout(32, 24, 64, 1, 1, 0).valid);
    CHECK_FALSE(computeSbtLayout(32, 32, 48, 1, 1, 0).valid);
    CHECK_FALSE(computeSbtLayout(32, 32, 64, 1, 1, 0, 16).valid);   // stride 32 > maxShaderGroupStride 16
    CHECK(computeSbtLayout(32, 32, 64, 1, 1, 0, 4096).valid);
}

TEST_CASE("rt refit: refitAction rebuilds on anything but an unchanged updatable topology") {
    const GeometrySignature a{100, 50, 106, 1, 40, 1, 0x1000};
    GeometrySignature moved = a;
    moved.indexSource = 0x2000;
    GeometrySignature grown = a;
    grown.vertexCount = 101;
    const GeometrySignature one[1] = {a};
    const GeometrySignature two[2] = {a, a};
    const GeometrySignature other[1] = {moved};
    const GeometrySignature bigger[1] = {grown};
    CHECK(refitAction({}, 0, one, kBuildAllowUpdate, 0) == RefitAction::Build);                                // nothing built yet
    CHECK(refitAction(one, 0, one, 0, 0) == RefitAction::Build);                                               // built without ALLOW_UPDATE
    CHECK(refitAction(one, kBuildAllowUpdate, one, kBuildAllowUpdate | kBuildAllowCompaction, 0) == RefitAction::Build);   // flags changed
    CHECK(refitAction(one, kBuildAllowUpdate, other, kBuildAllowUpdate, 0) == RefitAction::Build);             // another mesh section (LOD swap)
    CHECK(refitAction(one, kBuildAllowUpdate, bigger, kBuildAllowUpdate, 0) == RefitAction::Build);            // topology changed
    CHECK(refitAction(one, kBuildAllowUpdate, two, kBuildAllowUpdate, 0) == RefitAction::Build);               // geometry count changed
    CHECK(refitAction(one, kBuildAllowUpdate, one, kBuildAllowUpdate, kRefitsBeforeRebuild) == RefitAction::Build);   // BVH quality: rebuild after N
    CHECK(refitAction(one, kBuildAllowUpdate, one, kBuildAllowUpdate, kRefitsBeforeRebuild - 1) == RefitAction::Refit);
    CHECK(refitAction(two, kBuildAllowUpdate, two, kBuildAllowUpdate, 0) == RefitAction::Refit);
}

TEST_CASE("rt refit: compaction is forbidden for ALLOW_UPDATE on AMD proprietary") {
    CHECK(compactionAllowed(kBuildAllowCompaction, true));
    CHECK(compactionAllowed(kBuildAllowCompaction | kBuildAllowUpdate, false));
    CHECK_FALSE(compactionAllowed(kBuildAllowCompaction | kBuildAllowUpdate, true));
    CHECK_FALSE(compactionAllowed(kBuildAllowUpdate, false));
}

TEST_CASE("rt instance custom index: 24 bits, asserted at the limit") {
    CHECK(kMaxInstanceCustomIndex == 0xFFFFFFu);
    CHECK(instanceCustomIndexFits(0));
    CHECK(instanceCustomIndexFits(0xFFFFFFu));
    CHECK_FALSE(instanceCustomIndexFits(0x1000000u));
    CHECK_FALSE(instanceCustomIndexFits(0xFFFFFFFFu));
}

TEST_CASE("rt frame rebuild barrier: upload and compute inputs plus the previous frame AS builds and reads") {
    constexpr Barrier b = frameRebuildBarrier();
    constexpr Barrier up = barrierBetween(Phase::Upload, Phase::Blas);
    CHECK(b.valid);
    CHECK((b.srcStage & up.srcStage) == up.srcStage);
    CHECK((b.srcStage & kStageAsBuild) != 0);
    CHECK((b.srcAccess & kAccessAsWrite) != 0);
    CHECK(b.dstStage == up.dstStage);
    CHECK(b.dstAccess == up.dstAccess);
}
