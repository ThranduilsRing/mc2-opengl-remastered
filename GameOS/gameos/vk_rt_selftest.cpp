// vk_rt_selftest.cpp -- one-shot GPU proof of the ray-tracing infrastructure (MC2_VK_RT_SELFTEST=1, test hook only; renders nothing).
//
// Builds a one-triangle BLAS and a one-instance TLAS, traces four known rays through shaders/vulkan/rt_smoke.comp and checks hit / miss, t, primitive id and
// instance custom index. Then refits both structures (update) and re-traces, and compact-copies the BLAS into a fresh TLAS and re-traces. Logs one
// [VK_RT_SELFTEST v1] line. Every submit waits (vk_one_shot), so the scratch and the structures are released at scope exit with no GPU use in flight.


#include "vk_rt_selftest.h"

#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "Gates/gate.h"
#include "vk_island_util.h"
#include "vk_one_shot.h"
#include "vk_rt_acceleration.h"
#include "vk_rt_descriptors.h"
#include "vulkan_context.h"

namespace mc2vk::rt {

namespace {

constexpr uint32_t kRayCount = 4;
constexpr uint32_t kCustomIndex = 7;

struct Expect { bool hit; float t; };
// ray 0: front hit at t=1; ray 1: beside the triangle; ray 2: back hit at t=1; ray 3: pointing away.
constexpr Expect kExpect[kRayCount] = {{true, 1.0f}, {false, 0.0f}, {true, 1.0f}, {false, 0.0f}};

template <class... Args>
void slog(std::format_string<Args...> fmt, Args&&... args) {
    const std::string line = std::format("[VK_RT_SELFTEST v1] {}\n", std::format(fmt, std::forward<Args>(args)...));
    std::fwrite(line.data(), 1, line.size(), stderr);
}

struct Bench {
    const VulkanContext& ctx;
    RayQuerySet set;
    OwnedShaderModule module;
    OwnedPipeline pipe;
    OwnedBuffer rays, results;
    std::string failure;

    explicit Bench(const VulkanContext& c) : ctx(c) {}

    bool init() {
        const VkDevice dev = ctx.device();
        if (!set.create(ctx)) return fail("descriptor set");
        module = OwnedShaderModule(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + "rt_smoke.comp.spv", nullptr));
        if (!module) return fail("rt_smoke.comp.spv");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module.get();
        cpci.stage.pName = "main";
        cpci.layout = set.pipelineLayout();
        if (!mc2vk::create(pipe, dev, [&](VkPipeline* h) { return mc2vk::infra::compute(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, h); })) return fail("pipeline");

        const float r[kRayCount][8] = {
            {0.25f, 0.25f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f},
            {2.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f},
            {0.1f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 10.0f},
            {0.25f, 0.25f, -1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 10.0f},
        };
        if (!rays.createHostWritable(ctx.allocator(), dev, sizeof(r), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "rt_selftest.rays") || !rays.upload(r, sizeof(r)))
            return fail("rays buffer");
        if (!results.createHostWritable(ctx.allocator(), dev, kRayCount * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "rt_selftest.results")) return fail("results buffer");
        return true;
    }

    bool fail(std::string_view what) {
        failure = what;
        return false;
    }

    // Records the dispatch against `tlas` (the caller has issued the Tlas -> Consumer barrier) and the compute -> host barrier for the readback.
    void recordTrace(VkCommandBuffer cmd, VkAccelerationStructureKHR tlas) {
        set.write(ctx.device(), tlas, rays.get(), results.get());
        mc2vk::infra::bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.get());
        const VkDescriptorSet s = set.set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, set.pipelineLayout(), 0, 1, &s, 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        ctx.cmdPipelineBarrier2(cmd, dep);
    }

    bool check(std::string_view stage) {
        float out[kRayCount][4] = {};
        if (!results.read(out, sizeof(out))) return fail("results readback");
        for (uint32_t i = 0; i < kRayCount; ++i) {
            const bool hit = out[i][0] > 0.5f;
            const bool ok = hit == kExpect[i].hit && (!hit || (std::fabs(out[i][1] - kExpect[i].t) < 1e-3f && out[i][2] == 0.0f &&
                                                              static_cast<uint32_t>(out[i][3]) == kCustomIndex));
            if (!ok) {
                return fail(std::format("{} ray {} got hit={} t={:.4f} prim={:.0f} custom={:.0f}", stage, i, hit ? 1 : 0, out[i][1], out[i][2], out[i][3]));
            }
        }
        return true;
    }
};

}  // namespace

