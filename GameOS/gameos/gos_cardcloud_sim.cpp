//==========================================================================//
// File:    gos_cardcloud_sim.cpp                                            //
// Contents: GameOS-side GL bridge for the CardCloud GPU simulation.         //
//           VFX-GPU-SIM-CARDCLOUD-COMPUTE-1 (compare-only, pipeline         //
//           validation).                                                    //
//                                                                           //
//   submit()  — ACCUMULATE each CardCloud instance's compacted live records //
//               into a per-frame CPU buffer (fixes BUFFER-1 last-writer-win).//
//   flush()   — once/frame (from Batcher::Flush): upload the accumulated     //
//               array to a persistent SSBO, dispatch cardcloud_sim.comp to   //
//               integrate ONE validation step (age+=ageRate*dt,             //
//               pos+=vel*dt), and (MC2_VFX_GPU_SIM_COMPARE=1) read back +    //
//               compare against a CPU reference applying the SAME step to    //
//               the same input. maxAgeError/maxPosError ~ float epsilon     //
//               proves the GPU compute path (shader/SSBO/dispatch/barrier/   //
//               readback). It does NOT prove full gosFX lifetime physics —   //
//               drag/ether/accel + life-long parity (needs stable particle  //
//               IDs) are a separate PARITY slice.                           //
//                                                                           //
//   NOT here: rendering GPU-sim output, CPU-sim bypass, curve LUTs, IDs.     //
//===========================================================================//

#include "particles/cardcloud_sim.h"
#include "particles/batcher.h"   // is_gpu_sim_cardcloud_enabled / is_gpu_sim_compare_enabled
#include "gos_gpu_sync.h"

#include <gameos.hpp>
#include <GL/glew.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <vector>
#include <string>

namespace {

// Fixed validation timestep. COMPUTE-1 is a pipeline check, not real-time
// integration — GPU and the CPU reference use this identical dt, so the
// compare isolates GPU-vs-CPU compute correctness (not frame-time accuracy).
// Real per-frame dt belongs to the later PARITY slice.
const float kValidationDt = 1.0f / 60.0f;

// Persistent SSBO (process-lifetime; gos_particle_bridge convention).
GLuint  s_ssbo       = 0;
GLsizei s_capacity   = 0;     // records
GLuint  s_program    = 0;     // cardcloud_sim.comp program
bool    s_progFailed = false;
bool    s_bufFailed  = false;
GLint   s_loc_dt     = -1;
GLint   s_loc_count  = -1;

// Per-frame accumulator (single-threaded render path).
std::vector<mc2::particles::CardCloudSimParticle> s_frameRecords;
unsigned s_frameInstances    = 0;
unsigned s_frameCpuActiveSum = 0;

char* loadTextFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return nullptr; }
    char* buf = new char[sz + 1];
    size_t rd = std::fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    std::fclose(f);
    return buf;
}

void buildProgramIfNeeded() {
    if (s_program != 0 || s_progFailed) return;
    char* src = loadTextFile("shaders/cardcloud_sim.comp");
    if (!src) {
        s_progFailed = true;
        std::fprintf(stderr,
            "[VFX_GPU_SIM v2] event=shader_load_fail source_missing=shaders/cardcloud_sim.comp\n");
        std::fflush(stderr);
        return;
    }
    // "#version 430\n" prefix per CLAUDE.md shader-#version rule.
    const char* strings[2] = { "#version 430\n", src };
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 2, strings, nullptr);
    glCompileShader(sh);
    delete[] src;
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &n);
        if (n > 0) { char* log = new char[n]; glGetShaderInfoLog(sh, n, nullptr, log);
            std::fprintf(stderr, "[VFX_GPU_SIM v2] compute compile error:\n%s\n", log); delete[] log; }
        glDeleteShader(sh); s_progFailed = true; std::fflush(stderr); return;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDeleteShader(sh);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint n = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &n);
        if (n > 0) { char* log = new char[n]; glGetProgramInfoLog(prog, n, nullptr, log);
            std::fprintf(stderr, "[VFX_GPU_SIM v2] compute link error:\n%s\n", log); delete[] log; }
        glDeleteProgram(prog); s_progFailed = true; std::fflush(stderr); return;
    }
    s_program   = prog;
    s_loc_dt    = glGetUniformLocation(prog, "u_dt");
    s_loc_count = glGetUniformLocation(prog, "u_count");
    std::fprintf(stderr, "[VFX_GPU_SIM v2] event=compute_ready prog=%u recordBytes=%u\n",
                 (unsigned)prog, (unsigned)sizeof(mc2::particles::CardCloudSimParticle));
    std::fflush(stderr);
}

