<!-- doc-status: current | reviewed:2026-10-11 -->
# Engine-Side Hooks Required by the Testing Strategy

Three short C++ patches that have to land in `GameOS/gameos/gameos_graphics.cpp`
to unblock Tiers 1.2, 3, and 5 of `docs/testing-strategy.md`. All three are
env-gated -- zero cost when the env var is unset, so they're safe to land
ahead of the harness scripts that consume them.

Conventions in all three sketches:
- Engine path separator is `"/"` (`-DLINUX_BUILD` global). Do not hardcode
  `\\` -- `memory/mc2_path_separator_linux_build.md`.
- All env-var reads happen ONCE at first use, cached in file-scope statics.
- No new dependencies for 1.2 and 3. Sketch C (RenderDoc) vendors
  `renderdoc_app.h` under `3rdparty/renderdoc/` (single header, ~600 lines,
  Apache 2.0).

---

## Sketch A -- Tier 1.2: `KHR_debug` fatal env-gate

Wiring: amend the existing `glDebugMessageCallback` callback (grep
`gameos_graphics.cpp` for `glDebugMessageCallback` and `GL_DEBUG_OUTPUT`).
Do NOT introduce a new callback; piggy-back on the existing one so the
log-routing stays uniform.

```cpp
// In the existing GL_KHR_debug callback in gameos_graphics.cpp:

static void GLAPIENTRY mc2_gl_debug_cb(
    GLenum source, GLenum type, GLuint id, GLenum severity,
    GLsizei length, const GLchar* message, const void* /*userParam*/)
{
    // ... existing message routing / log emit ...

    // NEW: env-gated fail-fast for smoke / CI.
    // detect_leaks-equivalent: HIGH severity only; MEDIUM/LOW are too
    // noisy on AMD (perf warnings on every TGL stream).
    static const bool s_fatal_high =
        std::getenv("MC2_GL_DEBUG_FATAL") != nullptr;
    if (s_fatal_high && severity == GL_DEBUG_SEVERITY_HIGH) {
        std::fprintf(stderr,
            "[MC2_GL_DEBUG_FATAL] severity=HIGH source=0x%x type=0x%x "
            "id=%u\n  %.*s\n",
            source, type, id, (int)length, message);
        std::fflush(stderr);
        std::abort();
    }
}
```

Usage from smoke:
```bash
MC2_GL_DEBUG_FATAL=1 py -3 scripts/run_smoke.py --tier tier1 --duration 30
```

Memories this catches: `blend_state_inheritance_in_post_process.md`,
`sampler_state_inheritance_in_fast_paths.md`,
`gpu_direct_depth_state_inheritance.md`. All three would have been
`GL_DEBUG_SEVERITY_HIGH` `GL_DEBUG_TYPE_ERROR` events on first repro.

---

## Sketch B -- Tier 3: golden-frame PNG dump  [SUPERSEDED]

**SUPERSEDED 2026-05-20** by the existing Stage 2.E visual-diff
infrastructure, which is more mature and was shipped 2026-05-04:

- Engine capture: `GameOS/gameos/gos_visual_diff.h` + `gos_visual_diff.cpp`
  (env-gated, framework-locked, uses `gos::screenshot::writeTGA`; no new
  vendor needed).
- Python harness: `tests/smoke/object_visual_diff.py` with
  `--measure-variance` (same-config repeat measurement) and `--gate`
  (PASS/FAIL tolerance).
- Env contract: `MC2_VISUAL_DIFF_CAPTURE=1`, `MC2_VISUAL_DIFF_MISSION`,
  `MC2_VISUAL_DIFF_OUT`, `MC2_VISUAL_DIFF_FRAME_N`,
  `MC2_VISUAL_DIFF_MAX_FRAMES`.
- Wired at the pre-HUD seam in `gameosmain.cpp` (between `pp->endScene()`
  and `projectz_overlay_render`); does NOT require a new SwapBuffers hook.

Do NOT re-introduce Sketch B's `GoldenDumpState` block or vendor
`stb_image_write`; both are redundant with `gos_visual_diff` + the
engine's existing TGA serializer. See `docs/testing-strategy.md` Tier 3
section for the current contract, including the empirical-variance gate.

---

