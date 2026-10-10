//============================================================================
// EditorGpuTimer — see EditorGpuTimer.h.
//  * GPU per-pass timing via GL_TIMESTAMP (ping-pong, read a frame late).
//  * CPU per-pass wall-clock via steady_clock (same marks, printed same frame).
// A GPU timestamp only brackets *submitted GPU commands*; if a pass burns
// hundreds of ms on the CPU then submits little GPU work, the GPU delta stays
// small. The CPU line catches that case.
//============================================================================
#include <GL/glew.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>

#include "EditorGpuTimer.h"
#include "Gates/gate.h"

namespace {

const int   kMaxMarks = 16;
GLuint      s_q[2][kMaxMarks] = {{0}};   // ping-pong GPU query objects
const char* s_name[kMaxMarks] = {0};     // segment label at mark i
int         s_active = 0;
int         s_count[2] = {0, 0};
bool        s_inited = false;
bool        s_on = false;
unsigned long s_frame = 0;

// CPU wall-clock per mark (current frame; available immediately).
double      s_cpuMs[kMaxMarks] = {0};
int         s_cpuCount = 0;

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool enabled()
{
    if (!s_inited) {
        s_on = mc2::gate::get<"MC2_EDITOR_GPU_TIMERS">();
        s_inited = true;
    }
    return s_on;
}

void appendLog(const char* line)
{
    FILE* lf = std::fopen("editor-startup.log", "a");
    if (lf) { std::fprintf(lf, "%s\n", line); std::fclose(lf); }
}

} // namespace

void EditorGpuTimer_Begin()
{
    if (!enabled()) return;

    // Read + print the previous frame's GPU buffer (guaranteed available).
    const int prev = 1 - s_active;
    const int n = s_count[prev];
    if (n >= 2) {
        GLint avail = 0;
        glGetQueryObjectiv(s_q[prev][n - 1], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            GLuint64 t0 = 0;
            glGetQueryObjectui64v(s_q[prev][0], GL_QUERY_RESULT, &t0);
            GLuint64 prevT = t0;

            char line[640];
            int off = 0;
            off += std::snprintf(line + off, sizeof(line) - off, "[EDGPU f=%lu]", s_frame);
            for (int i = 1; i < n; ++i) {
                GLuint64 t = 0;
                glGetQueryObjectui64v(s_q[prev][i], GL_QUERY_RESULT, &t);
                off += std::snprintf(line + off, sizeof(line) - off, " %s=%.2f",
                                     s_name[i] ? s_name[i] : "?", (double)(t - prevT) / 1.0e6);
                prevT = t;
            }
            std::snprintf(line + off, sizeof(line) - off, " TOTAL=%.2f", (double)(prevT - t0) / 1.0e6);
            appendLog(line);
        }
    }

    ++s_frame;
    s_count[s_active] = 0;
    s_cpuCount = 0;
    EditorGpuTimer_Mark("start");
}

void EditorGpuTimer_Mark(const char* name)
{
    if (!enabled()) return;
    const int idx = s_count[s_active];
    if (idx >= kMaxMarks) return;
    if (!s_q[s_active][idx]) glGenQueries(1, &s_q[s_active][idx]);
    glQueryCounter(s_q[s_active][idx], GL_TIMESTAMP);
    s_name[idx] = name;
    s_count[s_active] = idx + 1;

    // CPU wall-clock at this same boundary.
    if (idx < kMaxMarks) { s_cpuMs[idx] = nowMs(); s_cpuCount = idx + 1; }
}

//----------------------------------------------------------------------------
// CPU-only phase timer (PRE-render work). Independent state from the GPU timer.
//----------------------------------------------------------------------------
namespace {
double      s_preMs[kMaxMarks] = {0};
const char* s_preName[kMaxMarks] = {0};
int         s_preCount = 0;
unsigned long s_preFrame = 0;
double      s_lastBeginMs = 0.0;   // wall-clock of previous frame's Begin
double      s_frameDtMs = 0.0;     // gap since previous frame (incl render+swap+idle)
} // namespace

