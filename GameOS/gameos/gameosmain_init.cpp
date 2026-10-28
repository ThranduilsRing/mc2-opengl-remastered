// main: the start-up helpers -- DPI awareness, the launcher hand-off and launcher_env.json, the instrumentation banner,
// the pre-window resolution and the Vulkan backend bring-up. The orchestrator is in gameosmain.cpp.
#include "gameosmain_internal.h"
#include "mc2fmt.h"
#include "../../GuiRuntime/PlayerFrontEnd.h"
#include "Gates/gate_cold.h"   // str<>()
#include "Gates/render_options_publish.h"


// Opt the process into per-monitor DPI awareness unless MC2_DPI_UNAWARE is set.
void initDpiAwareness() {
#ifdef PLATFORM_WINDOWS
    // DPI-AWARE-1: FIRST statement of the process — the per-process DPI
    // context is locked by the first window ANY subsystem creates (the VK
    // swapchain probe's hidden SDL window boots video before
    // graphics::create_window), so this must precede everything. See the
    // gos_render.cpp create_window comment for the field evidence (4K/225%
    // desktop => 2x-desktop window, "zoomed" menus). MC2_DPI_UNAWARE=1 =
    // legacy virtualized behavior.
    {
        if (!mc2::gate::get<"MC2_DPI_UNAWARE">()) {
            using SetDpiCtxFn = BOOL (WINAPI*)(void*);
            HMODULE u32 = LoadLibraryA("user32.dll");
            SetDpiCtxFn setCtx = u32
                ? reinterpret_cast<SetDpiCtxFn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext")) : nullptr;
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            if (!setCtx || !setCtx(reinterpret_cast<void*>(-4)))
                SetProcessDPIAware();
        }
    }
#endif
}

// Load the player settings before any renderer or campaign data.
void initLauncherEnv() { PlayerFrontEnd::loadSettings(); }


