// tests/unit/heap_shim.cpp
//
// MINIMAL-VIABLE-HARNESS-1: malloc-backed heap + GameOS-global shim so REAL
// engine TUs (mclib/file.cpp, inifile.cpp, packet.cpp, ffile.cpp, ...) link
// into mc2_tests WITHOUT mclib/heap.cpp and WITHOUT the GameOS runtime.
//
// Why not link heap.cpp? It was measured to not compile standalone
// (C2601/C1004 -- needs the engine target's exact macro set); see
// docs/testing/fitini-inmem-harness-bootstrap-recon-1.md blocker #3.
//
// Why is this legitimate? heap.h's own LINUX_BUILD -> USE_GOS_HEAP branch
// already swaps the free-list allocator for gos heap calls, proving the
// custom allocator is a *convention*, not a semantic contract
// (.claude/ARCH-REVIEW-MCLIB-CORE-1.md section 1). This TU supplies the same
// class-method symbols heap.h declares, backed by CRT malloc/free. heap.cpp
// is deliberately NOT in the target, so there is exactly one definition of
// each symbol -- no ODR violation. Game behavior is untouched: this file is
// linked ONLY by tests/unit/mc2_tests.
//
// Rules for extending (see docs/testing/minimal-viable-harness.md):
//   * Only define symbols the linker actually demands.
//   * Keep allocation semantics CRT-plain: Malloc(0) returns a real pointer,
//     Free(NULL) is a no-op, Free() of any Malloc/calloc result is exact.
//   * Never add engine-behavior logic here; if a TU needs real behavior,
//     link the real TU instead.

#include "heap.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

//---------------------------------------------------------------------------
// HeapManager -- bookkeeping only; no VirtualAlloc, no committed region.
//---------------------------------------------------------------------------
HeapManager::HeapManager (void)
{
	init();
}

HeapManager::~HeapManager (void)
{
	destroy();
}

void HeapManager::init (void)
{
	heap = NULL;
	memReserved = false;
	totalSize = 0;
	committedSize = 0;
	whoMadeMe = 0;
	nxt = NULL;
}

void HeapManager::destroy (void)
{
	heap = NULL;
	memReserved = false;
	totalSize = 0;
	committedSize = 0;
}

long HeapManager::createHeap (unsigned long memSize)
{
	totalSize = memSize;
	return NO_ERR;
}

long HeapManager::commitHeap (unsigned long commitSize)
{
	committedSize = commitSize ? commitSize : totalSize;
	return NO_ERR;
}

long HeapManager::decommitHeap (unsigned long)
{
	return NO_ERR;
}

MemoryPtr HeapManager::getHeapPtr (void)
{
	return heap;
}

HeapManager::operator MemoryPtr (void)
{
	return heap;
}

void HeapManager::MemoryDump ()
{
}

//---------------------------------------------------------------------------
// UserHeap -- every allocation forwards to CRT malloc/free.
//---------------------------------------------------------------------------
UserHeap::UserHeap (void) : HeapManager()
{
	heapStart = NULL;
	heapEnd = NULL;
	firstNearBlock = NULL;
	heapSize = 0;
	mallocFatals = false;
	heapState = NO_ERR;
	heapName = NULL;
	useGOSGuardPage = false;
	// Non-null so the inline heapReady() (USE_GOS_HEAP branch under
	// LINUX_BUILD) reports ready. Never dereferenced by shim-linked code.
	gosHeap = (HGOSHEAP)(intptr_t)1;
#ifdef _DEBUG
	recordArray = NULL;
	recordCount = 0;
	logMallocs = false;
#endif
}

UserHeap::~UserHeap (void)
{
	destroy();
}

long UserHeap::init (unsigned long memSize, const char *heapId, bool /*useGOS*/)
{
	heapSize = memSize;
	if (heapId)
	{
		heapName = (char *)::malloc(strlen(heapId) + 1);
		if (heapName)
			strcpy(heapName, heapId);
	}
	return NO_ERR;
}