void EditorCpuPhase_Begin()
{
    if (!enabled()) return;
    const double now = nowMs();
    s_frameDtMs = (s_lastBeginMs > 0.0) ? (now - s_lastBeginMs) : 0.0;
    s_lastBeginMs = now;
    s_preCount = 0;
    EditorCpuPhase_Mark("start");
}

void EditorCpuPhase_Mark(const char* name)
{
    if (!enabled()) return;
    if (s_preCount >= kMaxMarks) return;
    s_preMs[s_preCount]   = nowMs();
    s_preName[s_preCount] = name;
    ++s_preCount;
}

void EditorCpuPhase_End()
{
    if (!enabled()) return;
    if (s_preCount >= 2) {
        char line[640];
        int off = 0;
        off += std::snprintf(line + off, sizeof(line) - off, "[EDPRE f=%lu dt=%.1f]", s_preFrame, s_frameDtMs);
        for (int i = 1; i < s_preCount; ++i)
            off += std::snprintf(line + off, sizeof(line) - off, " %s=%.2f",
                                 s_preName[i] ? s_preName[i] : "?", s_preMs[i] - s_preMs[i - 1]);
        std::snprintf(line + off, sizeof(line) - off, " TOTAL=%.2f", s_preMs[s_preCount - 1] - s_preMs[0]);
        appendLog(line);
    }
    ++s_preFrame;
}

void EditorGpuTimer_End()
{
    if (!enabled()) return;

    // CPU deltas are available immediately — print this frame's CPU line now.
    if (s_cpuCount >= 2) {
        char line[640];
        int off = 0;
        off += std::snprintf(line + off, sizeof(line) - off, "[EDCPU f=%lu]", s_frame);
        for (int i = 1; i < s_cpuCount; ++i)
            off += std::snprintf(line + off, sizeof(line) - off, " %s=%.2f",
                                 s_name[i] ? s_name[i] : "?", s_cpuMs[i] - s_cpuMs[i - 1]);
        std::snprintf(line + off, sizeof(line) - off, " TOTAL=%.2f", s_cpuMs[s_cpuCount - 1] - s_cpuMs[0]);
        appendLog(line);
    }

    s_active = 1 - s_active;       // flip GPU ping-pong
}

//----------------------------------------------------------------------------
// Whole-frame phase timer (RunGameOSLogic).
//----------------------------------------------------------------------------
namespace {
double      s_frmMs[kMaxMarks] = {0};
const char* s_frmName[kMaxMarks] = {0};
int         s_frmCount = 0;
unsigned long s_frmFrame = 0;
double      s_frmLastBeginMs = 0.0;
double      s_frmDtMs = 0.0;
} // namespace

void EditorFramePhase_Begin()
{
    if (!enabled()) return;
    const double now = nowMs();
    s_frmDtMs = (s_frmLastBeginMs > 0.0) ? (now - s_frmLastBeginMs) : 0.0;
    s_frmLastBeginMs = now;
    s_frmCount = 0;
    EditorFramePhase_Mark("start");
}

void EditorFramePhase_Mark(const char* name)
{
    if (!enabled()) return;
    if (s_frmCount >= kMaxMarks) return;
    s_frmMs[s_frmCount]   = nowMs();
    s_frmName[s_frmCount] = name;
    ++s_frmCount;
}

void EditorFramePhase_End()
{
    if (!enabled()) return;
    if (s_frmCount >= 2) {
        char line[640];
        int off = 0;
        off += std::snprintf(line + off, sizeof(line) - off, "[EDFRM f=%lu dt=%.1f]", s_frmFrame, s_frmDtMs);
        for (int i = 1; i < s_frmCount; ++i)
            off += std::snprintf(line + off, sizeof(line) - off, " %s=%.2f",
                                 s_frmName[i] ? s_frmName[i] : "?", s_frmMs[i] - s_frmMs[i - 1]);
        std::snprintf(line + off, sizeof(line) - off, " TOTAL=%.2f", s_frmMs[s_frmCount - 1] - s_frmMs[0]);
        appendLog(line);
    }
    ++s_frmFrame;
}
