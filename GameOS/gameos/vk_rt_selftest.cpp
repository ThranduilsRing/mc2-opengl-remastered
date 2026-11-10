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
#include <span>
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
    uint32_t count = 0;
    std::string failure;

    explicit Bench(const VulkanContext& c) : ctx(c) {}

    // `r`: 8 floats per ray (origin.xyz, tMin, direction.xyz, tMax).
    bool init(std::span<const float> r, std::string_view spv = "rt_smoke.comp.spv", std::span<const BindingDesc> table = kSmokeBindings, uint32_t pushBytes = 0,
              uint32_t resultVec4s = 1) {
        const uint32_t n = static_cast<uint32_t>(r.size() / 8);
        const VkDevice dev = ctx.device();
        if (!set.create(ctx, table, pushBytes)) return fail("descriptor set");
        module = OwnedShaderModule(dev, vk_island_util::loadSpv(dev, vk_island_util::spvDir() + std::string(spv), nullptr));
        if (!module) return fail(spv);
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module.get();
        cpci.stage.pName = "main";
        cpci.layout = set.pipelineLayout();
        if (!mc2vk::create(pipe, dev, [&](VkPipeline* h) { return mc2vk::infra::compute(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, h); })) return fail("pipeline");

        if (n == 0) return fail("no rays");
        count = n;
        const size_t rayBytes = static_cast<size_t>(n) * 32;
        if (!rays.createHostWritable(ctx.allocator(), dev, rayBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "rt_selftest.rays") || !rays.upload(r.data(), rayBytes))
            return fail("rays buffer");
        if (!results.createHostWritable(ctx.allocator(), dev, static_cast<size_t>(n) * 16 * resultVec4s, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "rt_selftest.results"))
            return fail("results buffer");
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
        vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
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

    // `anyPrim`: the geometry has more than one triangle, so the primitive id is not checked.
    bool check(std::string_view stage, bool anyPrim = false) {
        float out[kRayCount][4] = {};
        if (!results.read(out, sizeof(out))) return fail("results readback");
        for (uint32_t i = 0; i < kRayCount; ++i) {
            const bool hit = out[i][0] > 0.5f;
            const bool ok = hit == kExpect[i].hit && (!hit || (std::fabs(out[i][1] - kExpect[i].t) < 1e-3f && (anyPrim || out[i][2] == 0.0f) &&
                                                              static_cast<uint32_t>(out[i][3]) == kCustomIndex));
            if (!ok) {
                return fail(std::format("{} ray {} got hit={} t={:.4f} prim={:.0f} custom={:.0f}", stage, i, hit ? 1 : 0, out[i][1], out[i][2], out[i][3]));
            }
        }
        return true;
    }
};

constexpr float kSelfTestRays[kRayCount][8] = {
    {0.25f, 0.25f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f},
    {2.0f, 2.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f},
    {0.1f, 0.1f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 10.0f},
    {0.25f, 0.25f, -1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 10.0f},
};

}  // namespace

bool traceRays(const VulkanContext& ctx, VkAccelerationStructureKHR tlas, std::span<const float> rays, std::span<float> out) {
    const size_t count = rays.size() / 8;
    if (!ctx.rt().rayQuery || tlas == VK_NULL_HANDLE || count == 0 || out.size() < count * 4) return false;
    Bench b(ctx);
    if (!b.init(rays)) return false;
    const bool ok = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        b.recordTrace(cmd, tlas);
    }, "rt trace probe");
    return ok && b.results.read(out.data(), count * 16);
}

