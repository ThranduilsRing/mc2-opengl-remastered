// MissionInterfaceManager: MC2_MIF_SPLIT / MC2_IFACE_COST_SPLIT phase timing and the GPU-ground-pick parity oracle.
// Declared in the class headers; shared names in missiongui_shared_internal.h.
#include "missiongui_shared_internal.h"
#include "mc2fmt.h"


// ---- MC2_MIF_SPLIT: split MissionInterfaceManager::update() (the 1K 43.9ms) --
// Default OFF. Prime suspect: Camera::inverseProject walking every quad on
// camera-motion cache misses -> walk-vs-cacheHit counter proves it.
//
// MISSION-INTERFACE-PERF-1 (MC2_IFACE_COST_SPLIT, default OFF, either env
// enables the whole thing): extends the split with
//   - a rollovers bucket (updateRollovers was untimed),
//   - ControlGui::update sub-phase fold-in (accumulators DEFINED in
//     controlgui.cpp, extern below -- TOBJSPLIT precedent, terrobj.cpp),
//   - one-line [IFACE_PERF v1] emission every 900 frames + at mission end
//     (MissionInterfaceManager::destroy) so short smoke runs produce numbers
//     without an interactive Tracy session.
// chrono (not RDTSC) is fine here: per-phase once-per-frame brackets, not
// Index order MUST match the MF_CG_* tail of MifPhase below (fold loop in
// mfFrameEnd is the single mapping point).
extern unsigned long long g_ifaceCgFrameNs[8];
namespace missiongui_detail {
	static const char* s_mfNames[MF_COUNT] = {"TOTAL","invProj","LOS","controlGui","updateTarget","postTarget","drawBars","rollovers",
		"cg.pauseWnd","cg.btnHover","cg.rosterScan","cg.moverState","cg.tacMap","cg.infoWnd","cg.vehicleTab","cg.fgBar"};
	const bool s_mfOn = false || false;
	static unsigned long long s_mfSum[MF_COUNT]={0}, s_mfMax[MF_COUNT]={0};
	unsigned long long s_mfFrameNs[MF_COUNT]={0};
	static unsigned long long s_mfFrames=0;
	unsigned long long s_mfInvWalks=0, s_mfInvHits=0;
	// [IFACE_PERF v1] window accumulators (reset each emission).
	static unsigned long long s_mfWinSum[MF_COUNT]={0}, s_mfWinMax[MF_COUNT]={0};
	static unsigned long long s_mfWinFrames=0;
	unsigned long long s_mfWinWalks=0, s_mfWinHits=0;
	static const unsigned long long MF_WINDOW_FRAMES = 900ULL;
	static bool s_mfAtexit=false;
	static void mfEmit() {
		if (!s_mfOn) return;
		mc2::fmt::print_out("[MIF_SPLIT v1] event=shutdown frames={:d} invProj_walks={:d} invProj_cacheHits={:d}", mc2::fmt::as_u64(s_mfFrames), mc2::fmt::as_u64(s_mfInvWalks), mc2::fmt::as_u64(s_mfInvHits));
		for (int i=0;i<MF_COUNT;i++)
			mc2::fmt::print_out(" {}={{avg_us:{:.1f},max_us:{:.1f}}}", mc2::fmt::safe(s_mfNames[i]), s_mfFrames?static_cast<double>(s_mfSum[i])/s_mfFrames/1000.0:0.0, static_cast<double>(s_mfMax[i])/1000.0);
		mc2::fmt::print_out("\n"); std::fflush(stdout);
	}
	// One [IFACE_PERF v1] line per window (or partial window at mission end).
	void mfEmitWindow(std::string_view event) {
		if (!s_mfOn || !s_mfWinFrames) return;
		std::printf("[IFACE_PERF v1] event=%.*s frames=%llu invProj_walks=%llu invProj_cacheHits=%llu",
			static_cast<int>(event.size()), event.data(), s_mfWinFrames, s_mfWinWalks, s_mfWinHits);
		for (int i=0;i<MF_COUNT;i++)
			mc2::fmt::print_out(" {}={{avg_us:{:.1f},max_us:{:.1f}}}", mc2::fmt::safe(s_mfNames[i]), static_cast<double>(s_mfWinSum[i])/s_mfWinFrames/1000.0, static_cast<double>(s_mfWinMax[i])/1000.0);
		mc2::fmt::print_out("\n"); std::fflush(stdout);
		for (int i=0;i<MF_COUNT;i++) { s_mfWinSum[i]=0; s_mfWinMax[i]=0; }
		s_mfWinFrames=0; s_mfWinWalks=0; s_mfWinHits=0;
	}
	void mfFrameEnd(unsigned long long totalNs) {
		if (!s_mfOn) return;
		if (!s_mfAtexit) { s_mfAtexit=true; std::atexit(mfEmit); }
		++s_mfFrames; s_mfFrameNs[MF_TOTAL]=totalNs;
		for (int c=0;c<8;c++) { s_mfFrameNs[MF_CG_FIRST+c]=g_ifaceCgFrameNs[c]; g_ifaceCgFrameNs[c]=0; }
		for (int i=0;i<MF_COUNT;i++) {
			s_mfSum[i]+=s_mfFrameNs[i]; if(s_mfFrameNs[i]>s_mfMax[i]) s_mfMax[i]=s_mfFrameNs[i];
			s_mfWinSum[i]+=s_mfFrameNs[i]; if(s_mfFrameNs[i]>s_mfWinMax[i]) s_mfWinMax[i]=s_mfFrameNs[i];
			s_mfFrameNs[i]=0;
		}
		if (++s_mfWinFrames >= MF_WINDOW_FRAMES) mfEmitWindow("window");
	}
}

