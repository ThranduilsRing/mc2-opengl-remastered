// vk_mech_skin_chunks.h -- splits mech skin node jobs into fixed-size vertex chunks so the dispatch is not sized by the frame's largest node.
// Header-only, no Vulkan. The shader takes any (srcFirst, count, dstFirst, boneBase) job, so a chunk is just a smaller job.
#ifndef MC2_VK_MECH_SKIN_CHUNKS_H
#define MC2_VK_MECH_SKIN_CHUNKS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mc2vk {

struct MechSkinJob { uint32_t srcFirst, count, dstFirst, boneBase; };

constexpr uint32_t kSkinChunk = 256;          // vertices per job (4 workgroups of 64)
constexpr uint32_t kMaxSkinJobs = 65535;      // maxComputeWorkGroupCount[1] floor

// Each node becomes ceil(count / chunk) jobs covering its vertices exactly once, in order. When that would exceed maxJobs the nodes are returned unchunked.
// chunkSkinJobsInto reuses the caller's `out` storage (cleared first); chunkSkinJobs is the allocating convenience form.
inline void chunkSkinJobsInto(const std::vector<MechSkinJob>& nodes, std::vector<MechSkinJob>& out, uint32_t chunk = kSkinChunk, uint32_t maxJobs = kMaxSkinJobs) {
    uint64_t total = 0;
    for (const MechSkinJob& n : nodes) total += (static_cast<uint64_t>(n.count) + chunk - 1) / chunk;
    out.clear();
    if (total > maxJobs) { out = nodes; return; }
    out.reserve(static_cast<size_t>(total));
    for (const MechSkinJob& n : nodes)
        for (uint32_t off = 0; off < n.count; off += chunk)
            out.push_back({n.srcFirst + off, (std::min)(chunk, n.count - off), n.dstFirst + off, n.boneBase});
}
inline std::vector<MechSkinJob> chunkSkinJobs(const std::vector<MechSkinJob>& nodes, uint32_t chunk = kSkinChunk, uint32_t maxJobs = kMaxSkinJobs) {
    std::vector<MechSkinJob> out;
    chunkSkinJobsInto(nodes, out, chunk, maxJobs);
    return out;
}

// Largest job in the list (what the dispatch x dimension is sized by).
inline uint32_t maxSkinJobCount(const std::vector<MechSkinJob>& jobs) {
    uint32_t m = 0;
    for (const MechSkinJob& j : jobs) m = (std::max)(m, j.count);
    return m;
}

}  // namespace mc2vk

#endif
