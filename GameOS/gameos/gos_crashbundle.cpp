#include "vk_render_infra.h"
// gos_crashbundle.cpp
// See gos_crashbundle.h. Implementation notes:
//   * Ring buffer uses a pre-allocated static 64 KiB byte region with a
//     monotonic write cursor. The crash-path flush reads a snapshot of the
//     cursor and writes the wrap-safe linear view via WriteFile directly.
//   * Inside the SEH filter we avoid malloc/new/fprintf. All scratch is on
//     the stack or in pre-allocated static buffers. File writes use WriteFile.
//   * MiniDumpWriteDump is safe per DbgHelp docs even in a corrupted heap.
//   * Dialog uses DialogBoxIndirectParamW with an in-memory template built on
//     the stack — no resource file required.

#include "gos_crashbundle.h"
#include "crash_content.h"
#ifdef _WIN32
#include "crash_content_writer.h"
#endif
#include "Gates/gate.h"
#include "build_identity.h"  // Stable build identity declarations
#include "mc2fmt.h"
#include <filesystem>
#include <string>
#include <string_view>

// Shared mission identity is also used when the native crash writer is absent.
char missionName[1024];

#ifndef _WIN32
extern "C" void crashbundle_init(void) {}
extern "C" void crashbundle_append(const char*) {}
void crashbundle_content_fd(int, std::string_view, std::string_view) {}
void crashbundle_content_image(std::string_view, const void*, size_t) {}
void crashbundle_mod(std::string_view, int) {}
void crashbundle_save_point(std::string_view, std::string_view) {}
extern "C" void crashbundle_device_lost(void) {}
extern "C" void crashbundle_trigger_test_crash(void) {
    volatile int* p = 0;
    *p = 0xDEADC0DE;
}
#else

#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

// CURSOR-GRAB-FIX-1. Canonical declaration lives in GameOS/gameos/gos_render.h
// (defined in gameos_input.cpp). Forward-declared rather than included so the
// crash-handler TU does not pull in SDL. A rename on the far side is a LINK
// error here, not a silent miss.
// META4-CLOSE-1: the NARROW predicate (MC2_WINDOW_OFFSCREEN / MC2_SMOKE_MODE).
// gos_UnattendedRunRequested() is also true for a bare MC2_NO_CURSOR_GRAB=1,
// which is a documented interactive option, and must not gate user-visible UI.
bool gos_NoUserPresentRequested();

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------
static const size_t kRingBytes = 64 * 1024;
static char g_ring[kRingBytes];
static volatile LONG64 g_ring_cursor = 0;  // total bytes written, monotonic
static volatile LONG   g_ring_lock   = 0;  // spinlock, 0=free 1=held

// NS3 boundary: missionName canonical def moved here so gameos_editor (which
// lacks code/mechcmd2.cpp) can link cleanly. All consumers extern-declare it.
// C++ linkage (no extern "C") — matches the old mechcmd2.cpp definition.
extern uint32_t g_mc2FrameCounter; // mclib/tgl.cpp

// Captured env flags (set once in crashbundle_init)
static struct {
    int destroy_trace;
    int heartbeat;
    int abl_trace;
    int abl_reg_trace;
    int ff_trace;
    int crash_fast_exit; // headless: skip the modal crash dialog + terminate now
    // CURSOR-GRAB-FIX-1: this run has no user watching it (MC2_NO_CURSOR_GRAB /
    // MC2_SMOKE_MODE / MC2_WINDOW_OFFSCREEN). Resolved ONCE at init, in normal
    // program state, so the crash path only reads an int -- this file's own rule
    // is to avoid malloc/new/fprintf inside the SEH filter, and getenv() with a
    // function-local static guard is not something to do in a corrupted process.
    int unattended;
    int initialized;
    char exe_dir[MAX_PATH];
} g_cfg = {0};