bool traceSceneRays(const VulkanContext& ctx, const SceneProbeInputs& in, std::span<const float> rays, std::span<float> out, bool forceOpaque) {
    const size_t count = rays.size() / 8;
    const VkImageView off = in.alphaOff ? in.alphaOff : in.alphaOn[0], on = in.alphaOn[0] ? in.alphaOn[0] : in.alphaOff;   // a missing array: bind the other
    if (!ctx.rt().rayQuery || in.tlas == VK_NULL_HANDLE || !in.materials || !in.uvIndex || in.vbo == 0 || !off || count == 0 || out.size() < count * 8)
        return false;
    const VkDevice dev = ctx.device();
    Bench b(ctx);
    if (!b.init(rays, "rt_scene_probe.comp.spv", kSceneProbeBindings, kSceneProbePushBytes, 2)) return false;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    OwnedSampler samp;
    if (!mc2vk::create(samp, dev, [&](VkSampler* h) { return vkCreateSampler(dev, &sci, nullptr, h); })) return false;
    b.set.writeTlas(dev, 0, in.tlas);
    b.set.writeBuffer(dev, 1, b.rays.get());
    b.set.writeBuffer(dev, 2, b.results.get());
    b.set.writeBuffer(dev, 3, in.materials);
    b.set.writeBuffer(dev, 4, in.uvIndex);
    b.set.writeImage(dev, 5, samp.get(), off);
    for (uint32_t k = 0; k < mc2::prop_buckets::kMaxBuckets; ++k)   // PROP-ARRAY-BUCKETS: bindings 6..9, one per alpha-ON size bucket
        b.set.writeImage(dev, 6 + k, samp.get(), in.alphaOn[k] ? in.alphaOn[k] : on);
    const uint32_t push[4] = {static_cast<uint32_t>(in.vbo), static_cast<uint32_t>(in.vbo >> 32), in.stride, forceOpaque ? 1u : 0u};
    const bool ok = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        mc2vk::infra::bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.pipe.get());
        const VkDescriptorSet s = b.set.set();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, b.set.pipelineLayout(), 0, 1, &s, 0, nullptr);
        vkCmdPushConstants(cmd, b.set.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, (b.count + 63) / 64, 1, 1);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        ctx.cmdPipelineBarrier2(cmd, dep);
    }, "rt scene material probe");
    return ok && b.results.read(out.data(), count * 32);
}