## Sketch C -- Tier 5: in-process RenderDoc capture  [SHIPPED 2026-05-20]

**SHIPPED 2026-05-20.** Implemented at `GameOS/gameos/gos_rdoc_capture.{h,cpp}`
with a slightly tightened env-var prefix (`MC2_RDC_*` not the prototype's
`MC2_RENDERDOC_*`) and a phase machine that latches on
`gos_terrain_indirect::WasEverFrameSolidArmed()` to match Tier 3 visual-diff
semantics. The harness is `scripts/renderdoc_capture.py`. See
`docs/testing-strategy.md` Tier 5 section for the contract, including the
normalization rules and the empirical "diff-vs-self == 0" gate. The text
below is preserved as the design-stage record.


Vendor `renderdoc_app.h` at `3rdparty/renderdoc/renderdoc_app.h`. Single
header from <https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/api/app/renderdoc_app.h>,
no link-time dependency (RenderDoc itself is opened via `LoadLibrary`).

```cpp
#include "renderdoc_app.h"

namespace {
RENDERDOC_API_1_5_0* g_rdoc = nullptr;
int g_rdoc_capture_frame = -1;
int g_rdoc_frame_counter = 0;

void init_renderdoc_api() {
    if (g_rdoc) return;
    const char* f = std::getenv("MC2_RENDERDOC_CAPTURE_FRAME");
    if (!f) return;
    g_rdoc_capture_frame = std::atoi(f);

    // renderdoc.dll is loaded by RenderDoc itself when launching the app
    // under capture, OR can be LoadLibrary'd directly to enable
    // programmatic capture. Either way, GetModuleHandle finds it.
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) {
        // Try to load from default install path if not already present.
        mod = LoadLibraryA("renderdoc.dll");
    }
    if (!mod) {
        std::fprintf(stderr,
            "[RENDERDOC] renderdoc.dll not found; in-process capture "
            "disabled. Install RenderDoc or pre-inject.\n");
        return;
    }
    auto get_api = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
    if (!get_api ||
        get_api(eRENDERDOC_API_Version_1_5_0, (void**)&g_rdoc) != 1) {
        g_rdoc = nullptr;
        return;
    }
    if (const char* out = std::getenv("MC2_RENDERDOC_OUT"))
        g_rdoc->SetCaptureFilePathTemplate(out);   // path WITHOUT ".rdc"
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 1);
}

// Called at end of frame, AFTER glFinish, BEFORE SwapBuffers
// (matches B's call site -- they can sit next to each other).
void try_renderdoc_capture_this_frame() {
    if (!g_rdoc || g_rdoc_capture_frame < 0) return;
    const int n = g_rdoc_frame_counter++;
    if (n + 1 == g_rdoc_capture_frame) {
        g_rdoc->StartFrameCapture(nullptr, nullptr);
    } else if (n == g_rdoc_capture_frame) {
        g_rdoc->EndFrameCapture(nullptr, nullptr);
        if (std::getenv("MC2_RENDERDOC_EXIT_AFTER")) std::exit(0);
    }
}
}  // anon
```

Driving it:
```
MC2_RENDERDOC_CAPTURE_FRAME=120
MC2_RENDERDOC_OUT=tests/smoke/artifacts/<ts>/mc2_01/capture
MC2_RENDERDOC_EXIT_AFTER=1
```

`renderdoc.dll` must be findable. Two options:

1. Launch `mc2.exe` from RenderDoc UI / `renderdoccmd capture`. RenderDoc
   injects itself; `GetModuleHandle` succeeds immediately.
2. Pre-load via `LoadLibrary("renderdoc.dll")` (path on `PATH` if
   RenderDoc installed normally; otherwise absolute path).

`scripts/renderdoc_capture.py` is the orchestrator that does (1) -- it
prefers `renderdoccmd capture`, then converts the resulting `.rdc` to
pipeline-state JSON, then diffs against the baseline.

### renderdoccmd CLI surface used by Tier 5

```
renderdoccmd capture <app.exe> [args...] -w <waitForExit>  # produce .rdc
renderdoccmd convert -i in.rdc -o out.json -f raw-json     # produce JSON
```

(Exact flag names should be verified against `renderdoccmd help` --
the convert format key has moved between RenderDoc minor versions; the
script normalizes.)