// Folder path for the current crash bundle. Populated inside the filter.
// Wide version for ShellExecuteW. ANSI used for WriteFile paths.
static char  g_crash_folderA[MAX_PATH];
static WCHAR g_crash_folderW[MAX_PATH];
static mc2::crash::ContentWriter g_content;
void crashbundle_content_fd(int fd, std::string_view name, std::string_view owner) { try { g_content.fd(fd, name, owner); } catch (...) {} }
void crashbundle_content_image(std::string_view name, const void* bytes, size_t size) { try { g_content.image(name, bytes, size); } catch (...) {} }
void crashbundle_mod(std::string_view id, int priority) { try { g_content.mod(id, priority); } catch (...) {} }
void crashbundle_save_point(std::string_view op, std::string_view path) { try { g_content.savePoint(op, path); } catch (...) {} }
extern "C" void mc2_log_set_crash_sink(void (*sink)(const char*));
// Full path to crash.txt (for clipboard copy from dialog)
static char  g_crash_txtA[MAX_PATH];

// ---------------------------------------------------------------------------
// Helpers (safe during normal runtime)
// ---------------------------------------------------------------------------
extern "C" void crashbundle_append(const char* line)
{
    if (!line) return;
    size_t n = strlen(line);
    if (n == 0) return;
    // Trim to avoid pathological lines blowing out the ring in one write.
    if (n > kRingBytes / 2) n = kRingBytes / 2;

    // Spinlock — short critical sections only.
    while (InterlockedCompareExchange(&g_ring_lock, 1, 0) != 0) {
        YieldProcessor();
    }
    LONG64 cur = g_ring_cursor;
    for (size_t i = 0; i < n; ++i) {
        g_ring[(cur + i) % kRingBytes] = line[i];
    }
    // ensure newline terminator
    g_ring[(cur + n) % kRingBytes] = '\n';
    g_ring_cursor = cur + (LONG64)n + 1;
    InterlockedExchange(&g_ring_lock, 0);
}

// ---------------------------------------------------------------------------
// Crash-path helpers — must avoid malloc/new/CRT-heavy paths
// ---------------------------------------------------------------------------

// Append a formatted chunk to a stack buffer at *pos. Uses wvsprintfA
// (user32) which does not allocate.
static void sbuff(char* dst, size_t cap, size_t* pos, const char* fmt, ...)
{
    if (*pos >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    // wvsprintfA has a 1024-byte output cap and does not support %f/%lld,
    // but it doesn't allocate. For our fields, we either pre-format or
    // use _snprintf_s which is CRT but does not heap-alloc for small %d/%s.
    int n = _vsnprintf_s(dst + *pos, cap - *pos, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) *pos += static_cast<size_t>(n);
}

static BOOL write_file_all(const char* path, const void* data, DWORD len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, len, &wrote, nullptr) && wrote == len;
    CloseHandle(h);
    return ok;
}

// Returns the folder path used; fills g_crash_folderA/W and g_crash_txtA
static void build_crash_folder(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    // <exe_dir>/crashes
    char crashes_root[MAX_PATH];
    mc2::fmt::to_buf_n(crashes_root, _countof(crashes_root), "{}\\crashes", mc2::fmt::safe(g_cfg.exe_dir));
    CreateDirectoryA(crashes_root, nullptr); // OK if exists

    mc2::fmt::to_buf_n(g_crash_folderA, _countof(g_crash_folderA),
                       "{}\\crash_{:04}-{:02}-{:02}_{:02}{:02}{:02}_{:03}_{}_{}",
                       crashes_root,
                       st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), GetTickCount64());
    CreateDirectoryA(g_crash_folderA, nullptr);

    MultiByteToWideChar(CP_ACP, 0, g_crash_folderA, -1,
                        g_crash_folderW, MAX_PATH);

    mc2::fmt::to_buf_n(g_crash_txtA, _countof(g_crash_txtA), "{}\\crash.txt", mc2::fmt::safe(g_crash_folderA));
}

