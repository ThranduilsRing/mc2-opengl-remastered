// gos_rdoc_capture.cpp - Tier 5 in-process RenderDoc capture hook.
// See gos_rdoc_capture.h for the contract.
#include "gos_rdoc_capture.h"
#include "gos_terrain_indirect.h"  // WasEverFrameSolidArmed()
#include "gos_smoke.h"             // SmokeMode::state().mission

#include "../../3rdparty/renderdoc/renderdoc_app.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace RdocCapture {

namespace {

enum class Phase { Waiting, Armed, Triggered, Done };

struct State {
    bool                  readEnv             = false;
    bool                  enabled             = false;
    bool                  apiResolved         = false;
    RENDERDOC_API_1_5_0*  api                 = nullptr;
    int                   captureFrame        = -1;  // frames after intro-complete
    bool                  exitAfter           = false;
    int                   localFrame          = 0;
    int                   introCompleteFrame  = -1;
    bool                  introObserved       = false;
    Phase                 phase               = Phase::Waiting;
    int                   triggeredAt         = -1;
    std::string           capturePath;                 // expanded, validated
};
State s_state;

// DISK-GUARD-3 (2026-07-29): MC2_RDC_CAPTURE_PATH used to be handed to
// SetCaptureFilePathTemplate VERBATIM. That is how a 1,010,693,435-byte
// capture ended up on disk named
//
//     vk_airport_{frame}_frame41.rdc
//
// -- a literal, unexpanded "{frame}" in the filename of a ~1 GB file, which
// nobody could later attribute to a mission or a frame. scripts/
// renderdoc_capture.py was taught to expand its own --out, but that fix only
// covers callers that go THROUGH the script; the offending capture came from
// an ad-hoc invocation that set MC2_RDC_CAPTURE_PATH directly and bypassed it
// entirely. This function is the real chokepoint: every caller, script or
// ad-hoc, passes through here.
//
// Contract:
//   {mission} -> --mission stem (SmokeMode), or $MC2_RDC_MISSION
//   {frame}   -> $MC2_RDC_CAPTURE_FRAME (s_state.captureFrame)
//   any surviving '{' or '}' -> FATAL. A mis-named gigabyte is worse than no
//   capture at all, and silently proceeding is precisely the failure this
//   lane exists to remove. Refusing to set the template is NOT an option
//   either: RenderDoc would then fall back to its own default path, which is
//   just a different flavour of "the file is not where you told it to be".
std::string expandCaptureTemplate(const char* raw) {
    std::string out(raw);

    auto replaceAll = [&out](const char* token, const std::string& value) {
        const std::string tok(token);
        for (std::string::size_type p = out.find(tok);
             p != std::string::npos;
             p = out.find(tok, p + value.size())) {
            out.replace(p, tok.size(), value);
        }
    };

    std::string mission = SmokeMode::state().mission;
    if (mission.empty()) {
        mission = mc2::gate::str<"MC2_RDC_MISSION">();   // "" when unset or empty
    }
    if (!mission.empty()) replaceAll("{mission}", mission);

    char frameBuf[32];
    mc2::fmt::to_buf(frameBuf, "{:d}", mc2::fmt::as_i32(s_state.captureFrame));
    replaceAll("{frame}", frameBuf);

    if (out.find('{') != std::string::npos ||
        out.find('}') != std::string::npos) {
        mc2::fmt::print_err("[RDOC v1] event=capture_path_unexpanded FATAL\n[RDOC v1]   MC2_RDC_CAPTURE_PATH='{}'\n[RDOC v1]   after expansion    ='{}'\n[RDOC v1]   unexpanded placeholder(s) remain. Supported tokens are\n[RDOC v1]   {{mission}} and {{frame}}; {{mission}} additionally needs\n[RDOC v1]   --mission <stem> (smoke mode) or MC2_RDC_MISSION set.\n[RDOC v1]   Refusing to write a ~1 GB capture under a name that\n[RDOC v1]   cannot be attributed to a mission or a frame.\n", mc2::fmt::safe(raw), out.c_str());
        std::fflush(stderr);
        std::exit(5);
    }
    return out;
}

void readEnvOnce() {
    if (s_state.readEnv) return;
    s_state.readEnv = true;
    if (!mc2::gate::isNonEmpty<"MC2_RDC_CAPTURE_FRAME">()) { s_state.enabled = false; return; }
    int n = mc2::gate::get<"MC2_RDC_CAPTURE_FRAME">();
    if (n < 0) { s_state.enabled = false; return; }
    s_state.enabled      = true;
    s_state.captureFrame = n;
    s_state.exitAfter    = mc2::gate::get<"MC2_RDC_EXIT_AFTER">();

    // Expand and VALIDATE the path template here, at arm time -- deliberately
    // NOT down in resolveApiOnce(). resolveApiOnce() early-returns whenever
    // renderdoc.dll is absent, so validating there would make a malformed
    // MC2_RDC_CAPTURE_PATH surface only on the runs that were about to write
    // a gigabyte. Checking at arm time means the bad template is rejected
    // before a single frame is rendered, and it is reachable (and therefore
    // testable) without RenderDoc attached at all.
    {
        const std::string tpl = mc2::gate::str<"MC2_RDC_CAPTURE_PATH">();   // "" when unset or empty
        if (!tpl.empty()) s_state.capturePath = expandCaptureTemplate(tpl.c_str());
    }
}

void resolveApiOnce() {
    if (s_state.apiResolved) return;
    s_state.apiResolved = true;

#if defined(_WIN32)
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) {
        // Fallback: try to load by name; succeeds if renderdoc.dll is on PATH.
        mod = LoadLibraryA("renderdoc.dll");
    }
    if (!mod) {
        mc2::fmt::print_err("[RDOC v1] event=api_unavailable reason=renderdoc_dll_not_found hint=launch_under_renderdoccmd_capture\n");
        std::fflush(stderr);
        return;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(mod, "RENDERDOC_GetAPI"));
#else
    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!mod) mod = dlopen("librenderdoc.so", RTLD_NOW);
    if (!mod) {
        mc2::fmt::print_err("[RDOC v1] event=api_unavailable reason=librenderdoc_not_found\n");
        std::fflush(stderr);
        return;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        dlsym(mod, "RENDERDOC_GetAPI"));
