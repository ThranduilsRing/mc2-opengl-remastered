// gos_validate.cpp - Validation mode: auto-run, telemetry, screenshot, JSON log
#include "gos_validate.h"
#include "gos_crashbundle.h"
#include "gos_screenshot.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <GL/glew.h>
#include "mc2fmt.h"

static ValidateConfig s_config = {};
static ValidateTelemetry s_telemetry = {};

ValidateConfig& getValidateConfig() { return s_config; }
ValidateTelemetry& getValidateTelemetry() { return s_telemetry; }

void validateParseArgs(int argc, char** argv) {
    memset(&s_config, 0, sizeof(s_config));
    s_config.maxFrames = 60;
    s_config.bloomOverride = -1;
    s_config.shadowsOverride = -1;
    s_config.fxaaOverride = -1;
    s_config.grassOverride = -1;
    strncpy(s_config.logPath, "validate.json", sizeof(s_config.logPath) - 1);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--validate") == 0) {
            s_config.enabled = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            s_config.maxFrames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            strncpy(s_config.screenshotPath, argv[++i], sizeof(s_config.screenshotPath) - 1);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            strncpy(s_config.logPath, argv[++i], sizeof(s_config.logPath) - 1);
        } else if (strcmp(argv[i], "--enable") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bloom") == 0) s_config.bloomOverride = 1;
            else if (strcmp(argv[i], "shadows") == 0) s_config.shadowsOverride = 1;
            else if (strcmp(argv[i], "fxaa") == 0) s_config.fxaaOverride = 1;
            else if (strcmp(argv[i], "grass") == 0) s_config.grassOverride = 1;
        } else if (strcmp(argv[i], "--disable") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bloom") == 0) s_config.bloomOverride = 0;
            else if (strcmp(argv[i], "shadows") == 0) s_config.shadowsOverride = 0;
            else if (strcmp(argv[i], "fxaa") == 0) s_config.fxaaOverride = 0;
            else if (strcmp(argv[i], "grass") == 0) s_config.grassOverride = 0;
        }
    }

    if (s_config.enabled) {
        mc2::fmt::print_err("VALIDATE: mode enabled, {:d} frames, log={}\n", mc2::fmt::as_i32(s_config.maxFrames), mc2::fmt::safe(s_config.logPath));
    }
}

void validateRecordShaderError(const char* msg) {
    if (s_config.enabled) {
        s_telemetry.shaderErrors.push_back(msg);
        s_telemetry.exitCode = 1;
    }
}

void validateRecordFrame(float frameMs) {
    s_telemetry.framesRendered++;
    s_telemetry.totalFrameMs += frameMs;
    if (frameMs > s_telemetry.maxFrameMs)
        s_telemetry.maxFrameMs = frameMs;
}

bool validateShouldExit() {
    return s_config.enabled && s_telemetry.framesRendered >= s_config.maxFrames;
}

// Escape a string for JSON output (handles backslashes and quotes)
static void writeJsonString(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') mc2::fmt::print(f, "\\\"");
        else if (*s == '\\') mc2::fmt::print(f, "\\\\");
        else if (*s == '\n') mc2::fmt::print(f, "\\n");
        else if (*s == '\r') mc2::fmt::print(f, "\\r");
        else fputc(*s, f);
    }
    fputc('"', f);
}

void validateWriteResults(int viewportW, int viewportH) {
    static bool s_resultsWritten = false;
    if (!s_config.enabled || s_resultsWritten) return;
    s_resultsWritten = true;

    if (s_config.screenshotPath[0]) {
        gos::screenshot::writeTGA(s_config.screenshotPath, viewportW, viewportH);
    }

    // Drain any remaining GL errors
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        char buf[64];
        mc2::fmt::to_buf(buf, "GL_ERROR: 0x{:04X}", mc2::fmt::as_u32(err));
        s_telemetry.glErrors.push_back(buf);
        s_telemetry.exitCode = 1;
    }

    float avgMs = s_telemetry.framesRendered > 0
        ? s_telemetry.totalFrameMs / s_telemetry.framesRendered : 0.0f;

    FILE* f = fopen(s_config.logPath, "w");
    if (!f) {
        mc2::fmt::print_err("VALIDATE: Failed to write log to {}\n", mc2::fmt::safe(s_config.logPath));
        return;
    }

    mc2::fmt::print(f, "{{\n");
    mc2::fmt::print(f, "  \"frames\": {:d},\n", mc2::fmt::as_i32(s_telemetry.framesRendered));
    mc2::fmt::print(f, "  \"avg_frame_ms\": {:.1f},\n", avgMs);
    mc2::fmt::print(f, "  \"max_frame_ms\": {:.1f},\n", s_telemetry.maxFrameMs);

    mc2::fmt::print(f, "  \"gl_errors\": [");
    for (size_t i = 0; i < s_telemetry.glErrors.size(); i++) {
        if (i > 0) mc2::fmt::print(f, ", ");
        writeJsonString(f, s_telemetry.glErrors[i].c_str());
    }
    mc2::fmt::print(f, "],\n");

    mc2::fmt::print(f, "  \"shader_errors\": [");
    for (size_t i = 0; i < s_telemetry.shaderErrors.size(); i++) {
        if (i > 0) mc2::fmt::print(f, ", ");
        writeJsonString(f, s_telemetry.shaderErrors[i].c_str());
    }
    mc2::fmt::print(f, "],\n");

    if (s_config.screenshotPath[0]) {
        mc2::fmt::print(f, "  \"screenshot\": ");
        writeJsonString(f, s_config.screenshotPath);
        mc2::fmt::print(f, ",\n");
    }

    mc2::fmt::print(f, "  \"exit_code\": {:d}\n", mc2::fmt::as_i32(s_telemetry.exitCode));
    mc2::fmt::print(f, "}}\n");
    fclose(f);

    mc2::fmt::print_err("VALIDATE: Results written to {} (exit_code={:d}, {:d} frames, {:.1f}ms avg)\n", mc2::fmt::safe(s_config.logPath), mc2::fmt::as_i32(s_telemetry.exitCode), mc2::fmt::as_i32(s_telemetry.framesRendered), avgMs);
}