// Writes the minidump
static void write_minidump(EXCEPTION_POINTERS* ep)
{
    char path[MAX_PATH];
    mc2::fmt::to_buf_n(path, _countof(path), "{}\\minidump.dmp", mc2::fmt::safe(g_crash_folderA));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MINIDUMP_TYPE t = (MINIDUMP_TYPE)(
        MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                      h, t, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(h);
}

// Writes the ring buffer snapshot to last_trace.txt
static void write_ring_snapshot(void)
{
    char path[MAX_PATH];
    mc2::fmt::to_buf_n(path, _countof(path), "{}\\last_trace.txt", mc2::fmt::safe(g_crash_folderA));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LONG64 cur = g_ring_cursor;
    DWORD wrote = 0;
    if (cur < (LONG64)kRingBytes) {
        WriteFile(h, g_ring, static_cast<DWORD>(cur), &wrote, nullptr);
    } else {
        // wrapped: oldest byte is at cur % kRingBytes
        DWORD start = static_cast<DWORD>((cur % kRingBytes));
        WriteFile(h, g_ring + start, static_cast<DWORD>((kRingBytes - start)), &wrote, nullptr);
        WriteFile(h, g_ring, start, &wrote, nullptr);
    }
    CloseHandle(h);
}

// Safely read missionName without faulting if bad pointer
static void safe_copy_mission_name(char* dst, size_t cap)
{
    __try {
        const char* src = missionName;
        size_t i = 0;
        while (i < cap - 1 && src[i] != '\0' && i < 1024) {
            char c = src[i];
            // strip non-printable to keep JSON valid
            if (c == '"' || c == '\\') { dst[i] = '?'; }
            else if (static_cast<unsigned char>(c) < 32) { dst[i] = ' '; }
            else { dst[i] = c; }
            ++i;
        }
        dst[i] = '\0';
        if (i == 0) { strcpy_s(dst, cap, "unavailable"); }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(dst, cap, "unavailable");
    }
}

static void write_profile_json(void)
{
    char path[MAX_PATH];
    mc2::fmt::to_buf_n(path, _countof(path), "{}\\profile.json", mc2::fmt::safe(g_crash_folderA));

    const char* build = MC2_BUILD_GIT_SHA;

    char mn[256];
    safe_copy_mission_name(mn, sizeof(mn));

    uint32_t frame = 0;
    __try { frame = g_mc2FrameCounter; }
    __except(EXCEPTION_EXECUTE_HANDLER) { frame = 0; }

    char buf[2048];
    size_t pos = 0;
    sbuff(buf, sizeof(buf), &pos, "{\n");
    sbuff(buf, sizeof(buf), &pos, "  \"build_hash\": \"%s\",\n", build);
    sbuff(buf, sizeof(buf), &pos, "  \"frame\": %u,\n", frame);
    sbuff(buf, sizeof(buf), &pos, "  \"mission_name\": \"%s\",\n", mn);
    sbuff(buf, sizeof(buf), &pos, "  \"env\": {\n");
    sbuff(buf, sizeof(buf), &pos, "    \"MC2_DESTROY_TRACE\": %s,\n",        g_cfg.destroy_trace  ? "true":"false");
    sbuff(buf, sizeof(buf), &pos, "    \"MC2_HEARTBEAT\": %s,\n",            g_cfg.heartbeat      ? "true":"false");
    sbuff(buf, sizeof(buf), &pos, "    \"MC2_ABL_TRACE\": %s,\n",            g_cfg.abl_trace      ? "true":"false");
    sbuff(buf, sizeof(buf), &pos, "    \"MC2_ABL_REG_TRACE\": %s,\n",        g_cfg.abl_reg_trace  ? "true":"false");
    sbuff(buf, sizeof(buf), &pos, "    \"MC2_FF_TRACE\": %s\n",              g_cfg.ff_trace       ? "true":"false");
    sbuff(buf, sizeof(buf), &pos, "  },\n");
    sbuff(buf, sizeof(buf), &pos, "  \"graphics\": \"unavailable\",\n");
    sbuff(buf, sizeof(buf), &pos, "  \"fps_recent\": \"unavailable\"\n");
    sbuff(buf, sizeof(buf), &pos, "}\n");

    write_file_all(path, buf, static_cast<DWORD>(pos));
}

// ---------------------------------------------------------------------------
// Stack walk into a stack buffer (produces the crash.txt body)
// ---------------------------------------------------------------------------
static size_t format_crash_txt(EXCEPTION_POINTERS* ep, char* out, size_t cap)
{
    size_t pos = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    sbuff(out, cap, &pos, "MC2 Remastered Crash Report\n");
    sbuff(out, cap, &pos, "Time: %04u-%02u-%02u %02u:%02u:%02u\n",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    sbuff(out, cap, &pos, "Build: %s\n", MC2_BUILD_GIT_SHA);
    sbuff(out, cap, &pos, "Fingerprint: %s\n", MC2_BUILD_FP_LITERAL);   // also keeps the MC2FP1 literal in the exe
    sbuff(out, cap, &pos, "Code: 0x%08lX  Flags: 0x%08lX  Addr: %p\n",
          ep->ExceptionRecord->ExceptionCode,
          ep->ExceptionRecord->ExceptionFlags,
          ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const char* kind =
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" :
            ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC";
        sbuff(out, cap, &pos, "%s violation at 0x%p\n", kind,
              reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
    }

    uint32_t frame = 0;
    __try { frame = g_mc2FrameCounter; } __except(EXCEPTION_EXECUTE_HANDLER) {}
    sbuff(out, cap, &pos, "Frame: %u\n", frame);

    char mn[256];
    safe_copy_mission_name(mn, sizeof(mn));
    sbuff(out, cap, &pos, "Mission: %s\n", mn);
    sbuff(out, cap, &pos, "\nStack:\n");

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    // SymInitialize may have already been called; that's fine — it's
    // idempotent with Invade=TRUE returning FALSE on second call.
    SymInitialize(proc, nullptr, TRUE);

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame_rec; ZeroMemory(&frame_rec, sizeof(frame_rec));
#if defined(_M_X64) || defined(_M_AMD64)
    frame_rec.AddrPC.Offset = ctx.Rip;
    frame_rec.AddrFrame.Offset = ctx.Rbp;
    frame_rec.AddrStack.Offset = ctx.Rsp;
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
    frame_rec.AddrPC.Offset = ctx.Eip;
    frame_rec.AddrFrame.Offset = ctx.Ebp;
    frame_rec.AddrStack.Offset = ctx.Esp;
    DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
    frame_rec.AddrPC.Mode = AddrModeFlat;
    frame_rec.AddrFrame.Mode = AddrModeFlat;
    frame_rec.AddrStack.Mode = AddrModeFlat;

#if defined(_M_X64) || defined(_M_AMD64)
    // EXEC violation at 0x0 = a CALL through a null function pointer (null vtable
    // / freed object / uninitialised callback). RIP is 0, so StackWalk64 cannot
    // seed the first frame and the stack comes out EMPTY. The return address the
    // bad CALL pushed is at [RSP] -> use it to recover the caller chain so the
    // crash is actually attributable. (Common with the soak's mission teardown.)
    if (frame_rec.AddrPC.Offset == 0 && frame_rec.AddrStack.Offset != 0 &&
        !IsBadReadPtr(reinterpret_cast<void*>(static_cast<uintptr_t>(frame_rec.AddrStack.Offset)), sizeof(DWORD64)))
    {
        sbuff(out, cap, &pos, "  #-- 0x0000000000000000  <null call (jmp/call to 0)>\n");
        DWORD64 retAddr = *reinterpret_cast<DWORD64*>(static_cast<uintptr_t>(frame_rec.AddrStack.Offset));
        ctx.Rip = retAddr;
        ctx.Rsp += sizeof(DWORD64);
        frame_rec.AddrPC.Offset = ctx.Rip;
        frame_rec.AddrStack.Offset = ctx.Rsp;
    }
#endif

    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(machine, proc, GetCurrentThread(), &frame_rec, &ctx,
                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame_rec.AddrPC.Offset == 0) break;

        char symBuf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp64 = 0;
        IMAGEHLP_LINE64 line; ZeroMemory(&line, sizeof(line));
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD dispL = 0;

        const char* symName = "?";
        const char* fileName = "?";
        DWORD lineNum = 0;
        if (SymFromAddr(proc, frame_rec.AddrPC.Offset, &disp64, sym))
            symName = sym->Name;
        if (SymGetLineFromAddr64(proc, frame_rec.AddrPC.Offset, &dispL, &line)) {
            fileName = line.FileName;
            lineNum = line.LineNumber;
        }
        sbuff(out, cap, &pos, "  #%02d 0x%016llX  %s  (%s:%u)\n",
              i, static_cast<unsigned long long>(frame_rec.AddrPC.Offset),
              symName, fileName, lineNum);
    }
    // Intentionally do NOT SymCleanup; other code or the OS may still use it.
    return pos;
}

// ---------------------------------------------------------------------------
// Dialog (modal, in-memory template)
// ---------------------------------------------------------------------------

enum {
    IDC_COPY_BTN   = 1001,
    IDC_OPEN_BTN   = 1002,
    IDC_CLOSE_BTN  = IDCANCEL, // let ESC dismiss
    IDC_BODY_TEXT  = 1004,
};

// The crash.txt path for clipboard-copy from the dialog callback
static const char* dialog_crash_txt_path = nullptr;
static const WCHAR* dialog_folder_path   = nullptr;

static void copy_file_to_clipboard(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return; }
    if (sz > 1024 * 1024) sz = 1024 * 1024; // sanity cap

    // allocate via GlobalAlloc — clipboard owns it after SetClipboardData
    HGLOBAL hMemA = GlobalAlloc(GMEM_MOVEABLE, sz + 1);
    if (!hMemA) { CloseHandle(h); return; }
    char* bufA = static_cast<char*>(GlobalLock(hMemA));
    DWORD got = 0;
    ReadFile(h, bufA, sz, &got, nullptr);
    bufA[got] = '\0';
    CloseHandle(h);

    // Convert to UTF-16 for CF_UNICODETEXT
    int wlen = MultiByteToWideChar(CP_UTF8, 0, bufA, -1, nullptr, 0);
    if (wlen <= 0) {
        // fallback: treat as ACP
        wlen = MultiByteToWideChar(CP_ACP, 0, bufA, -1, nullptr, 0);
    }
    HGLOBAL hMemW = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen) * sizeof(WCHAR));
    if (hMemW) {
        WCHAR* bufW = static_cast<WCHAR*>(GlobalLock(hMemW));
        int ok = MultiByteToWideChar(CP_UTF8, 0, bufA, -1, bufW, wlen);
        if (ok <= 0) MultiByteToWideChar(CP_ACP, 0, bufA, -1, bufW, wlen);
        GlobalUnlock(hMemW);
    }
    GlobalUnlock(hMemA);
    GlobalFree(hMemA); // we didn't end up giving this to clipboard

    if (hMemW && OpenClipboard(nullptr)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMemW);
        CloseClipboard();
        // ownership transfers to clipboard — don't free hMemW
    } else if (hMemW) {
        GlobalFree(hMemW);
    }
}