// Read the instrumentation gates, initialise the object recon and emit the [INSTR v1] banner.
void initInstrBanner() {
    {
        const bool destr   = mc2::gate::get<"MC2_DESTROY_TRACE">();
        const bool smoke   = mc2::gate::get<"MC2_SMOKE_MODE">();
        const bool waterFp = mc2::gate::get<"MC2_RENDER_WATER_FASTPATH">();
        const bool waterPc = mc2::gate::get<"MC2_RENDER_WATER_PARITY_CHECK">();
        const bool vpFast  = mc2::gate::get<"MC2_VERTEX_PROJECT_FAST">();
        const bool vpPar   = mc2::gate::get<"MC2_VERTEX_PROJECT_PARITY">();
        // [OBJECT_RECON v1] read MC2_OBJECT_RECON_TRACY here so the gate is
        // live before any update kernel runs. drainPerFrame() lazy-inits
        // too, but eager init avoids missing the very-first-frame data.
        mc2_object_recon::initFromEnv();
        const bool objRecon = mc2_object_recon::g_enabled;
        const bool tInd    = gos_terrain_indirect::IsEnabled();
        const bool tIndP   = gos_terrain_indirect::IsParityCheckEnabled();
        const bool tIndM   = gos_terrain_indirect::IsMineEnabled();   // PR2c
        const bool tIndO   = gos_terrain_indirect::IsOverlayEnabled();  // PR2b
        const bool tIndOP  = gos_terrain_indirect::IsOverlayParityCheckEnabled();  // PR2b
        // Std rule ("0"/"false"/"off"/"no" are false, anything else true) is the ParseEnvBool the terrobj.cpp
        // reader uses, so the banner reflects the actual gate state.
        const bool suTrace = mc2::gate::get<"MC2_STATIC_UPDATE_TRACE">();
        // Default ON; MC2_GPU_CULL_SUBSTRATE=0 opts out.
        bool gpuCullSubstrate = mc2::gate::get<"MC2_GPU_CULL_SUBSTRATE">();
        bool gpuCullParity    = mc2::gate::get<"MC2_GPU_CULL_AABB_PARITY">();
        const bool shrHR      = mc2::gate::get<"MC2_SHADER_HOT_RELOAD">();
        const bool revZ       = mc2::gate::get<"MC2_REVERSE_Z_TRACE">();
        // T3.1: (E) SpotLight_ -> real illumination. The
        // new TG_Light registration path is the unconditional production
        // behavior. MC2_SPOTLIGHT_REAL_TRACE counters remain (Debug
        // Instrumentation Rule: demote, don't delete) for future diagnostics.
        const char* build  = MC2_BUILD_GIT_SHA;
        // Grew 384 -> 512 -> 640 to absorb terrain_indirect{,_parity}
        // and gpu_objects fields without truncation.
        // Grew 640 -> 720 to absorb gpu_cull_substrate and gpu_cull_aabb_parity.
        // Grew 720 -> 768 to absorb terrain_indirect_mine (PR2c Stage 0c).
        // Grew 768 -> 832 to absorb terrain_indirect_overlay{,_parity} (PR2b).
        // Grew 832 -> 896 to absorb reverse_z_trace (reverse-Z float depth).
        // Grew 960 -> 1024 to absorb spotlight_real (T1.2, (E) SpotLight_ -> real).
        // Shrank back to 960 effective use post-T3.1 (spotlight_real field
        // removed); buffer kept at 1024 for future absorbers.
        // (water_skip_env field was tentatively added during the closed
        // water-projection-skip slice attempt; removed when the slice
        // closed — premise invalidated by Stage 0 M3 audit.)
        char _cbbuf[1024];
        mc2::fmt::to_buf(_cbbuf, "[INSTR v1] enabled: destroy={:d} smoke={:d} water_fp={:d} water_parity={:d} vp_fast={:d} vp_parity={:d} terrain_indirect={:d} terrain_indirect_parity={:d} terrain_indirect_mine={:d} terrain_indirect_overlay={:d} terrain_indirect_overlay_parity={:d} gpu_objects={:d} obj_recon_tracy={:d} static_update_trace={:d} static_prop_registry={:d} gpu_cull_substrate={:d} gpu_cull_aabb_parity={:d} shader_hot_reload={:d} reverse_z_trace={:d} camera_motion=1 build={}", mc2::fmt::as_i32(destr ? 1 : 0), mc2::fmt::as_i32(smoke ? 1 : 0), mc2::fmt::as_i32(waterFp ? 1 : 0), mc2::fmt::as_i32(waterPc ? 1 : 0), mc2::fmt::as_i32(vpFast ? 1 : 0), mc2::fmt::as_i32(vpPar ? 1 : 0), mc2::fmt::as_i32(tInd ? 1 : 0), mc2::fmt::as_i32(tIndP ? 1 : 0), mc2::fmt::as_i32(tIndM ? 1 : 0), mc2::fmt::as_i32(tIndO ? 1 : 0), mc2::fmt::as_i32(tIndOP ? 1 : 0), mc2::fmt::as_i32(1), mc2::fmt::as_i32(objRecon ? 1 : 0), mc2::fmt::as_i32(suTrace ? 1 : 0), mc2::fmt::as_i32(1), mc2::fmt::as_i32(gpuCullSubstrate ? 1 : 0), mc2::fmt::as_i32(gpuCullParity ? 1 : 0), mc2::fmt::as_i32(shrHR ? 1 : 0), mc2::fmt::as_i32(revZ ? 1 : 0), mc2::fmt::safe(build));
        puts(_cbbuf);
        crashbundle_append(_cbbuf);

        // [MATERIAL_GPU v4] startup banner — separate from [INSTR v1] to keep
        // that buffer size stable. Duplicates the getenv check because
        // s_materialGpuEnabled is a private file-scope static in the batcher
        // (not accessible cross-TU — Option A from MaterialGpu-2 spec §3).
        // v5: MC2_MATERIAL_GPU defaults ON; set to "0" to disable.
        {
            const bool matGpuOn = mc2::gate::get<"MC2_MATERIAL_GPU">();
            const bool matSampleOn = mc2::gate::get<"MC2_MATERIAL_GPU_SAMPLE">();
            char buf[96];
            mc2::fmt::to_buf(buf, "[MATERIAL_GPU v4] enabled={:d} sample={:d} binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\n", mc2::fmt::as_i32(static_cast<int>(matGpuOn)), mc2::fmt::as_i32(static_cast<int>(matSampleOn)));
            MC2_DIAG_FPUTS(buf, stderr);
        }

        // GPU cull record schema selftest.
        {
            int gcFail = gpu_cull::gpu_cull_record_selftest();
            if (gcFail > 0) {
                MC2_DIAGFE(stderr, "[GPU_CULL v1] FATAL: selftest failed (%d); "
                                "GpuActorRecord layout corrupt, see log above.\n", gcFail);
                fflush(stderr);
                gosASSERT(false);
                abort();
            }
        }

        // C2: readback ring buffer three-tier selftest is called from within
        // readback_init() (at mission load) — not here. readback_init() requires GL
        // context + a valid maxActors count, so it cannot run at engine startup.
        // The selftest emits [GPU_CULL v1] event=readback_selftest pass=3 fail=0
        // to the mission log when MC2_GPU_CULL_READBACK=1 is set.
        // (No call needed here — readback_selftest() in readback.cpp is self-contained.)

        if (mc2::gate::get<"MC2_STATIC_PROP_BAKE_SELFTEST">()) {
            // Stub: full self-test wired in Task 5 after buildRecipeFromShape
            // has callers and a registered type to test against.
            mc2::fmt::print_err("[STATIC_PROP_BAKE v1] event=selftest_stub note=full_test_wired_task5\n");
            fflush(stderr);
        }

        if (g_pzTrace) {
            char _pzbuf[256];
            mc2::fmt::to_buf(_pzbuf, "[PROJECTZ v1] enabled: trace={:d} heatmap={:d} summary={:d} guard_px={:d}", mc2::fmt::as_i32(g_pzDoTrace ? 1 : 0), mc2::fmt::as_i32(g_pzDoHeatmap ? 1 : 0), mc2::fmt::as_i32(g_pzDoSummary ? 1 : 0), mc2::fmt::as_i32(g_pzGuardPx));
            puts(_pzbuf);
        }
    }
}