void ensureCapacity(GLsizei needRecords) {
    if (s_ssbo == 0) {
        // TIER2-EXCLUDED: substrate-gated
        glGenBuffers(1, &s_ssbo);
        if (s_ssbo == 0) { s_bufFailed = true; return; }
    }
    if (needRecords > s_capacity) {
        GLsizei newCap = (needRecords < 1024) ? 1024 : needRecords;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
        while (glGetError() != GL_NO_ERROR) {}
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(newCap * sizeof(mc2::particles::CardCloudSimParticle)),
                     nullptr, GL_DYNAMIC_DRAW);
        if (glGetError() != GL_NO_ERROR) {
            s_bufFailed = true;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            return;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_capacity = newCap;
    }
}

void resetFrame() {
    s_frameRecords.clear();
    s_frameInstances    = 0;
    s_frameCpuActiveSum = 0;
}

} // namespace

extern "C" void gos_cardcloud_sim_submit(
    const mc2::particles::CardCloudSimParticle* records,
    unsigned int                                count,
    unsigned int                                cpuActiveCount)
{
    if (records == nullptr || count == 0) return;
    s_frameRecords.insert(s_frameRecords.end(), records, records + count);
    ++s_frameInstances;
    s_frameCpuActiveSum += cpuActiveCount;
}