static INT_PTR CALLBACK dlg_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg) {
    case WM_INITDIALOG:
        // Center on screen
        {
            RECT r; GetWindowRect(hDlg, &r);
            int w = r.right - r.left, h = r.bottom - r.top;
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hDlg, HWND_TOPMOST, (sw - w)/2, (sh - h)/2,
                         0, 0, SWP_NOSIZE);
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_COPY_BTN:
            if (dialog_crash_txt_path)
                copy_file_to_clipboard(dialog_crash_txt_path);
            // feedback: flash button text? — minimal, stay closed-source simple
            return TRUE;
        case IDC_OPEN_BTN:
            if (dialog_folder_path)
                ShellExecuteW(nullptr, L"open", dialog_folder_path, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case IDCANCEL:
        case IDOK:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, 0);
        return TRUE;
    }
    return FALSE;
}

// Construct a DLGTEMPLATEEX-ish in-memory template using the classic
// DLGTEMPLATE form (simpler, sufficient for 3 buttons + static text).
// Layout:
//   DLGTEMPLATE + menu(0) + class(0) + title(unicode)
//   per-item: align DWORD, DLGITEMTEMPLATE + class(atom) + title + 0
static void align_dword(BYTE** p)
{
    ULONG_PTR v = (ULONG_PTR)*p;
    v = (v + 3) & ~((ULONG_PTR)3);
    *p = reinterpret_cast<BYTE*>(v);
}
static BYTE* write_word(BYTE* p, WORD v) { *reinterpret_cast<WORD*>(p) = v; return p + 2; }
static BYTE* write_dword(BYTE* p, DWORD v){ *reinterpret_cast<DWORD*>(p) = v; return p + 4; }
static BYTE* write_wstr(BYTE* p, const WCHAR* s)
{
    if (!s) { *reinterpret_cast<WORD*>(p) = 0; return p + 2; }
    while (*s) { *reinterpret_cast<WORD*>(p) = static_cast<WORD>(*s); p += 2; ++s; }
    *reinterpret_cast<WORD*>(p) = 0;
    return p + 2;
}