// Publish the render options (options.cfg, then the MC2_OPTIONS_OVERRIDE test channel) before the window and the renderer exist: the
// shader-define and pipeline readers run during renderer start-up, long before CPrefs loads the file. CPrefs::load() reports the file's rejected
// entries and CPrefs::applyPrefs() publishes the validated values again and reports the override's, so this early pass logs nothing.
void initRenderOptionsFromConfig() {
    (void)mc2::options::publishFromInstall();
}

// Pick the pre-window resolution and fullscreen state from the environment or options.cfg.
void initResolutionFromConfig() {
    // Pre-window resolution bootstrap (ui-phase1: supersedes [HUD-RES-CLAMP v1]).
    //
    // GetGameOSEnvironment (mechcmd2.cpp) hardcodes 800x600.  prefs.load()
    // + gos_SetScreenMode run inside InitializeGameEngine — after the window
    // is already created — so the window and all UI coordinate math start
    // with the wrong dimensions.
    //
    // Fix: read ResolutionX/Y (and FullScreen) from options.cfg here, before
    // create_window, so the window is sized correctly from the first frame.
    // Uses a simple line scanner to avoid pulling in the full FitIniFile stack
    // before the GL context exists.  Falls back to 800x600 if the file is
    // absent or the keys are missing.
    //
    // Env var overrides (highest priority, useful for CI/smoke/debugging):
    //   MC2_WIDTH=<px>   MC2_HEIGHT=<px>
    //
    // The old [HUD-RES-CLAMP v1] hardcoded 800x600 unconditionally to keep the
    // legacy 2D HUD's layout correct; that was a stopgap for the pre-ImGui HUD
    // and is retired now that the defs/ImGui UI drives real-resolution layout.
    {
        const bool envW = mc2::gate::isSet<"MC2_WIDTH">();
        const bool envH = mc2::gate::isSet<"MC2_HEIGHT">();
        if (envW && mc2::gate::get<"MC2_WIDTH">() > 0)
            Environment.screenWidth = mc2::gate::get<"MC2_WIDTH">();
        if (envH && mc2::gate::get<"MC2_HEIGHT">() > 0)
            Environment.screenHeight = mc2::gate::get<"MC2_HEIGHT">();

        // If no env override, try options.cfg.
        if (!envW && !envH)
        {
            FILE* f = std::fopen("options.cfg", "r");
            if (f)
            {
                char line[256];
                bool inBlock = false;
                int foundW = 0, foundH = 0;
                bool foundFS = false;
                bool fullscreenVal = false;

                while (std::fgets(line, sizeof(line), f))
                {
                    // Strip CR/LF
                    char* nl = std::strrchr(line, '\n');
                    if (nl) *nl = '\0';
                    nl = std::strrchr(line, '\r');
                    if (nl) *nl = '\0';

                    if (std::strstr(line, "[MechCommander2]"))
                    {
                        inBlock = true;
                        continue;
                    }
                    // Any new block ends the section we want
                    if (inBlock && line[0] == '[')
                        break;
                    if (!inBlock)
                        continue;

                    // Lines look like: "lg ResolutionX = 1920"
                    const char* eq = std::strchr(line, '=');
                    if (!eq)
                        continue;

                    char key[64] = {};
                    // Copy everything before '=' and find the last word (the key name)
                    std::ptrdiff_t beforeEq = eq - line;
                    if (beforeEq <= 0 || beforeEq >= 63)
                        continue;
                    char before[64] = {};
                    std::strncpy(before, line, static_cast<size_t>(beforeEq));

                    // The key is the last whitespace-delimited token before '='
                    char* tok = std::strtok(before, " \t");
                    char* lastTok = tok;
                    while (tok) { lastTok = tok; tok = std::strtok(nullptr, " \t"); }
                    if (!lastTok)
                        continue;

                    const int val = std::atoi(eq + 1);
                    if      (std::strcmp(lastTok, "ResolutionX") == 0 && val > 0) foundW = val;
                    else if (std::strcmp(lastTok, "ResolutionY") == 0 && val > 0) foundH = val;
                    else if (std::strcmp(lastTok, "FullScreen")  == 0)
                    {
                        foundFS = true;
                        // FitIniFile booleans: "1" or "true". UI-DEFS-LOCKSTEP-
                        // HARDEN-1 latent fix: the game itself writes
                        // "b FullScreen = TRUE" -- match case-insensitively, or
                        // a game-written cfg boots windowed (fullscreen=0).
                        bool sawTrue = false;
                        for (const char* s = eq + 1; *s && !sawTrue; ++s)
                            sawTrue = (s[0] == 't' || s[0] == 'T') &&
                                      (s[1] == 'r' || s[1] == 'R') &&
                                      (s[2] == 'u' || s[2] == 'U') &&
                                      (s[3] == 'e' || s[3] == 'E');
                        fullscreenVal = (val != 0 || sawTrue);
                    }
                }
                std::fclose(f);

                if (foundW > 0) Environment.screenWidth  = foundW;
                if (foundH > 0) Environment.screenHeight = foundH;
                if (foundFS)    Environment.fullScreen   = fullscreenVal ? 1 : 0;

                mc2::fmt::print_out("[RESOLUTION] Pre-window: {:d}x{:d} fullscreen={:d} (from options.cfg)\n", mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight), mc2::fmt::as_i32(Environment.fullScreen));
            }
            else
            {
                mc2::fmt::print_out("[RESOLUTION] Pre-window: {:d}x{:d} (options.cfg not found, using defaults)\n", mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight));
            }
        }
        else
        {
            mc2::fmt::print_out("[RESOLUTION] Pre-window: {:d}x{:d} (from MC2_WIDTH/MC2_HEIGHT env)\n", mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight));
        }
    }
}
