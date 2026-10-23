#include "heap.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string_view>

// Environment: file.cpp reads Environment.checkCDForFiles on the FST-miss
// path. Zero-initialized => no CD probing, exactly what tests want.
// (Engine definition lives in GameOS/gameos/gameos.cpp, which drags the
// whole platform layer -- stubbed instead.)
gosEnvironment Environment = {};

// STOP()/Fatal()/Assert() funnel (engine definition: gameos_debugging.cpp).
// In tests a STOP is an unconditional hard failure: print and abort so the
// test runner reports the crash instead of corrupting silently.
// A test that exercises a path the engine only LOGS (STOP is a message box + continue there) can clear
// g_mc2TestStopFatal; it then counts the STOPs and the code under test carries on, like the engine.
bool g_mc2TestStopFatal = true;
int  g_mc2TestStopCount = 0;

int __cdecl InternalFunctionStop (const char *Message, ...)
{
	fprintf(stderr, "\n[mc2_tests heap_shim] STOP: ");
	va_list args;
	va_start(args, Message);
	vfprintf(stderr, Message, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
	if (!g_mc2TestStopFatal) { ++g_mc2TestStopCount; return 0; }
	abort();
}

int __cdecl InternalFunctionStop (const char *Message, const char *value)
{
	fprintf(stderr, "\n[mc2_tests heap_shim] STOP: %s %s\n", Message, value);
	fflush(stderr);
	if (!g_mc2TestStopFatal) { ++g_mc2TestStopCount; return 0; }
	abort();
}

// PAUSE() funnel -- non-fatal in the engine (dialog + continue); in tests just
// log and keep going. Returning 0 skips ENTER_DEBUGGER.
int __cdecl InternalFunctionPause (const char *Message, ...)
{
	fprintf(stderr, "\n[mc2_tests heap_shim] PAUSE: ");
	va_list args;
	va_start(args, Message);
	vfprintf(stderr, Message, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
	return 0;
}

// Window-mode flips: File::open's CD-missing dialog path and ffile.cpp flip
// the display mode before/after showing a message box. No display in tests.
void EnterWindowMode (void) {}
bool gos_NoUserPresentRequested (void) { return false; }   // not an unattended session
void EnterFullScreenMode (void) {}
void ExitGameOS (void) {}

// Crash-bundle instrumentation ring (engine: GameOS/gameos/gos_crashbundle.cpp,
// drags SEH/minidump machinery). FastFile::readFast appends trace lines.
extern "C" void crashbundle_append (const char *) {}
void crashbundle_content_fd(int, std::string_view, std::string_view) {}
void crashbundle_content_image(std::string_view, const void*, size_t) {}
void crashbundle_mod(std::string_view, int) {}
void crashbundle_save_point(std::string_view, std::string_view) {}
extern "C" void crashbundle_init (void) {}