// ---- GPU-GROUND-PICK-PARITY-1 (S1): GPU-vs-CPU ground-pick oracle ----------
// Gate MC2_GPU_GROUND_PICK_PARITY (default OFF). Parity-ONLY: at each hover /
// click site the CPU Camera::inverseProject already produced the authoritative
// wPos; this oracle ALSO runs the GPU depth-unproject (lookupAtPixel) at the
// same cursor pixel, times both paths, and records the world-space position
// delta. The GPU result is NEVER consumed. Mirrors the MC2_MIF_SPLIT counter
// idiom above and the MC2_TERRAIN_PICK_PARITY discipline the recon cites
// (.claude/GPU-PICKING-RECON-1.md sec 6). Three measurements per the slice:
//   (a) delta distribution (world units) + >2wu flag lines with context,
//   (b) synchronous readback stall us vs the CPU quad-walk us (R1 verdict),
//   (c) buffer-unavailable frame count (pause/menu/ortho fallback -> R3 data).
namespace missiongui_detail {
	const bool s_gpParityOn = RenderWorld::IsGpuGroundPickParityEnabled();
	// Sample knob: run the GPU pick every Nth eligible site. Default 30 (NOT 1):
	// the synchronous single-pixel glReadPixels stalls the pipeline (measured
	// avg ~1.2ms, max tens of ms, worst-case >1s when the prior-frame FBO write
	// isn't yet visible), and firing it on EVERY camera-motion frame froze the
	// mc2_24 fly-through (heartbeat_freeze) in the S1 soak. A conservative
	// default sample rate keeps the oracle from freezing the game if someone
	// enables the gate without tuning; set MC2_GPU_GROUND_PICK_PARITY_SAMPLE=1
	// for a dense interactive parity capture (move the cursor by hand).
	static int s_gpSampleN = 30;   // the row reads 30 below 1
	static const std::string s_gpMissionTagText = mc2::gate::str<"MC2_GPU_GROUND_PICK_PARITY_TAG">();   // "" when unset or empty
	static const char* s_gpMissionTag = s_gpMissionTagText.empty() ? "?" : s_gpMissionTagText.c_str();
	// Center-probe knob: unattended smoke parks the cursor at (0,0), so the
	// primary cursor-driven oracle never samples real terrain (compared=0).
	// MC2_GPU_GROUND_PICK_PARITY_CENTER=1 adds a screen-CENTER parity probe on
	// camera-motion frames: CPU inverseProject at the center pixel vs GPU
	// depth-unproject at the same center pixel. This measures the actual
	// GPU-vs-CPU ground-pos delta over the terrain the camera is looking at
	// (incl. displaced terrain), which the parked-cursor path cannot. Same
	// throttle as the cursor path. Diagnostic-only; result never consumed.
	const bool s_gpCenter = false;
	static unsigned long long s_gpSites=0;          // eligible parity sites seen
	static unsigned long long s_gpCompared=0;        // both CPU+GPU produced a point
	static unsigned long long s_gpGpuInvalid=0;      // GPU had no world pos (fallback)
	static unsigned long long s_gpStaleGen=0;        // GENERATION_MISMATCH (recycled-slot pixel)
	static unsigned long long s_gpSlotDead=0;        // SLOT_DEAD / INDEX_OUT_OF_RANGE / TERRAIN_RESERVED
	static unsigned long long s_gpNoDepth=0;         // isValid record but depth==0 (no worldPos)
	static unsigned long long s_gpFbUnavail=0;       // buffer unavailable (R3)
	static unsigned long long s_gpBg=0;              // background/sky pixel (no depth)
	static unsigned long long s_gpOver2wu=0;         // delta > 2 wu
	static double s_gpDeltaSum=0.0, s_gpDeltaMax=0.0; // horizontal (xy) delta wu
	static double s_gpDeltaZSum=0.0, s_gpDeltaZMax=0.0; // vertical (z) delta wu
	static unsigned long long s_gpCpuNs=0, s_gpCpuMaxNs=0, s_gpCpuSamples=0;
	static unsigned long long s_gpGpuNs=0, s_gpGpuMaxNs=0, s_gpGpuSamples=0;
	// Histogram buckets for the horizontal delta distribution (wu).
	// [0]<=0.5  [1]<=1  [2]<=2  [3]<=5  [4]<=20  [5]>20
	static unsigned long long s_gpHist[6]={0};
	static unsigned long long s_gpcCompared=0, s_gpcInvalid=0, s_gpcBg=0;
	static double s_gpcDeltaSum=0.0, s_gpcDeltaMax=0.0;
	static double s_gpcDeltaZSum=0.0, s_gpcDeltaZMax=0.0;
	static unsigned long long s_gpcHist[6]={0};
	static bool s_gpAtexit=false;
	static void gpEmit() {
		if (!s_gpParityOn) return;
		const double cpuAvg = s_gpCpuSamples ? static_cast<double>(s_gpCpuNs)/s_gpCpuSamples/1000.0 : 0.0;
		const double gpuAvg = s_gpGpuSamples ? static_cast<double>(s_gpGpuNs)/s_gpGpuSamples/1000.0 : 0.0;
		const double dAvg   = s_gpCompared ? s_gpDeltaSum/static_cast<double>(s_gpCompared) : 0.0;
		const double dzAvg  = s_gpCompared ? s_gpDeltaZSum/static_cast<double>(s_gpCompared) : 0.0;
		mc2::fmt::print_err("[GPU_PICK_PARITY] event=shutdown mission={} sites={:d} compared={:d} gpu_invalid={:d} buffer_unavailable={:d} background_px={:d} over_2wu={:d}\n", mc2::fmt::safe(s_gpMissionTag), mc2::fmt::as_u64(s_gpSites), mc2::fmt::as_u64(s_gpCompared), mc2::fmt::as_u64(s_gpGpuInvalid), mc2::fmt::as_u64(s_gpFbUnavail), mc2::fmt::as_u64(s_gpBg), mc2::fmt::as_u64(s_gpOver2wu));
		mc2::fmt::print_err("[GPU_PICK_PARITY] invalid_reasons={{stale_gen:{:d},slot_dead:{:d},no_depth:{:d}}}\n", mc2::fmt::as_u64(s_gpStaleGen), mc2::fmt::as_u64(s_gpSlotDead), mc2::fmt::as_u64(s_gpNoDepth));
		mc2::fmt::print_err("[GPU_PICK_PARITY] delta_wu={{xy_avg:{:.3f},xy_max:{:.3f},z_avg:{:.3f},z_max:{:.3f}}} hist_xy_wu=[<=0.5:{:d},<=1:{:d},<=2:{:d},<=5:{:d},<=20:{:d},>20:{:d}]\n", dAvg, s_gpDeltaMax, dzAvg, s_gpDeltaZMax, mc2::fmt::as_u64(s_gpHist[0]), mc2::fmt::as_u64(s_gpHist[1]), mc2::fmt::as_u64(s_gpHist[2]), mc2::fmt::as_u64(s_gpHist[3]), mc2::fmt::as_u64(s_gpHist[4]), mc2::fmt::as_u64(s_gpHist[5]));
		mc2::fmt::print_err("[GPU_PICK_PARITY] timing_us={{cpu_invproj_avg:{:.1f},cpu_invproj_max:{:.1f},gpu_readback_avg:{:.1f},gpu_readback_max:{:.1f}}} cpu_samples={:d} gpu_samples={:d}\n", cpuAvg, static_cast<double>(s_gpCpuMaxNs)/1000.0, gpuAvg, static_cast<double>(s_gpGpuMaxNs)/1000.0, mc2::fmt::as_u64(s_gpCpuSamples), mc2::fmt::as_u64(s_gpGpuSamples));
		if (s_gpCenter) {
			const double cdAvg  = s_gpcCompared ? s_gpcDeltaSum/static_cast<double>(s_gpcCompared) : 0.0;
			const double cdzAvg = s_gpcCompared ? s_gpcDeltaZSum/static_cast<double>(s_gpcCompared) : 0.0;
			mc2::fmt::print_err("[GPU_PICK_PARITY] center_probe mission={} compared={:d} invalid={:d} background_px={:d} delta_wu={{xy_avg:{:.3f},xy_max:{:.3f},z_avg:{:.3f},z_max:{:.3f}}} hist_xy_wu=[<=0.5:{:d},<=1:{:d},<=2:{:d},<=5:{:d},<=20:{:d},>20:{:d}]\n", mc2::fmt::safe(s_gpMissionTag), mc2::fmt::as_u64(s_gpcCompared), mc2::fmt::as_u64(s_gpcInvalid), mc2::fmt::as_u64(s_gpcBg), cdAvg, s_gpcDeltaMax, cdzAvg, s_gpcDeltaZMax, mc2::fmt::as_u64(s_gpcHist[0]), mc2::fmt::as_u64(s_gpcHist[1]), mc2::fmt::as_u64(s_gpcHist[2]), mc2::fmt::as_u64(s_gpcHist[3]), mc2::fmt::as_u64(s_gpcHist[4]), mc2::fmt::as_u64(s_gpcHist[5]));
		}
		std::fflush(stderr);
	}
	// Center-screen parity probe. Caller (hover site) passes the CPU
	// inverseProject result at the CENTER pixel (ccx/y/z) + the center pixel
	// coords. Runs the GPU depth-unproject at the same center pixel and records
	// the delta. Throttled by the same sample counter. Result never consumed.
	void gpCenterProbe(int cxPix, int cyPix, float ccx, float ccy, float ccz) {
		if (!s_gpParityOn || !s_gpCenter) return;
		if ((s_gpSites % static_cast<unsigned long long>(s_gpSampleN)) != 0) return;
		if (!RenderWorld::IsObjectIdBufferEnabled()) return;
		float vMulX, vMulY, vAddX, vAddY;
		gos_GetViewport(&vMulX, &vMulY, &vAddX, &vAddY);
		if (!(vMulX > 0.0f && vMulY > 0.0f)) return;
		RenderWorld::ScreenPickContext ctx;
		ctx.mouseX = cxPix; ctx.mouseY = cyPix;
		ctx.vMulX  = vMulX;  ctx.vMulY  = vMulY;
		ctx.vAddX  = vAddX;  ctx.vAddY  = vAddY;
		ctx.drawableWidth  = Environment.drawableWidth;
		ctx.drawableHeight = Environment.drawableHeight;
		RenderWorld::screenPickCompute(&ctx);
		RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(ctx.glX, ctx.glY);
		if (!res.worldPosValid) {
			const char* r = res.lookupFailReason ? res.lookupFailReason : "";
			if (std::strcmp(r, "BACKGROUND_PIXEL") == 0) ++s_gpcBg; else ++s_gpcInvalid;
			return;
		}
		const float dx = res.worldX - ccx;
		const float dy = res.worldY - ccy;
		const float dz = res.worldZ - ccz;
		const double dHoriz = std::sqrt(static_cast<double>(dx)*dx + static_cast<double>(dy)*dy);
		const double dVert  = std::fabs(static_cast<double>(dz));
		++s_gpcCompared;
		s_gpcDeltaSum += dHoriz; if (dHoriz > s_gpcDeltaMax) s_gpcDeltaMax = dHoriz;
		s_gpcDeltaZSum += dVert; if (dVert  > s_gpcDeltaZMax) s_gpcDeltaZMax = dVert;
		int b = dHoriz <= 0.5 ? 0 : dHoriz <= 1.0 ? 1 : dHoriz <= 2.0 ? 2
		        : dHoriz <= 5.0 ? 3 : dHoriz <= 20.0 ? 4 : 5;
		++s_gpcHist[b];
		if (dHoriz > 2.0) {
			mc2::fmt::print_err("[GPU_PICK_PARITY] center_over_2wu mission={} center_px=({:d},{:d}) gl=({:d},{:d}) cpu=({:.1f},{:.1f},{:.1f}) gpu=({:.1f},{:.1f},{:.1f}) dxy={:.2f} dz={:.2f}\n", mc2::fmt::safe(s_gpMissionTag), mc2::fmt::as_i32(cxPix), mc2::fmt::as_i32(cyPix), mc2::fmt::as_i32(ctx.glX), mc2::fmt::as_i32(ctx.glY), ccx, ccy, ccz, res.worldX, res.worldY, res.worldZ, dHoriz, dVert);
		}
	}
	// Called at each ground-pick site. cpuNs = measured CPU inverseProject cost
	// for this site; (cx,cy,cz) = the CPU wPos it produced. mouseX/Y = raw cursor.
	// site = short label ("hover"/"Lmove"/"Rmove") for >2wu flag context.
	void gpParitySite(int mouseX, int mouseY, unsigned long long cpuNs,
	                         float cx, float cy, float cz, std::string_view site) {
		if (!s_gpParityOn) return;
		if (!s_gpAtexit) { s_gpAtexit = true; std::atexit(gpEmit); }
		++s_gpSites;
		s_gpCpuNs += cpuNs; ++s_gpCpuSamples;
		if (cpuNs > s_gpCpuMaxNs) s_gpCpuMaxNs = cpuNs;
		if ((s_gpSites % static_cast<unsigned long long>(s_gpSampleN)) != 0) return;
		if (!RenderWorld::IsObjectIdBufferEnabled()) { ++s_gpFbUnavail; return; }
		float vMulX, vMulY, vAddX, vAddY;
		gos_GetViewport(&vMulX, &vMulY, &vAddX, &vAddY);
		if (!(vMulX > 0.0f && vMulY > 0.0f &&
		      mouseX >= 0 && mouseY >= 0 &&
		      mouseX < static_cast<int>(vMulX) && mouseY < static_cast<int>(vMulY))) {
			++s_gpFbUnavail; return;   // offscreen / no viewport -> R3 fallback
		}
		RenderWorld::ScreenPickContext ctx;
		ctx.mouseX = mouseX; ctx.mouseY = mouseY;
		ctx.vMulX  = vMulX;  ctx.vMulY  = vMulY;
		ctx.vAddX  = vAddX;  ctx.vAddY  = vAddY;
		ctx.drawableWidth  = Environment.drawableWidth;
		ctx.drawableHeight = Environment.drawableHeight;
		RenderWorld::screenPickCompute(&ctx);
		const std::chrono::steady_clock::time_point g0 = std::chrono::steady_clock::now();
		RenderWorld::LookupResult res = RenderWorld::lookupAtPixel(ctx.glX, ctx.glY);
		const unsigned long long gpuNs = (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - g0).count();
		s_gpGpuNs += gpuNs; ++s_gpGpuSamples;
		if (gpuNs > s_gpGpuMaxNs) s_gpGpuMaxNs = gpuNs;
		// Classify the GPU result. lookupFailReason distinguishes buffer-absent
		// (NO_FBO_OR_TEX / NO_POSTPROCESS -> R3) from an in-scene sky/far pixel.
		if (!res.worldPosValid) {
			const char* r = res.lookupFailReason ? res.lookupFailReason : "NO_WORLD_POS";
			if (std::strcmp(r, "NO_FBO_OR_TEX") == 0 ||
			    std::strcmp(r, "NO_POSTPROCESS") == 0 ||
			    std::strcmp(r, "OID_BUFFER_DISABLED") == 0) {
				++s_gpFbUnavail;
			} else if (std::strcmp(r, "BACKGROUND_PIXEL") == 0) {
				++s_gpBg;           // sky / far plane under cursor (depth==0)
			} else if (std::strcmp(r, "GENERATION_MISMATCH") == 0) {
				++s_gpStaleGen;     // recycled-slot pixel: last-write-wins stale objectID
				++s_gpGpuInvalid;
			} else if (std::strcmp(r, "SLOT_DEAD") == 0 ||
			           std::strcmp(r, "INDEX_OUT_OF_RANGE") == 0 ||
			           std::strcmp(r, "TERRAIN_RESERVED") == 0) {
				++s_gpSlotDead;
				++s_gpGpuInvalid;
			} else {
				// isValid record parsed but depth==0 -> object pixel without a
				// resolvable world pos (rare); or an unclassified reason string.
				++s_gpNoDepth;
				++s_gpGpuInvalid;
			}
			return;
		}
		// Both paths produced a world point: record the delta. MC2 world convention
		// (matches EditorInspector consumer): x/y horizontal plane, z elevation.
		const float dx = res.worldX - cx;
		const float dy = res.worldY - cy;
		const float dz = res.worldZ - cz;
		const double dHoriz = std::sqrt(static_cast<double>(dx)*dx + static_cast<double>(dy)*dy);
		const double dVert  = std::fabs(static_cast<double>(dz));
		++s_gpCompared;
		s_gpDeltaSum += dHoriz; if (dHoriz > s_gpDeltaMax) s_gpDeltaMax = dHoriz;
		s_gpDeltaZSum += dVert; if (dVert  > s_gpDeltaZMax) s_gpDeltaZMax = dVert;
		int b = dHoriz <= 0.5 ? 0 : dHoriz <= 1.0 ? 1 : dHoriz <= 2.0 ? 2
		        : dHoriz <= 5.0 ? 3 : dHoriz <= 20.0 ? 4 : 5;
		++s_gpHist[b];
		if (dHoriz > 2.0) {
			++s_gpOver2wu;
			std::fprintf(stderr,
				"[GPU_PICK_PARITY] over_2wu site=%.*s mission=%s screen=(%d,%d) gl=(%d,%d) "
				"cpu=(%.1f,%.1f,%.1f) gpu=(%.1f,%.1f,%.1f) dxy=%.2f dz=%.2f\n",
				static_cast<int>(site.size()), site.data(), s_gpMissionTag, mouseX, mouseY, ctx.glX, ctx.glY,
				cx, cy, cz, res.worldX, res.worldY, res.worldZ, dHoriz, dVert);
		}
	}
}
