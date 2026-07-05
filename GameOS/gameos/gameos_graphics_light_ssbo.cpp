// gameos_graphics_light_ssbo.cpp - [LIGHTSSBO v1] LightsData SSBO cluster.
// GAMEOS-GRAPHICS-SPLIT-1 slice 4: moved verbatim from gameos_graphics.cpp.
// Pure raw-GL free functions + file statics; no gosRenderer dependency.

#include "gameos.hpp"
#include "../../RenderCore/RenderResourceRegistry.h"
#include "../../RenderCore/GpuBufferOwner.h"

#include <GL/glew.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mc2_hitch_trace.h"

// MC2_GPUBUF_LIGHT_GROWONCE gate (moved with the cluster).
static bool gosLightGrowOnceEnabled() {
    static const bool s_on = (std::getenv("MC2_GPUBUF_LIGHT_GROWONCE") != nullptr);
    return s_on;
}

// ===================================================================
// [LIGHTSSBO v1] LightsData SSBO. Was a std140 UBO (ObjectLights
// light[64]) at LIGHT_DATA_ATTACHMENT_SLOT; converted to an unbounded
// std430 SSBO at LIGHT_DATA_SSBO_BINDING to remove the 64-slot ceiling
// (mc2_17 was 57/64 combined mech+static — one dense mission from silent
// corruption). The gos buffer API has no STORAGE type, so this is raw
// GL, mirroring the s_perCmdSsbo pattern. Named device-mediated helper
// (vulkan-prep): callers do NOT touch GL directly.
// See docs/superpowers/plans/2026-05-17-lightsdata-ubo-to-ssbo.md
// ===================================================================
// LIGHTDATA-SSBO-OWNER-1: the live default-path light-data SSBO is narrowed behind
// a GpuBufferOwner identity record (logical id + lifetime + debug name + GLuint
// value). Every gen/bind/bufferData/subdata/delete/guard site reaches the raw
// handle ONLY via s_lightDataOwner.glName; GL args/binding-slot/flags/order are
// unchanged. Persistent lifetime: lazy-created on first upload, destroyed in
// gos_LightDataSsbo_Destroy (txmmgr.cpp mcTextureManager teardown). Grow-once:
// on grow the handle is deleted+regenerated, so the owner is RE-REGISTERED on the
// new handle (registerLightDataSsbo helper) and invalidated on destroy.
static RenderCore::GpuBufferOwner s_lightDataOwner{
    RenderCore::RenderResourceId::LightDataSsbo,
    RenderCore::RenderResourceLifetime::Persistent,
    "LightDataSsbo",
    0u};
#define s_lightDataSsbo (s_lightDataOwner.glName)
static GLsizeiptr s_lightDataSsboBytes = 0;

// LIGHTDATA-SSBO-OWNER-1: (re)register the owner in the resource registry at every
// create/grow site (observe-only metadata; never read by the draw/upload path).
static void registerLightDataSsbo(GLsizeiptr bytes) {
    RenderCore::RenderResourceDesc d;
    d.id        = RenderCore::RenderResourceId::LightDataSsbo;
    d.kind      = RenderCore::RenderResourceKind::Buffer;
    d.lifetime  = RenderCore::RenderResourceLifetime::Persistent;
    d.format    = RenderCore::RenderResourceFormat::BufferRaw;
    d.debugName = "LightDataSsbo";
    d.glName    = static_cast<GLuint>(s_lightDataOwner.glName);
    d.sizeBytes = static_cast<uint64_t>(bytes);
    d.valid     = true;
    RenderCore::registerOrUpdateRenderResource(d);
}
static void invalidateLightDataSsbo() {
    RenderCore::RenderResourceDesc invalid;
    invalid.id = RenderCore::RenderResourceId::LightDataSsbo;
    RenderCore::registerOrUpdateRenderResource(invalid);
}
static const bool s_lightSsboTrace =
	(getenv("MC2_LIGHTSSBO_TRACE") != nullptr);