// -- Tier-1 instrumentation: GL error drain (stability spec §4) -------------

#include <string.h>
#include "Gates/gate.h"

static const bool s_glErrorDrainSilent = mc2::gate::get<"MC2_GL_ERROR_DRAIN_SILENT">();

// Seven known pass names (spec §4.1). Keep in the same order as the pass table.
enum GlDrainPass { GLP_SHADOW_STATIC = 0, GLP_SHADOW_DYNAMIC, GLP_TERRAIN, GLP_OBJECTS_3D, GLP_POST_PROCESS, GLP_HUD, GLP_FRAME, GLP_COUNT };

struct GlPassState {
    const char*  name;
    uint64_t     monoCount;         // monotonic total since process start
    uint32_t     lastPrintFrame;    // frame of most recent first-error print
    uint32_t     suppressedCount;   // errors accumulated during suppression window
    bool         inSuppression;
};

static GlPassState s_glPassState[GLP_COUNT] = {
    {"shadow_static",  0, 0, 0, false},
    {"shadow_dynamic", 0, 0, 0, false},
    {"terrain",        0, 0, 0, false},
    {"objects_3d",     0, 0, 0, false},
    {"post_process",   0, 0, 0, false},
    {"hud",            0, 0, 0, false},
    {"frame",          0, 0, 0, false},
};

extern uint32_t g_mc2FrameCounter;  // defined in mclib/tgl.cpp; incremented by gameosmain.cpp frame-end.

static int passIndex(const char* name) {
    for (int i = 0; i < GLP_COUNT; i++) {
        if (strcmp(name, s_glPassState[i].name) == 0) return i;
    }
    return -1;
}

static const char* glErrorName(GLenum e) {
    switch (e) {
        case GL_INVALID_ENUM:                  return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:                 return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION:             return "GL_INVALID_OPERATION";
        case GL_STACK_OVERFLOW:                return "GL_STACK_OVERFLOW";
        case GL_STACK_UNDERFLOW:               return "GL_STACK_UNDERFLOW";
        case GL_OUT_OF_MEMORY:                 return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
#ifdef GL_CONTEXT_LOST
        case GL_CONTEXT_LOST:                  return "GL_CONTEXT_LOST";
#endif
        default:                               return "UNKNOWN";
    }
}

void drainGLErrors(const char* pass) {
    int pi = passIndex(pass);
    if (pi < 0) return;  // unknown pass name — drop silently rather than crash
    GlPassState& st = s_glPassState[pi];

    uint32_t errorsThisFrame = 0;
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        errorsThisFrame++;
        st.monoCount++;

        // Print only the first error per (pass, frame), and only outside
        // the 30-frame suppression window.
        bool shouldPrint =
               !s_glErrorDrainSilent
            && errorsThisFrame == 1
            && (g_mc2FrameCounter - st.lastPrintFrame) >= 30;

        if (shouldPrint) {
            // If we're exiting a suppression window, emit a summary line first.
            if (st.inSuppression && st.suppressedCount > 0) {
                char _cbbuf[256];
                mc2::fmt::to_buf(_cbbuf, "[GL_ERROR v1] pass={} suppressed elapsed_frames={:d} count_in_window={:d}", mc2::fmt::safe(st.name), mc2::fmt::as_u32((unsigned)(g_mc2FrameCounter - st.lastPrintFrame)), mc2::fmt::as_u32((unsigned)st.suppressedCount));
                puts(_cbbuf);
                crashbundle_append(_cbbuf);
                fflush(stdout);
            }
            st.inSuppression   = false;
            st.suppressedCount = 0;

            {
                char _cbbuf[256];
                mc2::fmt::to_buf(_cbbuf, "[GL_ERROR v1] frame={:d} pass={} code={}(0x{:04X}) mono_count={:d}", mc2::fmt::as_u32(g_mc2FrameCounter), mc2::fmt::safe(st.name), mc2::fmt::safe(glErrorName(err)), mc2::fmt::as_u32((unsigned)err), mc2::fmt::as_u64((unsigned long long)st.monoCount));
                puts(_cbbuf);
                crashbundle_append(_cbbuf);
            }
            fflush(stdout);
            st.lastPrintFrame = g_mc2FrameCounter;
        } else if (!s_glErrorDrainSilent && errorsThisFrame == 1) {
            // First error this frame, but still in suppression window.
            st.inSuppression = true;
            st.suppressedCount++;
        }
    }
}
