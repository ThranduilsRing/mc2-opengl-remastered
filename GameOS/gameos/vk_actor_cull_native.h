#pragma once
#include "gpu_cull_record.h"
#include <span>
#include <optional>
namespace gpu_cull {
struct NativeCullHeader { uint32_t visible, frame, dilated, culled; };
enum class NativeCullSubmit { Submitted, Busy, Failed };
bool nativeCullInit(uint32_t capacity);
void nativeCullShutdown();
NativeCullSubmit nativeCullSubmit(uint32_t slot, std::span<const GpuActorRecord> records,
                                const float* mvp, float dilation, uint32_t frame);
bool nativeCullPoll(uint32_t slot, NativeCullHeader& header, uint32_t& count,
                    std::span<uint32_t> visibility, std::span<uint32_t> ids);
std::optional<uint64_t> nativeCullLastDispatchNs();
}