bool runSelfTest(const VulkanContext& ctx) {
    if (!ctx.rt().rayQuery) {
        slog("skipped tier={} (no ray query on this device)", profile::rtTierName(ctx.rt().tier));
        return false;
    }
    const VkDevice dev = ctx.device();
    const VmaAllocator al = ctx.allocator();
    Bench b(ctx);
    if (!b.init()) { slog("FAIL init: {}", b.failure); return false; }

    const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint32_t idx[3] = {0, 1, 2};
    constexpr VkBufferUsageFlags kInput = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    OwnedBuffer vb, ib, inst;
    if (!vb.createFilled(al, dev, verts, sizeof(verts), kInput, "rt_selftest.vb") || !ib.createFilled(al, dev, idx, sizeof(idx), kInput, "rt_selftest.ib") ||
        !inst.createHostWritable(al, dev, kInstanceBytes, kInput, "rt_selftest.instances")) {
        slog("FAIL buffers");
        return false;
    }
    TriangleGeometry g;
    g.vertices = vb.deviceAddress();
    g.vertexCount = 3;
    g.indices = ib.deviceAddress();
    g.triangleCount = 1;

    BuildFlags flags;
    flags.allowUpdate = true;
    flags.allowCompaction = true;

    AccelStructure blas, tlas;
    OwnedBuffer blasScratch, tlasScratch;
    OwnedQueryPool qp = createCompactionQueryPool(ctx, 1);
    if (!qp) { slog("FAIL query pool"); return false; }

    // Submit 1: upload (host writes, visible at submit) -> BLAS (+ compaction query) -> TLAS -> trace.
    bool recorded = false;
    const bool s1 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        if (!recordBlasBuild(ctx, cmd, g, flags, "rt_selftest.blas", blas, blasScratch)) return;
        recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
        const float shift[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const VkAccelerationStructureInstanceKHR i0 = makeInstance(blas.address(), shift, kCustomIndex);
        if (!inst.upload(&i0, sizeof(i0))) return;
        if (!recordTlasBuild(ctx, cmd, inst.deviceAddress(), 1, flags, "rt_selftest.tlas", tlas, tlasScratch)) return;
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        b.recordTrace(cmd, tlas.get());
        recorded = true;
    }, "rt_selftest build");
    if (!s1 || !recorded || !b.check("build")) { slog("FAIL build: {}", b.failure.empty() ? "submit" : b.failure); return false; }

    // Submit 2: refit both structures in place and re-trace.
    OwnedBuffer blasUpd, tlasUpd;
    recorded = false;
    const bool s2 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        if (!recordBlasUpdate(ctx, cmd, g, blas, blasUpd)) return;
        recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
        recordCompactionQuery(ctx, cmd, qp.get(), 0, blas);  // sized after the last build or refit
        if (!recordTlasUpdate(ctx, cmd, inst.deviceAddress(), 1, tlas, tlasUpd)) return;
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        b.recordTrace(cmd, tlas.get());
        recorded = true;
    }, "rt_selftest update");
    if (!s2 || !recorded || !b.check("update")) { slog("FAIL update: {}", b.failure.empty() ? "submit" : b.failure); return false; }

    auto variant = [&](const char* label, bool upd, bool refit) {
        BuildFlags f;
        f.allowUpdate = upd;
        f.allowCompaction = true;
        AccelStructure vb, vs, vt;
        OwnedBuffer vbs, vus, vts;
        OwnedQueryPool vq = createCompactionQueryPool(ctx, 1);
        const bool a = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
            if (!recordBlasBuild(ctx, cmd, g, f, "rt_selftest.vb", vb, vbs)) return;
            recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
            if (refit && !recordBlasUpdate(ctx, cmd, g, vb, vus)) return;
            if (refit) recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
            recordCompactionQuery(ctx, cmd, vq.get(), 0, vb);
        }, "rt_selftest vA");
        const std::optional<VkDeviceSize> cs = readCompactedSize(ctx, vq.get(), 0);
        if (!a || !cs) { slog("diag {}: stage1 fail", label); return; }
        const bool c = submitOneShot(ctx, [&](VkCommandBuffer cmd) { recordCompactCopy(ctx, cmd, vb, *cs, "rt_selftest.vs", vs); }, "rt_selftest vC");
        if (!c || !vs) { slog("diag {}: copy fail", label); return; }
        const bool t = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
            const VkAccelerationStructureInstanceKHR i1 = makeInstance(vs.address(), nullptr, kCustomIndex);
            if (!inst.upload(&i1, sizeof(i1))) return;
            if (!recordTlasBuild(ctx, cmd, inst.deviceAddress(), 1, flags, "rt_selftest.vt", vt, vts)) return;
            recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
            b.recordTrace(cmd, vt.get());
        }, "rt_selftest vT");
        slog("diag {}: size={}->{} {}", label, static_cast<unsigned long long>(vb.size()), static_cast<unsigned long long>(*cs), t && b.check(label) ? "HIT-OK" : b.failure);
    };
    variant("compactOnly", false, false);
    variant("updNoRefit", true, false);
    variant("updRefit", true, true);

    // Submit 3: compact-copy the BLAS. Submit 4: build a fresh TLAS over the copy and re-trace.
    const std::optional<VkDeviceSize> compactedSize = readCompactedSize(ctx, qp.get(), 0);
    if (!compactedSize) { slog("FAIL compaction query"); return false; }
    const VkDeviceSize compacted = *compactedSize;
    AccelStructure small, tlas2;
    OwnedBuffer tlas2Scratch;
    const bool s3 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        recordCompactCopy(ctx, cmd, blas, compacted, "rt_selftest.blas.compact", small);
    }, "rt_selftest compact copy");
    if (!s3 || !small) { slog("FAIL compact: copy submit"); return false; }
    recorded = false;
    const bool s4 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        const VkAccelerationStructureInstanceKHR i0 = makeInstance(small.address(), nullptr, kCustomIndex);
        if (!inst.upload(&i0, sizeof(i0))) return;
        if (!recordTlasBuild(ctx, cmd, inst.deviceAddress(), 1, flags, "rt_selftest.tlas2", tlas2, tlas2Scratch)) return;
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        b.recordTrace(cmd, tlas2.get());
        recorded = true;
    }, "rt_selftest compact trace");
    if (!s4 || !recorded || !b.check("compact")) { slog("FAIL compact: {}", b.failure.empty() ? "submit" : b.failure); return false; }

    slog("PASS rays={} blas_bytes={} compacted_bytes={} stages=build,update,compact", kRayCount, static_cast<unsigned long long>(blas.size()),
         static_cast<unsigned long long>(compacted));
    return true;
}

}  // namespace mc2vk::rt

