// CPU cull-record production. The native producer snapshots each submitted span.
#pragma once
#include <cstdint>
#include <span>
#include "gpu_cull_record.h"
namespace gpu_cull {
void substrate_init(uint32_t maxActors);
void substrate_shutdown();
void substrate_frameBegin();
// A further sim tick in the same render frame: drop this frame's dynamic records and start them again in the SAME ring slot
// (frameBegin would advance the slot once per tick and could wrap onto one the GPU still reads).
void substrate_frameRewind();
void substrate_submitDynamicActor(const GpuActorRecord& record);
void substrate_flushUpload();
bool substrate_isEnabled();
uint32_t substrate_getCpuVisibleCount();
uint32_t substrate_getCurrentRecordCount();
uint32_t substrate_capacity();
std::span<const GpuActorRecord> substrate_currentRecords();
// Frozen static prefix stays resident in the CPU ring; dynamic records append after it.
void substrate_rebuildStaticPrefix(const GpuActorRecord* records,uint32_t count);
void substrate_appendStaticPropRecord(const GpuActorRecord& record);
#if defined(MC2_SUBSTRATE_COUNT_PARITY)
void substrate_countParityCheck();
#endif
}
