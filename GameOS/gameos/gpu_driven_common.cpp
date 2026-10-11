// gpu_driven_common.cpp — Phase C GPU-driven rendering: shared infrastructure.
//
// Env-var cache helpers for all Phase C tasks (1.2-1.6, Stages 2-3).
// Each getter uses a static const bool local (lambda-init pattern from
// gos_terrain_lighting.cpp IsEnabled()), so the getenv() call is paid
// exactly once at process start.
//
// Killswitch summary:
//   MC2_GPU_DRIVEN=0           -- disable all gpu_driven paths (default ON)
//   MC2_GPU_DRIVEN_PARITY=1    -- enable parity checks (default OFF)
//   MC2_GPU_DRIVEN_TRACE=1     -- enable trace logging (default OFF)
//   MC2_GPU_DRIVEN_WATER=0     -- disable water fast-path (default ON, gated by global)
//   MC2_GPU_DRIVEN_TERRAIN_SOLID=0 -- disable terrain solid fast-path (default ON, gated by global)
//   MC2_GPU_DRIVEN_OVERLAY=0   -- disable overlay fast-path (default ON, gated by global)

#include "gpu_driven_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <GL/glew.h>

#include "utils/shader_builder.h"  // load_shader — engine #include resolver
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace gpu_driven {

bool IsGlobalEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_GPU_DRIVEN">();
    return s_enabled;
}

bool IsParityEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_GPU_DRIVEN_PARITY">();
    return s_enabled;
}

bool IsTraceEnabled() {
    static const bool s_enabled = mc2::gate::get<"MC2_GPU_DRIVEN_TRACE">();
    return s_enabled;
}

bool IsWaterEnabled() {
    static const bool s_enabled = [] {
        if (!IsGlobalEnabled()) return false;
        return mc2::gate::get<"MC2_GPU_DRIVEN_WATER">();
    }();
    return s_enabled;
}

bool IsTerrainSolidEnabled() {
    static const bool s_enabled = [] {
        if (!IsGlobalEnabled()) return false;
        return mc2::gate::get<"MC2_GPU_DRIVEN_TERRAIN_SOLID">();
    }();
    return s_enabled;
}

bool IsOverlayEnabled() {
    static const bool s_enabled = [] {
        if (!IsGlobalEnabled()) return false;
        return mc2::gate::get<"MC2_GPU_DRIVEN_OVERLAY">();
    }();
    return s_enabled;
}

// ---------------------------------------------------------------------------
// Shared compute program builder
// ---------------------------------------------------------------------------

namespace {

static GLuint gpu_driven_compile_compute_shader(const char** strings, int count) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    if (!sh) return 0;
    glShaderSource(sh, count, strings, nullptr);
    glCompileShader(sh);
    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetShaderInfoLog(sh, logLen, nullptr, log);
            mc2::fmt::print_err("[GPU_DRIVEN v1] compute shader compile error:\n{}\n", mc2::fmt::safe(log));
            fflush(stderr);
            delete[] log;
        }
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint gpu_driven_link_compute_program(GLuint shader) {
    GLuint prog = glCreateProgram();
    if (!prog) return 0;
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            glGetProgramInfoLog(prog, logLen, nullptr, log);
            mc2::fmt::print_err("[GPU_DRIVEN v1] compute program link error:\n{}\n", mc2::fmt::safe(log));
            fflush(stderr);
            delete[] log;
        }
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

}  // namespace

GLuint gpu_driven::BuildComputeProgramFromFile(
        const char* fname,
        const std::string* preambles, int nPreambles,
        const char* debugName)
{
    // Resolve #include directives through the engine's own recursive
    // preprocessor (the makeProgram path), NOT the GL driver — neither
    // GL_ARB_shading_language_include nor GL_GOOGLE_include_directive is
    // requested, so an unresolved #include is a hard compile error.
    // resolvedSource must outlive the glShaderSource call below.
    std::string resolvedSource;
    std::vector<std::string> includes;
    if (!load_shader(fname, resolvedSource, includes)) {
        mc2::fmt::print_err("[GPU_DRIVEN v1] {} source load/parse failed: {}\n", mc2::fmt::safe(debugName), mc2::fmt::safe(fname));
        fflush(stderr);
        return 0;
    }

    const char* kVersionPrefix = "#version 430\n";
    std::vector<const char*> srcStrs;
    srcStrs.push_back(kVersionPrefix);
    for (int j = 0; j < nPreambles; ++j)
        srcStrs.push_back(preambles[j].c_str());
    srcStrs.push_back(resolvedSource.c_str());

    GLuint sh = gpu_driven_compile_compute_shader(srcStrs.data(), (int)srcStrs.size());
    if (!sh) {
        mc2::fmt::print_err("[GPU_DRIVEN v1] {} compile failed\n", mc2::fmt::safe(debugName));
        fflush(stderr);
        return 0;
    }
    GLuint prog = gpu_driven_link_compute_program(sh);
    glDeleteShader(sh);
    if (!prog) {
        mc2::fmt::print_err("[GPU_DRIVEN v1] {} link failed\n", mc2::fmt::safe(debugName));
        fflush(stderr);
    }
    return prog;
}

}  // namespace gpu_driven