void UserHeap::destroy (void)
{
	if (heapName)
	{
		::free(heapName);
		heapName = NULL;
	}
	heapSize = 0;
}

unsigned long UserHeap::totalCoreLeft (void)
{
	return 0x40000000;		// "plenty" -- shim never runs out before malloc does
}

unsigned long UserHeap::coreLeft (void)
{
	return 0x40000000;
}

void *UserHeap::Malloc (size_t memSize)
{
	// Engine callers Malloc(0) in edge paths (e.g. zero-block .fit);
	// return a real pointer so the matching Free stays symmetric.
	return ::malloc(memSize ? memSize : 1);
}

long UserHeap::Free (void *memBlock)
{
	if (memBlock)
		::free(memBlock);
	return NO_ERR;
}

void *UserHeap::calloc (size_t memSize)
{
	return ::calloc(1, memSize ? memSize : 1);
}

void UserHeap::walkHeap (bool, bool)
{
}

long UserHeap::getLastError (void)
{
	return heapState;
}

bool UserHeap::pointerOnHeap (void *)
{
	// CRT owns everything; report true so debug checks never false-fail.
	return true;
}

#ifdef _DEBUG
void UserHeap::startHeapMallocLog (void) {}
void UserHeap::stopHeapMallocLog (void) {}
void UserHeap::dumpRecordLog (void) {}
#endif

//---------------------------------------------------------------------------
// HeapList -- statistics registry; pure no-op in tests.
//---------------------------------------------------------------------------
GlobalHeapRec HeapList::heapRecords[MAX_HEAPS];
bool HeapList::heapInstrumented = false;

void HeapList::addHeap (HeapManagerPtr) {}
void HeapList::removeHeap (HeapManagerPtr) {}
void HeapList::update (void) {}
void HeapList::dumpLog (void) {}
void HeapList::initializeStatistics () {}

HeapListPtr globalHeapList = NULL;

//---------------------------------------------------------------------------
// Globals the file stack expects the engine to own.
//---------------------------------------------------------------------------

// systemHeap: File::operator new/delete, FitIniFile::afterOpen block table,
// et al. all route here. Function-local static so it is constructed before
// first use even during other TUs' dynamic init.
static UserHeap &testSystemHeap (void)
{
	static UserHeap s_heap;
	return s_heap;
}

UserHeapPtr systemHeap = &testSystemHeap();

// Environment: file.cpp reads Environment.checkCDForFiles on the FST-miss
// path. Zero-initialized => no CD probing, exactly what tests want.
// (Engine definition lives in GameOS/gameos/gameos.cpp, which drags the
// whole platform layer -- stubbed instead.)
gosEnvironment Environment = {};

// STOP()/Fatal()/Assert() funnel (engine definition: gameos_debugging.cpp).
// In tests a STOP is an unconditional hard failure: print and abort so the
// test runner reports the crash instead of corrupting silently.
int __cdecl InternalFunctionStop (const char *Message, ...)
{
	fprintf(stderr, "\n[mc2_tests heap_shim] STOP: ");
	va_list args;
	va_start(args, Message);
	vfprintf(stderr, Message, args);
	va_end(args);
	fprintf(stderr, "\n");
	fflush(stderr);
	abort();
}

int __cdecl InternalFunctionStop (const char *Message, const char *value)
{
	fprintf(stderr, "\n[mc2_tests heap_shim] STOP: %s %s\n", Message, value);
	fflush(stderr);
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
void EnterFullScreenMode (void) {}
void ExitGameOS (void) {}

// Crash-bundle instrumentation ring (engine: GameOS/gameos/gos_crashbundle.cpp,
// drags SEH/minidump machinery). FastFile::readFast appends trace lines.
extern "C" void crashbundle_append (const char *) {}
extern "C" void crashbundle_init (void) {}