bool runSelfTest(const VulkanContext& ctx) {
    if (!ctx.rt().rayQuery) {
        slog("skipped tier={} (no ray query on this device)", profile::rtTierName(ctx.rt().tier));
        return false;
    }
    const VkDevice dev = ctx.device();
    const VmaAllocator al = ctx.allocator();
    Bench b(ctx);
    if (!b.init({&kSelfTestRays[0][0], static_cast<size_t>(kRayCount) * 8})) { slog("FAIL init: {}", b.failure); return false; }

    const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint32_t idx[3] = {0, 1, 2};
    constexpr VkBufferUsageFlags kInput = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    OwnedBuffer vb, ib, inst;
    if (!vb.createFilled(al, dev, verts, sizeof(verts), kInput, "rt_selftest.vb") || !ib.createFilled(al, dev, idx, sizeof(idx), kInput, "rt_selftest.ib") ||
        !inst.createHostWritable(al, dev, kInstanceBytes, kInput, "rt_selftest.instances", -1, -1, kInstanceAddressAlignment)) {
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
    ScratchArena blasScratch, tlasScratch;   // every submit waits, so the arenas are destroyed at scope exit with no GPU use in flight
    OwnedQueryPool qp = createCompactionQueryPool(ctx, 1);
    if (!qp) { slog("FAIL query pool"); return false; }

    // Submit 1: upload (host writes, visible at submit) -> BLAS -> TLAS -> trace.
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
    ScratchArena blasUpd, tlasUpd;
    recorded = false;
    const bool s2 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        if (!recordBlasUpdate(ctx, cmd, g, blas, blasUpd)) return;
        recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
        if (!recordTlasUpdate(ctx, cmd, inst.deviceAddress(), 1, tlas, tlasUpd)) return;
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        b.recordTrace(cmd, tlas.get());
        recorded = true;
    }, "rt_selftest update");
    if (!s2 || !recorded || !b.check("update")) { slog("FAIL update: {}", b.failure.empty() ? "submit" : b.failure); return false; }

    // Stages 3-5: compaction. A BLAS built with ALLOW_UPDATE does not survive a COMPACT copy on the AMD RX 7900 XTX (the copy shrinks 724 -> 660 bytes and
    // every ray misses, with or without a refit), so a structure that will be compacted is built with ALLOW_COMPACTION only.
    BuildFlags cflags;
    cflags.allowCompaction = true;
    AccelStructure cb, small, tlas2;
    ScratchArena cbScratch, tlas2Scratch;
    const bool s3 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        if (!recordBlasBuild(ctx, cmd, g, cflags, "rt_selftest.blas.c", cb, cbScratch)) return;
        recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
        recordCompactionQuery(ctx, cmd, qp.get(), 0, cb);
    }, "rt_selftest compact build");
    const std::optional<VkDeviceSize> compactedSize = s3 ? readCompactedSize(ctx, qp.get(), 0) : std::nullopt;
    if (!compactedSize) { slog("FAIL compaction query"); return false; }
    const VkDeviceSize compacted = *compactedSize;
    const bool s4 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        recordCompactCopy(ctx, cmd, cb, compacted, "rt_selftest.blas.compact", small);
        recordBarrier(ctx, cmd, Phase::Copy, Phase::Tlas);
    }, "rt_selftest compact copy");
    if (!s4 || !small) { slog("FAIL compact: copy submit"); return false; }
    recorded = false;
    const bool s5 = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
        const VkAccelerationStructureInstanceKHR i0 = makeInstance(small.address(), nullptr, kCustomIndex);
        if (!inst.upload(&i0, sizeof(i0))) return;
        if (!recordTlasBuild(ctx, cmd, inst.deviceAddress(), 1, flags, "rt_selftest.tlas2", tlas2, tlas2Scratch)) return;
        recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
        b.recordTrace(cmd, tlas2.get());
        recorded = true;
    }, "rt_selftest compact trace");
    if (!s5 || !recorded || !b.check("compact")) { slog("FAIL compact: {}", b.failure.empty() ? "submit" : b.failure); return false; }

    // Stage 6 (informational; the evidence behind recordCompactCopy's ALLOW_UPDATE rule, memory amd-blas-compact-allow-update-2026-11-07): the matrix
    // {1 triangle, 16x16 grid = 512 triangles} x {C fast-trace, U|C fast-trace, U|C fast-build}. Each cell builds, COMPACT-copies, traces the copy, and
    // (updatable cells) refits the copy and traces again. One "matrix" line per cell; the PASS line counts the cells that missed.
    constexpr uint32_t kGridN = 16;
    constexpr uint32_t kGridVerts = (kGridN + 1) * (kGridN + 1);
    float gridVerts[kGridVerts * 3];
    uint32_t gridIdx[kGridN * kGridN * 6];
    for (uint32_t y = 0; y <= kGridN; ++y) {
        for (uint32_t x = 0; x <= kGridN; ++x) {   // offset by -0.01 so no ray lands exactly on a grid vertex or edge
            float* v = &gridVerts[(y * (kGridN + 1) + x) * 3];
            v[0] = static_cast<float>(x) / kGridN - 0.01f;
            v[1] = static_cast<float>(y) / kGridN - 0.01f;
            v[2] = 0.0f;
        }
    }
    for (uint32_t y = 0, k = 0; y < kGridN; ++y) {
        for (uint32_t x = 0; x < kGridN; ++x) {
            const uint32_t a = y * (kGridN + 1) + x, c = a + kGridN + 1;
            const uint32_t quad[6] = {a, a + 1, c, a + 1, c + 1, c};
            for (uint32_t j = 0; j < 6; ++j) gridIdx[k++] = quad[j];
        }
    }
    OwnedBuffer gvb, gib;
    if (!gvb.createFilled(al, dev, gridVerts, sizeof(gridVerts), kInput, "rt_selftest.grid.vb") ||
        !gib.createFilled(al, dev, gridIdx, sizeof(gridIdx), kInput, "rt_selftest.grid.ib")) {
        slog("FAIL grid buffers");
        return false;
    }
    TriangleGeometry grid = g;
    grid.vertices = gvb.deviceAddress();
    grid.vertexCount = kGridVerts;
    grid.indices = gib.deviceAddress();
    grid.triangleCount = kGridN * kGridN * 2;

    auto traceOver = [&](const TriangleGeometry& geo, bool anyPrim, AccelStructure& bl, AccelStructure& tl, ScratchArena& sc, std::string_view stage, bool refit) {
        bool rec = false;
        const bool ok = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
            if (refit) {
                if (!recordBlasUpdate(ctx, cmd, geo, bl, sc)) return;
                recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
            }
            const VkAccelerationStructureInstanceKHR i0 = makeInstance(bl.address(), nullptr, kCustomIndex);
            if (!inst.upload(&i0, sizeof(i0))) return;
            if (!recordTlasBuild(ctx, cmd, inst.deviceAddress(), 1, cflags, stage, tl, sc)) return;
            recordBarrier(ctx, cmd, Phase::Tlas, Phase::Consumer);
            b.recordTrace(cmd, tl.get());
            rec = true;
        }, stage);
        return ok && rec && b.check(stage, anyPrim);
    };
    struct Geo { const char* name; const TriangleGeometry* geo; bool anyPrim; };
    struct Var { const char* name; bool update; bool fastTrace; };
    const Geo geos[2] = {{"tri1", &g, false}, {"grid512", &grid, true}};
    const Var vars[3] = {{"C", false, true}, {"UCtrace", true, true}, {"UCbuild", true, false}};
    uint32_t missed = 0;
    for (const Geo& ge : geos) {
        for (const Var& v : vars) {
            BuildFlags f;
            f.allowCompaction = true;
            f.allowUpdate = v.update;
            f.preferFastTrace = v.fastTrace;
            AccelStructure src, copy, t1, t2;
            ScratchArena sc;
            const bool built = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
                if (!recordBlasBuild(ctx, cmd, *ge.geo, f, "rt_selftest.m.blas", src, sc)) return;
                recordBarrier(ctx, cmd, Phase::Blas, Phase::Tlas);
                recordCompactionQuery(ctx, cmd, qp.get(), 0, src);
            }, "rt_selftest matrix build");
            const std::optional<VkDeviceSize> csize = built && src ? readCompactedSize(ctx, qp.get(), 0) : std::nullopt;
            std::string copied = "fail:query", refit = "n/a";
            if (csize) {
                const bool cp = submitOneShot(ctx, [&](VkCommandBuffer cmd) {
                    if (recordCompactCopy(ctx, cmd, src, *csize, "rt_selftest.m.compact", copy)) recordBarrier(ctx, cmd, Phase::Copy, Phase::Tlas);
                }, "rt_selftest matrix copy");
                if (!cp || !copy) {
                    copied = "refused";
                } else {
                    copied = traceOver(*ge.geo, ge.anyPrim, copy, t1, sc, "rt_selftest.m.copied", false) ? "hit" : "miss(" + b.failure + ")";
                    if (v.update) refit = traceOver(*ge.geo, ge.anyPrim, copy, t2, sc, "rt_selftest.m.refit", true) ? "hit" : "miss(" + b.failure + ")";
                }
            }
            if (copied.starts_with("miss") || copied.starts_with("fail") || refit.starts_with("miss")) ++missed;
            slog("matrix geo={} flags={} as_bytes={} compacted_bytes={} copied={} refit={}", ge.name, v.name, static_cast<unsigned long long>(src.size()),
                 static_cast<unsigned long long>(csize.value_or(0)), copied, refit);
        }
    }

    slog("PASS rays={} blas_bytes={} compacted_bytes={} stages=build,update,compact matrix_cells=6 matrix_missed={}", kRayCount,
         static_cast<unsigned long long>(blas.size()), static_cast<unsigned long long>(compacted), missed);
    return true;
}

}  // namespace mc2vk::rt