// LIGHT-GROW-ONCE-SUBDATA-1: per-record stride for headroom sizing.
// Lockstep with mclib/tgl.h `static_assert(sizeof(TG_HWLightsData) == 3600)`.
// gameos_graphics.cpp does NOT include tgl.h, so the value is mirrored here
// (same convention as LIGHT_DATA_SSBO_BINDING being a hardcoded #define).
// LIGHT-ABI-WIDEN-STAGE0-1: widened 1808->3600 (per-object cap N=16->32). This
// hand-copied literal is the sneakiest drift site — scripts/check-light-abi-lockstep.py
// fails CI if it disagrees with the other 4 lockstep sites.
// Headroom of +128 records matches the CPU backing grow step
// (mclib/txmmgr.cpp addLightDataStructure, `lightDataStructuresCapacity + 128`)
// so the GL grow cadence equals the CPU grow cadence — both amortized, rare.
static constexpr GLsizeiptr kLightRecordStride = 3600;
static constexpr GLsizeiptr kLightGrowHeadroomRecords = 128;

// LIGHT-PREFIX-GPU-COPY-1 (TXMMGR-PERF-EASYWINS-1): keep a VRAM stash of the
// immutable static light prefix [0..S) and, per frame, glCopyBufferSubData it
// into the freshly-orphaned slot-20 SSBO instead of re-pushing it over PCIe.
// Only the dynamic suffix [S..count) goes through glBufferSubData each frame.
//
// WHY: LIGHTSSBO-ORPHAN-1 (the NVIDIA implicit-sync stall fix) orphans the
// whole store every frame, which defeated the STATIC_LIGHT_UPLOAD_SPLIT
// prefix-skip — measured [RENDERLISTS_COST v1] light_upload ~950 µs/frame on
// mc2_24 (~2.7k static records × 3600 B ≈ 9.7 MB PCIe re-upload per frame).
//
// NVIDIA-SAFETY (why this does NOT reintroduce the ORPHAN-1 stall): the orphan
// still happens (fresh store, no in-flight readers), the prefix arrives via a
// GPU-side VRAM->VRAM copy (no CPU-blocking PCIe write), and the stash itself
// is only WRITTEN on prefixDirty frames (mission-load bake / re-bake) — it is
// read-only in steady state, so no cross-frame in-flight-write hazard exists.
// Contract: any [0..S) mutation sets mc2MarkStaticLightPrefixDirty()
// (mclib/txmmgr.cpp:1583-1591), which refreshes the stash here.
//
// Default OFF pending soak; kill-switch by unsetting. Requires the split path
// (MC2_STATIC_LIGHT_UPLOAD_SPLIT default-ON + MC2_LIGHTBAKE default-ON).
// Subsumed by MC2_GPUBUF_LIGHT_GROWONCE when that ships (grow-once branch
// runs first).
static bool gosLightPrefixGpuCopyEnabled() {
    static const bool s_on = []() {
        const char* v = std::getenv("MC2_LIGHT_PREFIX_GPU_COPY");
        return v && v[0] != '0';
    }();
    return s_on;
}
static GLuint     s_lightPrefixStash      = 0;  // immutable prefix mirror (VRAM)
static GLsizeiptr s_lightPrefixStashBytes = 0;  // stash capacity
static GLsizeiptr s_lightPrefixStashLive  = 0;  // live prefix bytes valid in stash