#endif
    if (!get_api) {
        mc2::fmt::print_err("[RDOC v1] event=api_unavailable reason=GetAPI_missing\n");
        std::fflush(stderr);
        return;
    }
    void* p = nullptr;
    int ok = get_api(eRENDERDOC_API_Version_1_5_0, &p);
    if (ok != 1 || !p) {
        mc2::fmt::print_err("[RDOC v1] event=api_unavailable reason=GetAPI_returned_{:d}\n", mc2::fmt::as_i32(ok));
        std::fflush(stderr);
        return;
    }
    s_state.api = reinterpret_cast<RENDERDOC_API_1_5_0*>(p);

    // Already expanded + validated in readEnvOnce(); empty means the caller
    // set no template and RenderDoc's own default applies.
    if (!s_state.capturePath.empty()) {
        s_state.api->SetCaptureFilePathTemplate(s_state.capturePath.c_str());
        mc2::fmt::print_err("[RDOC v1] event=capture_path template='{}'\n", s_state.capturePath.c_str());
        std::fflush(stderr);
    }

    int major = 0, minor = 0, patch = 0;
    s_state.api->GetAPIVersion(&major, &minor, &patch);
    mc2::fmt::print_err("[RDOC v1] event=api_resolved version={:d}.{:d}.{:d} capture_frame={:d} exit_after={:d}\n", mc2::fmt::as_i32(major), mc2::fmt::as_i32(minor), mc2::fmt::as_i32(patch), mc2::fmt::as_i32(s_state.captureFrame), mc2::fmt::as_i32(s_state.exitAfter ? 1 : 0));
    std::fflush(stderr);
}

}  // namespace

bool isEnabled() {
    readEnvOnce();
    return s_state.enabled;
}

void onFrameTick() {
    readEnvOnce();
    if (!s_state.enabled) return;
    resolveApiOnce();
    if (!s_state.api) return;

    // Latch on the first frame that the engine reports intro-pan complete,
    // matching VisualDiff semantics so the captured EID layout is comparable.
    if (!s_state.introObserved) {
        if (gos_terrain_indirect::WasEverFrameSolidArmed()) {
            s_state.introObserved      = true;
            s_state.introCompleteFrame = s_state.localFrame;
            s_state.phase              = Phase::Armed;
            mc2::fmt::print_err("[RDOC v1] event=intro_armed local_frame={:d}\n", mc2::fmt::as_i32(s_state.localFrame));
            std::fflush(stderr);
        }
        ++s_state.localFrame;
        return;
    }

    const int sinceArm = s_state.localFrame - s_state.introCompleteFrame;

    if (s_state.phase == Phase::Armed && sinceArm == s_state.captureFrame) {
        // TriggerCapture marks the NEXT presented frame for capture. The
        // engine has not yet swapped (this hook is pre-SwapBuffers), so the
        // capture covers the frame whose hook we are inside now.
        s_state.api->TriggerCapture();
        s_state.phase       = Phase::Triggered;
        s_state.triggeredAt = s_state.localFrame;
        mc2::fmt::print_err("[RDOC v1] event=trigger_capture local_frame={:d} since_arm={:d}\n", mc2::fmt::as_i32(s_state.localFrame), mc2::fmt::as_i32(sinceArm));
        std::fflush(stderr);
    } else if (s_state.phase == Phase::Triggered &&
               s_state.localFrame >= s_state.triggeredAt + 2) {
        // Give RenderDoc one full present cycle to finalize the .rdc before
        // we exit. Two frames is conservative; one is the captured present,
        // the next ensures EndFrameCapture-equivalent state is flushed.
        std::fprintf(stderr,
            "[RDOC v1] event=capture_complete local_frame=%d\n",
            s_state.localFrame);
        std::fflush(stderr);
        s_state.phase = Phase::Done;
        if (s_state.exitAfter) {
            std::exit(0);
        }
    } else if (s_state.phase == Phase::Armed &&
               sinceArm > s_state.captureFrame + mc2::gate::get<"MC2_RDC_MAX_WAIT">()) {
        mc2::fmt::print_err("[RDOC v1] event=capture_timeout local_frame={:d}\n", mc2::fmt::as_i32(s_state.localFrame));
        std::fflush(stderr);
        s_state.phase = Phase::Done;
        if (s_state.exitAfter) std::exit(4);
    }

    ++s_state.localFrame;
}

}  // namespace RdocCapture
