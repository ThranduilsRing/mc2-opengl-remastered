//============================================================================
// EditorWatchdog — see EditorWatchdog.h. TEMPORARY diagnostic.
//============================================================================
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>

#include "EditorWatchdog.h"
#include "Gates/gate.h"

#pragma comment(lib, "dbghelp.lib")

namespace {

const DWORD  kStallMs      = 120;    // report a stall once main is silent this long
const DWORD  kPollMs       = 40;     // watchdog wake interval
const DWORD  kRateLimitMs  = 1000;   // min gap between captures (avoid spam)
const int    kMaxFrames    = 40;

HANDLE volatile s_mainThread = NULL;          // duplicated real handle to main thread
volatile LONGLONG s_lastBeatMs = 0;           // GetTickCount64 of last heartbeat
bool         s_inited = false;
bool         s_on = false;

bool enabled()
{
    return mc2::gate::get<"MC2_EDITOR_WATCHDOG">();
}

void appendLog(const char* line)
{
    FILE* lf = std::fopen("editor-startup.log", "a");
    if (lf) { std::fprintf(lf, "%s\n", line); std::fclose(lf); }
}

void captureStall(DWORD stallMs)
{
    HANDLE th = s_mainThread;
    if (!th) return;

    if (SuspendThread(th) == (DWORD)-1) return;

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;

    char hdr[128];
    std::snprintf(hdr, sizeof(hdr), "[WATCHDOG] Main thread stalled %lu ms", (unsigned long)stallMs);
    appendLog(hdr);

    if (GetThreadContext(th, &ctx))
    {
        HANDLE proc = GetCurrentProcess();
        // SymInitialize may already have run (crash bundler) — harmless to repeat.
        SymInitialize(proc, NULL, TRUE);
        SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);

        STACKFRAME64 frame;
        memset(&frame, 0, sizeof(frame));
        DWORD machine;
#if defined(_M_X64)
        machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset    = ctx.Rip; frame.AddrPC.Mode    = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
#else
        machine = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset    = ctx.Eip; frame.AddrPC.Mode    = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Ebp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Esp; frame.AddrStack.Mode = AddrModeFlat;
#endif
        appendLog("[WATCHDOG] Stack:");
        for (int i = 0; i < kMaxFrames; ++i)
        {
            if (!StackWalk64(machine, proc, th, &frame, &ctx,
                             NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            if (frame.AddrPC.Offset == 0) break;

            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen   = 255;
            DWORD64 disp = 0;

            char line[360];
            if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym))
                std::snprintf(line, sizeof(line), "  %s + 0x%llx", sym->Name, (unsigned long long)disp);
            else
                std::snprintf(line, sizeof(line), "  0x%llx", (unsigned long long)frame.AddrPC.Offset);
            appendLog(line);
        }
    }
    else
    {
        appendLog("[WATCHDOG] (GetThreadContext failed)");
    }

    ResumeThread(th);
}

DWORD WINAPI watchdogProc(LPVOID)
{
    LONGLONG lastCaptureMs = 0;
    for (;;)
    {
        Sleep(kPollMs);
        const LONGLONG now = (LONGLONG)GetTickCount64();
        const LONGLONG beat = s_lastBeatMs;
        if (beat == 0) continue;
        const LONGLONG age = now - beat;
        if (age >= (LONGLONG)kStallMs && (now - lastCaptureMs) >= (LONGLONG)kRateLimitMs)
        {
            lastCaptureMs = now;
            captureStall((DWORD)age);
        }
    }
    return 0;
}

void initOnce()
{
    s_inited = true;
    s_on = enabled();
    if (!s_on) return;

    // Duplicate the (pseudo) current-thread handle into a real handle the
    // watchdog thread can use to suspend/inspect us.
    HANDLE dup = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &dup,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, 0);
    s_mainThread = dup;
    s_lastBeatMs = (LONGLONG)GetTickCount64();

    appendLog("[WATCHDOG] armed (MC2_EDITOR_WATCHDOG=1, stall>500ms)");
    CreateThread(NULL, 0, watchdogProc, NULL, 0, NULL);
}

} // namespace

void EditorWatchdog_Heartbeat()
{
    if (!s_inited) initOnce();
    if (!s_on) return;
    s_lastBeatMs = (LONGLONG)GetTickCount64();
}