// LIGHT-GROW-ONCE-SUBDATA-1: when MC2_GPUBUF_LIGHT_GROWONCE is ON, upload only
// the live used bytes into the single persistent slot-20 SSBO via
// glBufferSubData (no per-frame full glBufferData orphan re-spec). Grow ONLY
// when used bytes exceed current GL capacity, sized with +128-record headroom
// so grow is rare. Returns true if it handled the upload (ON path); false to
// fall through to the unchanged OFF (orphan) path.
//
// CRITICAL NVIDIA CAVEAT (documented, NOT solved here — see slice + recon §3/§4):
// glBufferSubData into the single live buffer that is read all-frame by every
// lit draw (and cross-phase by the mech/static-prop batchers, txmmgr.cpp:485-492)
// has a CROSS-FRAME in-flight-write hazard. On NVIDIA, SubData into a buffer the
// GPU is still reading from the prior frame's draws STALLS the CPU until the GPU
// finishes — exactly the ~80ms stall the LIGHTSSBO-ORPHAN-1 orphan path dodges.
// On AMD this is tolerated (no stall), which is WHY this gate is default-OFF and
// NVIDIA is a HARD BLOCKER before any default-on. Do NOT add an N-buffer rotation
// to fix it — that is the full GpuStorageRing ring (explicitly out of scope here).
static bool gos_LightDataSsbo_UploadGrowOnce(const void* data, size_t bytes)
{
	const GLsizeiptr want = (GLsizeiptr)bytes;
	if (s_lightDataSsbo == 0) {
		// First create: allocate at requested size + headroom, no data copy
		// of trailing slack. Upload the live bytes via SubData.
		const GLsizeiptr cap = want + kLightGrowHeadroomRecords * kLightRecordStride;
		glGenBuffers(1, &s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		// Allocate uninitialized capacity (nullptr) ONCE, then fill live bytes.
		// GL_DYNAMIC_DRAW: rewritten-often, drawn-often (read every frame).
		glBufferData(GL_SHADER_STORAGE_BUFFER, cap, nullptr, GL_DYNAMIC_DRAW);
		s_lightDataSsboBytes = cap;
		MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, want, data);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: register at create
		if (s_lightSsboTrace) {
			std::fprintf(stderr,
			    "[LIGHTSSBO v1] event=growonce_create binding=%d capBytes=%td liveBytes=%zu\n",
			    LIGHT_DATA_SSBO_BINDING, (ptrdiff_t)cap, bytes);
			std::fflush(stderr);
		}
		return true;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
	if (want > s_lightDataSsboBytes) {
		// RARE grow. The old buffer may be read in-flight by the prior frame's
		// lit draws; a stall on the (rare) grow is acceptable, so DRAIN with
		// glFinish before deleting it. Recreate at new capacity WITH +128-record
		// headroom so growth amortizes, rebind to slot 20, log ONCE per grow.
		// RF2 invariant preserved: glBindBufferBase (buffer->binding-point) is
		// CONTEXT state and must re-follow the new storage; the program block
		// binding (gos_BindLightDataStorageBlock) is PROGRAM state and is NOT
		// re-issued here.
		const GLsizeiptr newCap = want + kLightGrowHeadroomRecords * kLightRecordStride;
		const GLsizeiptr oldCap = s_lightDataSsboBytes;
		glFinish();  // drain any in-flight reads of the old store before delete
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		glDeleteBuffers(1, &s_lightDataSsbo);
		glGenBuffers(1, &s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		glBufferData(GL_SHADER_STORAGE_BUFFER, newCap, nullptr, GL_DYNAMIC_DRAW);
		s_lightDataSsboBytes = newCap;
		MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, want, data);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: handle recreated on grow -> re-register
		if (s_lightSsboTrace) {
			std::fprintf(stderr,
			    "[LIGHTSSBO v1] event=growonce_grow oldCap=%td newCap=%td liveBytes=%zu\n",
			    (ptrdiff_t)oldCap, (ptrdiff_t)newCap, bytes);
			std::fflush(stderr);
		}
		return true;
	}
	// Steady state: in-place partial update of the live bytes ONLY. No orphan,
	// no full re-spec. This is the per-frame win — [GPUBUF v1] light owner
	// orphan bytes drop to ~0 (routed through MC2_GL_BufferSubData, NOT the
	// owner glBufferData macro, so the light orphan tally is not incremented).
	MC2_GL_BufferSubData(GL_SHADER_STORAGE_BUFFER, 0, want, data);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	return true;
}

void __stdcall gos_LightDataSsbo_Upload(const void* data, size_t bytes)
{
	if (bytes == 0) return;
	// LIGHT-GROW-ONCE-SUBDATA-1: ON path takes over entirely (grow-once +
	// per-frame SubData). When OFF, fall through to the byte-identical legacy
	// orphan path below — nothing in that path changes.
	if (gosLightGrowOnceEnabled()) {
		if (gos_LightDataSsbo_UploadGrowOnce(data, bytes)) return;
	}
	if (s_lightDataSsbo == 0) {
		glGenBuffers(1, &s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, data, GL_DYNAMIC_DRAW, LightSsbo);
		s_lightDataSsboBytes = (GLsizeiptr)bytes;
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: register at create (legacy path)
		if (s_lightSsboTrace) {
			std::fprintf(stderr, "[LIGHTSSBO v1] event=enabled binding=%d bytes=%zu\n",
			             LIGHT_DATA_SSBO_BINDING, bytes);
			std::fflush(stderr);
		}
		return;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
	if ((GLsizeiptr)bytes > s_lightDataSsboBytes) {
		// Grow: reallocate storage. RF2 — the buffer->binding-point
		// (glBindBufferBase below) is CONTEXT state and must follow the
		// new storage; the program block->binding
		// (glShaderStorageBlockBinding, gos_BindLightDataStorageBlock) is
		// PROGRAM state and is UNAFFECTED by buffer reallocation — do NOT
		// re-issue it here.
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, data, GL_DYNAMIC_DRAW, LightSsbo);
		if (s_lightSsboTrace) {
			std::fprintf(stderr, "[LIGHTSSBO v1] event=buffer_grow old=%td new=%zu\n",
			             (ptrdiff_t)s_lightDataSsboBytes, bytes);
			std::fflush(stderr);
		}
		s_lightDataSsboBytes = (GLsizeiptr)bytes;
		registerLightDataSsbo(s_lightDataSsboBytes);  // LIGHTDATA-SSBO-OWNER-1: storage realloc'd (same handle) -> refresh size
	} else {
		// LIGHTSSBO-ORPHAN-1: buffer orphaning eliminates the implicit GPU pipeline
		// sync stall on NVIDIA. glBufferSubData on a buffer that the GPU is still
		// reading (from the prior frame's draw calls) forces the NVIDIA driver to
		// block the CPU until the GPU finishes — observed as ~80ms in the
		// RenderLists.LightDataUpload Tracy zone on a 1050 Ti. AMD tolerates it
		// silently. glBufferData(nullptr) discards the old backing store immediately;
		// the driver retires it asynchronously once the GPU finishes, and hands the
		// CPU a fresh store with no sync stall. GL_STREAM_DRAW is the correct hint
		// for write-once-per-frame data (vs GL_DYNAMIC_DRAW which NVIDIA can place
		// in VRAM, making the subsequent write go through PCI-E with sync).
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, s_lightDataSsboBytes, nullptr, GL_STREAM_DRAW, LightSsbo);
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, data);
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void __stdcall gos_LightDataSsbo_UploadSplit(const void* data, size_t prefixBytes,
                                             size_t totalBytes, bool prefixDirty)
{
	if (totalBytes == 0) return;
	if (prefixBytes > totalBytes) prefixBytes = totalBytes;  // clamp (S floored by count)
	const char* base = static_cast<const char*>(data);

	// LIGHT-GROW-ONCE-SUBDATA-1: when ON, the split (prefix/suffix) optimization
	// is moot — the grow-once path keeps a persistent store, so SubData of the
	// full live range is correct and cheap (no orphan to defeat prefixDirty, no
	// full re-spec). Route the whole upload through the grow-once helper. The
	// per-frame cost is one glBufferSubData of usedBytes (the win). See the
	// NVIDIA in-flight caveat on gos_LightDataSsbo_UploadGrowOnce.
	if (gosLightGrowOnceEnabled()) {
		gos_LightDataSsbo_UploadGrowOnce(data, totalBytes);
		if (s_lightSsboTrace) {
			std::fprintf(stderr,
			    "[LIGHTSSBO v2] event=growonce_split_subsumed total=%zu prefix=%zu\n",
			    totalBytes, prefixBytes);
			std::fflush(stderr);
		}
		return;
	}

	// Create or grow → full upload (prefix necessarily included; dirty cleared
	// implicitly since the whole buffer is now fresh).
	if (s_lightDataSsbo == 0 || (GLsizeiptr)totalBytes > s_lightDataSsboBytes) {
		gos_LightDataSsbo_Upload(data, totalBytes);  // reuses create/grow + binding
		// LIGHT-PREFIX-GPU-COPY-1: the caller CONSUMED prefixDirty before this
		// early return. If a re-bake (S unchanged) landed on the same frame as a
		// buffer grow, the stash would silently keep the stale prefix. Force a
		// refresh on the next gated frame (cheap; grow frames are rare).
		s_lightPrefixStashLive = 0;
		if (s_lightSsboTrace) {
			std::fprintf(stderr, "[LIGHTSSBO v2] event=full_on_grow total=%zu prefix=%zu\n",
			             totalBytes, prefixBytes);
			std::fflush(stderr);
		}
		return;
	}

	// LIGHT-PREFIX-GPU-COPY-1 (gated, default OFF): orphan-preserving prefix
	// restore via VRAM->VRAM copy; PCIe traffic = dynamic suffix only.
	if (gosLightPrefixGpuCopyEnabled() && prefixBytes > 0) {
		// Refresh the stash when the prefix mutated (bake/re-bake sets the
		// dirty flag), on first use, or when S extended (mission-load growth).
		const bool stashStale = prefixDirty || s_lightPrefixStash == 0 ||
		                        (GLsizeiptr)prefixBytes != s_lightPrefixStashLive;
		if (stashStale) {
			if ((GLsizeiptr)prefixBytes > s_lightPrefixStashBytes) {
				// Grow with the same +128-record headroom cadence as the CPU
				// backing store so mission-load growth amortizes.
				const GLsizeiptr cap = (GLsizeiptr)prefixBytes +
				    kLightGrowHeadroomRecords * kLightRecordStride;
				if (s_lightPrefixStash) glDeleteBuffers(1, &s_lightPrefixStash);
				glGenBuffers(1, &s_lightPrefixStash);
				glBindBuffer(GL_COPY_READ_BUFFER, s_lightPrefixStash);
				glBufferData(GL_COPY_READ_BUFFER, cap, nullptr, GL_STATIC_DRAW);
				s_lightPrefixStashBytes = cap;
			} else {
				glBindBuffer(GL_COPY_READ_BUFFER, s_lightPrefixStash);
			}
			MC2_GL_BufferSubData(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)prefixBytes, base);
			s_lightPrefixStashLive = (GLsizeiptr)prefixBytes;
			if (s_lightSsboTrace) {
				std::fprintf(stderr,
				    "[LIGHTSSBO v3] event=prefix_stash_refresh prefix=%zu cap=%td dirty=%d\n",
				    prefixBytes, (ptrdiff_t)s_lightPrefixStashBytes, prefixDirty ? 1 : 0);
				std::fflush(stderr);
			}
		} else {
			glBindBuffer(GL_COPY_READ_BUFFER, s_lightPrefixStash);
		}
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
		// Same orphan discipline as ORPHAN-1: fresh store, no in-flight readers.
		MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, s_lightDataSsboBytes, nullptr, GL_STREAM_DRAW, LightSsbo);
		glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_SHADER_STORAGE_BUFFER,
		                    0, 0, (GLsizeiptr)prefixBytes);
		if (totalBytes > prefixBytes) {
			glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)prefixBytes,
			                (GLsizeiptr)(totalBytes - prefixBytes), base + prefixBytes);
		}
		glBindBuffer(GL_COPY_READ_BUFFER, 0);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		return;
	}

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_lightDataSsbo);
	// LIGHTSSBO-ORPHAN-1: orphan before any write to avoid implicit GPU sync stall
	// on NVIDIA (same root cause as the non-split path above). After orphaning, the
	// old data store is gone, so we must re-upload the prefix unconditionally —
	// the prefixDirty skip is disabled. On AMD the orphan is equally fast (~1us)
	// and eliminates the latent stall if the GPU falls behind the CPU.
	MC2_GL_BufferData_Owner(GL_SHADER_STORAGE_BUFFER, s_lightDataSsboBytes, nullptr, GL_STREAM_DRAW, LightSsbo);
	if (prefixBytes > 0) {
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)prefixBytes, base);
	}
	if (totalBytes > prefixBytes) {
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)prefixBytes,
		                (GLsizeiptr)(totalBytes - prefixBytes), base + prefixBytes);
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, LIGHT_DATA_SSBO_BINDING, s_lightDataSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	if (s_lightSsboTrace) {
		// prefixDirty is always treated as true post-orphan; log what was requested
		std::fprintf(stderr, "[LIGHTSSBO v2] event=split_orphan prefixDirty=%d prefix=%zu suffix=%zu\n",
		             prefixDirty ? 1 : 0, prefixBytes, totalBytes - prefixBytes);
		std::fflush(stderr);
	}
}

void __stdcall gos_LightDataSsbo_Destroy()
{
	if (s_lightDataSsbo) {
		glDeleteBuffers(1, &s_lightDataSsbo);
		s_lightDataSsbo      = 0;
		s_lightDataSsboBytes = 0;
		invalidateLightDataSsbo();  // LIGHTDATA-SSBO-OWNER-1: mark registry slot unavailable on teardown
	}
	// LIGHT-PREFIX-GPU-COPY-1: tear down the prefix stash alongside the main
	// SSBO; the next mission's first dirty frame recreates it.
	if (s_lightPrefixStash) {
		glDeleteBuffers(1, &s_lightPrefixStash);
		s_lightPrefixStash      = 0;
		s_lightPrefixStashBytes = 0;
		s_lightPrefixStashLive  = 0;
	}
}