static void show_crash_dialog(const char* crash_txt_path,
                              const WCHAR* folder_path_w,
                              const char* folder_path_a)
{
    // CURSOR-GRAB-FIX-1: a window nobody is watching must not seize the user's
    // attention either. dlg_proc() below forces this dialog HWND_TOPMOST and
    // centres it on the primary display, and DialogBoxIndirectParamW is MODAL --
    // so an unattended run that crashed used to plant a focus-stealing,
    // input-eating box in the middle of whatever the user was doing, and hang
    // there until he clicked it. Second, independent guard: the call site also
    // takes the fast-exit branch (crashbundle_init sets crash_fast_exit for an
    // unattended run), and either one alone is sufficient.
    if (g_cfg.unattended) {
        static const char kMsg[] = "[CRASH] dialog suppressed (unattended run)\n";
        fwrite(kMsg, 1, sizeof(kMsg) - 1, stderr);
        fflush(stderr);
        return;
    }

    dialog_crash_txt_path = crash_txt_path;
    dialog_folder_path    = folder_path_w;

    // Dialog units: 4 DLU horizontal = 1 baseX pt; keep it simple.
    // Size: 260 x 110 DLU
    static BYTE tmpl[2048];
    memset(tmpl, 0, sizeof(tmpl));
    BYTE* p = tmpl;

    // DLGTEMPLATE
    DLGTEMPLATE dt;
    dt.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
               DS_SETFONT | DS_CENTER | WS_VISIBLE;
    dt.dwExtendedStyle = 0;
    dt.cdit = 4; // static body + 3 buttons
    dt.x = 0; dt.y = 0; dt.cx = 260; dt.cy = 110;
    memcpy(p, &dt, sizeof(dt)); p += sizeof(dt);
    p = write_word(p, 0); // no menu
    p = write_word(p, 0); // default dialog class

    // title
    p = write_wstr(p, L"MC2 Remastered crashed");
    // font: 8pt MS Shell Dlg (DS_SETFONT)
    p = write_word(p, 8);
    p = write_wstr(p, L"MS Shell Dlg");

    // Body text
    WCHAR body[512];
    _snwprintf_s(body, _countof(body), _TRUNCATE,
                 L"MC2 Remastered crashed.\n\nReport saved to:\n%hs",
                 folder_path_a ? folder_path_a : "(unknown)");

    // --- Static text ---
    align_dword(&p);
    DLGITEMTEMPLATE it;
    it.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    it.dwExtendedStyle = 0;
    it.x = 8; it.y = 8; it.cx = 244; it.cy = 70;
    it.id = IDC_BODY_TEXT;
    memcpy(p, &it, sizeof(it)); p += sizeof(it);
    p = write_word(p, 0xFFFF); p = write_word(p, 0x0082); // static class atom
    p = write_wstr(p, body);
    p = write_word(p, 0); // 0 creation-data length

    // --- Copy crash report ---
    align_dword(&p);
    it.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    it.x = 8;  it.y = 88; it.cx = 80; it.cy = 16;
    it.id = IDC_COPY_BTN;
    memcpy(p, &it, sizeof(it)); p += sizeof(it);
    p = write_word(p, 0xFFFF); p = write_word(p, 0x0080); // button class atom
    p = write_wstr(p, L"Copy crash report");
    p = write_word(p, 0);

    // --- Open folder ---
    align_dword(&p);
    it.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    it.x = 92; it.y = 88; it.cx = 72; it.cy = 16;
    it.id = IDC_OPEN_BTN;
    memcpy(p, &it, sizeof(it)); p += sizeof(it);
    p = write_word(p, 0xFFFF); p = write_word(p, 0x0080);
    p = write_wstr(p, L"Open folder");
    p = write_word(p, 0);

    // --- Close (default) ---
    align_dword(&p);
    it.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    it.x = 184; it.y = 88; it.cx = 68; it.cy = 16;
    it.id = IDCANCEL;
    memcpy(p, &it, sizeof(it)); p += sizeof(it);
    p = write_word(p, 0xFFFF); p = write_word(p, 0x0080);
    p = write_wstr(p, L"Close");
    p = write_word(p, 0);

    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                            (LPCDLGTEMPLATEW)tmpl,
                            nullptr, dlg_proc, 0);
}