extern "C" void gos_cardcloud_sim_flush(void)
{
    const unsigned n = (unsigned)s_frameRecords.size();
    if (n == 0) { resetFrame(); return; }   // gate-OFF or all-dead frame

    buildProgramIfNeeded();
    ensureCapacity((GLsizei)n);
    if (s_progFailed || s_bufFailed || s_program == 0 || s_ssbo == 0) { resetFrame(); return; }

    // Keep the pre-integration input for the CPU reference compare.
    // (s_frameRecords IS the input; we read back into a separate vector.)
    GLint savedProgram = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    (GLsizeiptr)(n * sizeof(mc2::particles::CardCloudSimParticle)),
                    s_frameRecords.data());

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, /*binding=*/0, s_ssbo);
    glUseProgram(s_program);
    if (s_loc_dt    >= 0) glUniform1f(s_loc_dt, kValidationDt);
    if (s_loc_count >= 0) glUniform1ui(s_loc_count, n);
    const GLuint groups = (n + 63u) / 64u;
    glDispatchCompute(groups, 1, 1);
    gpuSyncBarrier(GpuProducer::ComputeShader, GpuConsumer::BufferReadback,
                   "cardcloud-sim-dispatch");

    // Dispatch diagnostic — ALWAYS-ON (not gated on compare) so the compute
    // path is observable with MC2_VFX_GPU_SIM_CARDCLOUD=1 alone. One-shot +
    // periodic; no readback (cheap, no stall).
    {
        static bool s_dispFirst = false;
        static unsigned long long s_dispCalls = 0;
        if (!s_dispFirst) {
            s_dispFirst = true;
            std::fprintf(stderr,
                "[VFX_GPU_SIM v2] class=CardCloud DISPATCH instances=%u count=%u groups=%u (compute path live)\n",
                s_frameInstances, n, (unsigned)groups);
            std::fflush(stderr);
        }
        if ((++s_dispCalls % 600ull) == 0ull) {
            std::fprintf(stderr,
                "[VFX_GPU_SIM v2] class=CardCloud dispatched=%llu lastCount=%u\n",
                s_dispCalls, n);
            std::fflush(stderr);
        }
    }

    // Compare (gated): read back + diff vs a CPU reference of the SAME step.
    if (mc2::particles::Batcher::is_gpu_sim_compare_enabled()) {
        static std::vector<mc2::particles::CardCloudSimParticle> s_readback;
        s_readback.resize(n);
        // Read back via GL_COPY_READ_BUFFER (not the SSBO binding) — keeps the
        // SSBO slot clean during the CPU stall; safer on AMD (INDEX-SHADERS).
        glBindBuffer(GL_COPY_READ_BUFFER, s_ssbo);
        glGetBufferSubData(GL_COPY_READ_BUFFER, 0,
                           (GLsizeiptr)(n * sizeof(mc2::particles::CardCloudSimParticle)),
                           s_readback.data());
        glBindBuffer(GL_COPY_READ_BUFFER, 0);

        double maxAgeErr = 0.0, maxPosErr = 0.0;
        unsigned aliveGpu = 0, aliveRef = 0, compared = 0;
        for (unsigned i = 0; i < n; ++i) {
            const mc2::particles::CardCloudSimParticle& in  = s_frameRecords[i]; // input
            const mc2::particles::CardCloudSimParticle& out = s_readback[i];      // GPU result
            const bool inAlive = (in.flags & mc2::particles::kCardCloudSimFlagAlive) != 0;
            float refAge = in.age;
            float refPos[3] = { in.position[0], in.position[1], in.position[2] };
            if (inAlive) {
                refAge += in.ageRate * kValidationDt;
                refPos[0] += in.velocity[0] * kValidationDt;
                refPos[1] += in.velocity[1] * kValidationDt;
                refPos[2] += in.velocity[2] * kValidationDt;
            }
            const bool refAlive = inAlive && (refAge < 1.0f);
            if (refAlive)  ++aliveRef;
            if (out.flags & mc2::particles::kCardCloudSimFlagAlive) ++aliveGpu;
            if (inAlive) {
                ++compared;
                double dAge = std::fabs((double)out.age - (double)refAge);
                if (dAge > maxAgeErr) maxAgeErr = dAge;
                double dx = (double)out.position[0] - (double)refPos[0];
                double dy = (double)out.position[1] - (double)refPos[1];
                double dz = (double)out.position[2] - (double)refPos[2];
                double dPos = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dPos > maxPosErr) maxPosErr = dPos;
            }
        }
        static bool s_first = false;
        static unsigned long long s_calls = 0;
        if (!s_first) {
            s_first = true;
            std::fprintf(stderr,
                "[VFX_GPU_SIM v2] class=CardCloud FIRST_DISPATCH instances=%u count=%u groups=%u\n",
                s_frameInstances, n, (unsigned)groups);
            std::fflush(stderr);
        }
        if ((++s_calls % 120ull) == 0ull) {
            std::fprintf(stderr,
                "[VFX_GPU_SIM v2] class=CardCloud calls=%llu instances=%u count=%u groups=%u "
                "cpuActiveSum=%u compared=%u aliveGPU=%u aliveRef=%u "
                "maxAgeError=%.3e maxPosError=%.3e dt=%.5f compareStatus=pipeline-validate(GPU==CPUref)\n",
                s_calls, s_frameInstances, n, (unsigned)groups, s_frameCpuActiveSum,
                compared, aliveGpu, aliveRef, maxAgeErr, maxPosErr, (double)kValidationDt);
            std::fflush(stderr);
        }
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, /*binding=*/0, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glUseProgram((GLuint)savedProgram);

    resetFrame();
}