// ---------------------------------------------------------------------------
// The SEH filter
// ---------------------------------------------------------------------------
static volatile LONG g_in_filter = 0;

static LONG WINAPI crashbundle_filter(EXCEPTION_POINTERS* ep)
{
    // Re-entrancy guard: if we fault inside the filter, fall through.
    if (InterlockedCompareExchange(&g_in_filter, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    __try {
        build_crash_folder();

        // 1) Build crash.txt content (also printed to stderr)
        static char crashtxt[32 * 1024];
        size_t n = format_crash_txt(ep, crashtxt, sizeof(crashtxt));

        // Keep the historical stderr trace the existing filter produced —
        // operators watching the console still see what they expect.
        fwrite(crashtxt, 1, n, stderr);
        fflush(stderr);

        // 2) Write crash.txt
        write_file_all(g_crash_txtA, crashtxt, static_cast<DWORD>(n));

        // 3) Write last_trace.txt
        write_ring_snapshot();
        g_content.snapshot(g_crash_folderA);
#ifdef MC2_VULKAN_ISLAND
        __try { mc2vk::infra::crashSnapshot(g_crash_folderA); }
        __except(EXCEPTION_EXECUTE_HANDLER) { /* A damaged mapping must not prevent the CPU bundle. */ }
#endif

        // 4) Write profile.json
        write_profile_json();

        // 5) Minidump
        write_minidump(ep);

        // 6) Either terminate now (headless soak/harness) or show the dialog.
        if (g_cfg.crash_fast_exit) {
            // Loud, single-line, parseable marker to stderr (always live) so the
            // watching harness/MCP detects the crash + bundle path immediately,
            // then exit WITHOUT the modal dialog (no WaitForExit-timeout wait).
            char mk[MAX_PATH + 64];
            int mn = mc2::fmt::to_buf(mk, "\n[CRASH] FAST-EXIT code=0x{:08X} bundle={}\n",
                                      ep->ExceptionRecord->ExceptionCode, g_crash_folderA);
            if (mn >= static_cast<int>(sizeof(mk))) mn = static_cast<int>(sizeof(mk)) - 1;
            if (mn > 0) { fwrite(mk, 1, static_cast<size_t>(mn), stderr); fflush(stderr); }
            TerminateProcess(GetCurrentProcess(),
                             ep->ExceptionRecord->ExceptionCode);
        }
        // Modal dialog — blocks until user dismisses (interactive runs only).
        show_crash_dialog(g_crash_txtA, g_crash_folderW, g_crash_folderA);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        // swallow any fault inside the filter; the OS still owns the crash.
    }

    // Let the OS do its default thing (WER, JIT debugger, etc.) after us.
    return EXCEPTION_CONTINUE_SEARCH;
}

// The Windows heap ends the process on a corrupted block without running the unhandled-exception
// filter, so a heap-corruption crash would leave no bundle. The vectored handler is the one
// in-process hook that still sees STATUS_HEAP_CORRUPTION, and the process dies right after it.
static LONG WINAPI crashbundle_heap_corruption_handler(EXCEPTION_POINTERS* ep)
{
    constexpr DWORD kStatusHeapCorruption = 0xC0000374;
    if (ep->ExceptionRecord->ExceptionCode != kStatusHeapCorruption)
        return EXCEPTION_CONTINUE_SEARCH;
    return crashbundle_filter(ep);
}

extern "C" void crashbundle_device_lost(void)
{
    if (InterlockedCompareExchange(&g_in_filter, 1, 0) != 0) return;
    build_crash_folder();
    char reason[2048];
    const int bytes = mc2::fmt::to_buf(reason, "Reason: Vulkan device lost\nFingerprint: {}\n", mc2::fmt::safe(MC2_BUILD_FP_LITERAL));
    if (bytes > 0) write_file_all(g_crash_txtA, reason, DWORD(bytes));
    write_ring_snapshot();
    g_content.snapshot(g_crash_folderA);
    write_profile_json();
    write_minidump(nullptr); // Live-process dump: there is no CPU exception for device loss.
#ifdef MC2_VULKAN_ISLAND
    mc2vk::infra::crashSnapshot(g_crash_folderA);
#endif
    mc2::fmt::print_err("[CRASHBUNDLE] device_lost bundle={}\n", g_crash_folderA);
    InterlockedExchange(&g_in_filter, 0);
}

// ---------------------------------------------------------------------------
// Public init + test trigger
// ---------------------------------------------------------------------------
extern "C" void crashbundle_init(void)
{
    if (g_cfg.initialized) return;
    g_cfg.initialized = 1;

    g_cfg.destroy_trace = mc2::gate::get<"MC2_DESTROY_TRACE">() ? 1 : 0;
    g_cfg.heartbeat = mc2::gate::get<"MC2_HEARTBEAT">() ? 1 : 0;
    g_cfg.abl_trace = mc2::gate::get<"MC2_ABL_TRACE">() ? 1 : 0;
    g_cfg.abl_reg_trace = mc2::gate::get<"MC2_ABL_REG_TRACE">() ? 1 : 0;
    g_cfg.ff_trace = mc2::gate::get<"MC2_FF_TRACE">() ? 1 : 0;
    // Headless crash fast-exit: write the bundle, then terminate WITHOUT the
    // modal dialog so an automated soak/harness process exits immediately on
    // crash (the watcher sees it now, not at the WaitForExit timeout). Auto-on
    // under the crash-soak; also settable directly.
    g_cfg.crash_fast_exit = mc2::gate::get<"MC2_CRASH_FAST_EXIT">() ? 1 : 0;
    { int soak = 0; soak = mc2::gate::get<"MC2_SOAK_AUTOWIN">() ? 1 : 0; if (soak) g_cfg.crash_fast_exit = 1; }

    // CURSOR-GRAB-FIX-1: an unattended run implies fast-exit. Declared here
    // rather than by adding another gate read per variable so this
    // file agrees with the engine's ONE definition of "no user is watching"
    // (gos_render.h / gameos_input.cpp) instead of re-deriving it and drifting.
    //
    // META4-CLOSE-1: that definition is gos_NoUserPresentRequested(), not the
    // wider gos_UnattendedRunRequested(). The wide one is true for a bare
    // MC2_NO_CURSOR_GRAB=1 -- a documented INTERACTIVE option -- so a player
    // who set it lost the crash dialog and got a forced fast-exit instead. A
    // crash dialog is exactly the user-visible UI that may only be suppressed
    // when there is provably no user to see it.
    g_cfg.unattended = gos_NoUserPresentRequested() ? 1 : 0;
    if (g_cfg.unattended)
        g_cfg.crash_fast_exit = 1;

    char exe_path[MAX_PATH];
    DWORD got = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    // No exception may leave this extern "C" function, and a path near MAX_PATH must not reach the CRT invalid-parameter handler.
    g_cfg.exe_dir[0] = '.'; g_cfg.exe_dir[1] = '\0';
    try {
        if (got > 0 && got < MAX_PATH) {
            const std::string exeDir = std::filesystem::path(exe_path).parent_path().string();
            if (!exeDir.empty() && exeDir.size() < sizeof(g_cfg.exe_dir)) memcpy(g_cfg.exe_dir, exeDir.c_str(), exeDir.size() + 1);
        }
        const auto journalDir = std::filesystem::path(g_cfg.exe_dir) / "debug_state";
        std::error_code journalEc;
        std::filesystem::create_directories(journalDir, journalEc);
        if (g_content.init((journalDir / "crash_content.jsonl").string()))
            g_content.mod("base", -1);
    } catch (...) {}
    mc2_log_set_crash_sink(crashbundle_append);
    SetUnhandledExceptionFilter(crashbundle_filter);
    AddVectoredExceptionHandler(1, crashbundle_heap_corruption_handler);
}

extern "C" void crashbundle_trigger_test_crash(void)
{
    mc2::fmt::print_err("[CRASHBUNDLE] RAlt+Shift+C test crash triggered\n");
    fflush(stderr);
    crashbundle_append("[CRASHBUNDLE] test crash triggered via RAlt+Shift+C");
    volatile int* p = static_cast<volatile int*>(nullptr);
    *p = 0xDEADC0DE; // null write → SEH
}

#endif // _WIN32
