//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLEXEC.CPP
//
//***************************************************************************

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<utility>
#include "mc2fmt.h"
#include "platform_intrinsics.h"

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLERR_H
#include"ablerr.h"
#endif

#ifndef ABLSCAN_H
#include"ablscan.h"
#endif

#ifndef ABLEXEC_H
#include"ablexec.h"
#endif

#ifndef ABLDBUG_H
#include"abldbug.h"
#endif

//***************************************************************************

//--------
// GLOBALS
char*					codeBuffer = nullptr;
char*					codeBufferPtr = nullptr;


StackItem*				stack = nullptr;
size_t					stackCount = 0;
long*					StaticVariablesSizes = nullptr;
long*					EternalVariablesSizes = nullptr;
long					eternalOffset = 0;
long					MaxStaticVariables = 0;
long					MaxEternalVariables = 0;
long					NumStaticVariables = 0;
long					NumOrderCalls = 1;
long					NumStateHandles = 0;
StateHandleInfo			StateHandleList[MAX_STATE_HANDLES_PER_MODULE];
long					MaxCodeBufferSize = 0;
long					MaxLoopIterations = 100001;
bool					AssertEnabled = false;
bool					PrintEnabled = true;
bool					StringFunctionsEnabled = true;
bool					DebugCodeEnabled = false;
bool					IncludeDebugInfo = true;
bool					ProfileABL = false;
bool					Crunch = true;

char					SetStateDebugStr[256];

// The root executor context runs every script on the environment stack; a nested run binds its own.
AblExecContext			g_ablRootExec;
AblExecContext*			g_ablExec = &g_ablRootExec;


//----------
// EXTERNALS


extern ModuleEntryPtr	ModuleRegistry;
extern ABLModulePtr*	ModuleInstanceRegistry;
extern ABLModulePtr		CurLibrary;

extern TokenCodeType	curToken;
extern int32_t          lineNumber;
extern int32_t          FileNumber;
extern TypePtr			IntegerTypePtr;
extern TypePtr			CharTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;



extern DebuggerPtr		debugger;

extern void (*ABLEndlessStateCallback) (UserFile* log);

//***************************************************************************
// CRUNCH/DECRUNCH routines
//***************************************************************************

// Bytecode is byte-packed: a multi-byte slot need not be aligned, so it is copied, never dereferenced through a typed pointer.
template <class T> static void putCode (char* at, T value) {
	memcpy(at, &value, sizeof(T));
}

template <class T> static T peekCode (const void* at) {
	T value;
	memcpy(&value, at, sizeof(T));
	return(value);
}

void crunchToken (void) {

	if (!Crunch)
		return;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		*codeBufferPtr = static_cast<char>(std::to_underlying(curToken));
		codeBufferPtr++;
	}
}

//***************************************************************************

char* LastNodeCrunchSlot = nullptr;

char* lastCrunchedNodeSlot (void) {

	return(Crunch ? LastNodeCrunchSlot : nullptr);
}

void patchCrunchedNode (char* slot, SymTableNodePtr nodePtr) {

	// Re-point a node pointer crunchSymTableNodePtr emitted earlier (a routine call's id) at another routine.
	if (Crunch && slot)
		putCode(slot, nodePtr);
}

void crunchSymTableNodePtr (SymTableNodePtr nodePtr) {

	if (!Crunch)
		return;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		LastNodeCrunchSlot = codeBufferPtr;
		putCode(codeBufferPtr, nodePtr);
		codeBufferPtr += sizeof(SymTableNodePtr);
	}
}

//***************************************************************************

void crunchStatementMarker (void) {

	if (!Crunch)
		return;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		char saveCode = *(--codeBufferPtr);
		*codeBufferPtr = static_cast<char>(std::to_underlying(TKN_STATEMENT_MARKER));
		codeBufferPtr++;
		if (IncludeDebugInfo) {
			*(reinterpret_cast<unsigned char*>(codeBufferPtr)) = static_cast<unsigned char>(FileNumber);
			codeBufferPtr += sizeof(unsigned char);
			putCode(codeBufferPtr, static_cast<int>(lineNumber));
			codeBufferPtr += sizeof(int);
		}
		*codeBufferPtr = saveCode;
		codeBufferPtr++;
	}
}

//***************************************************************************

void uncrunchStatementMarker (void) {

	//-------------------------
	// Pull code off the buffer
	codeBufferPtr--;
	//-------------------------------
	// Pull debug info off the buffer
	if (IncludeDebugInfo)
		codeBufferPtr -= (sizeof(unsigned char) + sizeof(int));
	//-------------------------------------
	// Pull statement marker off the buffer
	codeBufferPtr--;
}

//***************************************************************************

char* crunchAddressMarker (Address address) {

	if (!Crunch)
		return(nullptr);

	char* saveCodeBufferPtr = nullptr;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		char saveCode = *(--codeBufferPtr);
		*codeBufferPtr = static_cast<char>(std::to_underlying(TKN_ADDRESS_MARKER));
		codeBufferPtr++;
		saveCodeBufferPtr = codeBufferPtr;
		putCode(codeBufferPtr, address);
		codeBufferPtr += sizeof(Address);
		*codeBufferPtr = saveCode;
		codeBufferPtr++;
	}

	return(saveCodeBufferPtr);
}

//***************************************************************************

char* fixupAddressMarker (Address address) {

	if (!Crunch)
		return(nullptr);

	char* oldAddress = peekCode<Address>(address);

	putCode(address, static_cast<size_t>(codeBufferPtr - address));
	return(oldAddress);
}

//***************************************************************************

void crunchInteger (int value) {

	if (!Crunch)
		return;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		putCode(codeBufferPtr, value);
		codeBufferPtr += sizeof(int);
	}
}

//***************************************************************************

void crunchByte (unsigned char value) {

	if (!Crunch)
		return;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		*(reinterpret_cast<unsigned char*>(codeBufferPtr)) = value;
		codeBufferPtr += sizeof(unsigned char);
	}
}

//***************************************************************************

void crunchOffset (Address address) {

	if (!Crunch)
		return;

	if (codeBufferPtr >= (codeBuffer + MaxCodeBufferSize - 100))
		syntaxError(ABL_ERR_SYNTAX_CODE_SEGMENT_OVERFLOW);
	else {
		putCode(codeBufferPtr, static_cast<size_t>(address - codeBufferPtr));
		codeBufferPtr += sizeof(size_t);
	}
}

//***************************************************************************

char* createCodeSegment (long& codeSegmentSize) {

	codeSegmentSize = static_cast<long>(codeBufferPtr - codeBuffer + 1);
	// The ABL VM's getCodeToken / execute loop reads exactly one byte past
	// the last valid opcode before the end-of-segment check fires. On
	// normal MSVC builds this overread is hidden by heap-allocator slack
	// (requests rounded up to a 16-byte granule); ASan's red zones
	// eliminate that slack and report a 1-byte heap-buffer-overflow.
	// Allocate one extra byte and write a 0 sentinel so the trailing
	// read decodes as end-of-routine regardless of how the VM gets
	// there. Caller-visible codeSegmentSize stays the actual bytecode
	// length; no consumer needs updating.
	// See docs/asan-follow-ups.md findings #5 / #6.
	char* codeSegment = static_cast<char*>(ABL_alloc(ABLPool::Code, codeSegmentSize + 1));
	if (!codeSegment)
		ABL_Fatal(0, " ABL: Unable to AblCodeHeap->malloc code segment ");

	for (long i = 0; i < codeSegmentSize; i++)
		codeSegment[i] = codeBuffer[i];
	codeSegment[codeSegmentSize] = 0;  // VM end-of-segment sentinel

	codeBufferPtr = codeBuffer;
	return(codeSegment);
}

//***************************************************************************

SymTableNodePtr getCodeSymTableNodePtr (void) {

	SymTableNodePtr nodePtr = peekCode<SymTableNodePtr>(g_ablExec->codeSegmentPtr);
	g_ablExec->codeSegmentPtr += sizeof(SymTableNodePtr);
	return(nodePtr);
}

//***************************************************************************

int getCodeStatementMarker (void) {

	//------------------------------------------
	// NOTE: If there's a problem, we return -1.

	int lineNum = -1;
	if (g_ablExec->codeToken == TKN_STATEMENT_MARKER) {
		if (IncludeDebugInfo) {
			FileNumber = *(reinterpret_cast<unsigned char*>(g_ablExec->codeSegmentPtr));
			g_ablExec->codeSegmentPtr += sizeof(unsigned char);
			lineNum = peekCode<int>(g_ablExec->codeSegmentPtr);
			g_ablExec->codeSegmentPtr += sizeof(int);
		}
	}
	return(lineNum);
}

//***************************************************************************

char* getCodeAddressMarker (void) {

	Address address = nullptr;

	if (g_ablExec->codeToken == TKN_ADDRESS_MARKER) {
		address = peekCode<size_t>(g_ablExec->codeSegmentPtr) + g_ablExec->codeSegmentPtr - 1;
		g_ablExec->codeSegmentPtr += sizeof(Address);
	}
	return(address);
}

//***************************************************************************

int getCodeInteger (void) {

	int value = peekCode<int>(g_ablExec->codeSegmentPtr);
	g_ablExec->codeSegmentPtr += sizeof(int);
	return(value);
}

//***************************************************************************

unsigned char getCodeByte (void) {

	unsigned char value = *(reinterpret_cast<unsigned char*>(g_ablExec->codeSegmentPtr));
	g_ablExec->codeSegmentPtr += sizeof(unsigned char);
	return(value);
}

//***************************************************************************

char* getCodeAddress (void) {

	Address address = peekCode<size_t>(g_ablExec->codeSegmentPtr) + g_ablExec->codeSegmentPtr - 1;
	g_ablExec->codeSegmentPtr += sizeof(size_t);
	return(address);
}

//***************************************************************************
// STACK routines
//***************************************************************************

void pop (void) {

	--g_ablExec->tos;
}

//***************************************************************************

void getCodeToken (void) {

	g_ablExec->codeToken = static_cast<TokenCodeType>(*g_ablExec->codeSegmentPtr);
	g_ablExec->codeSegmentPtr++;
}

//***************************************************************************

void pushInteger (int value) {

	StackItemPtr valuePtr = ++g_ablExec->tos;

	if (valuePtr >= g_ablExec->stackLimit)
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->integer = value;
}

//***************************************************************************

void pushReal (float value) {

	StackItemPtr valuePtr = ++g_ablExec->tos;

	if (valuePtr >= g_ablExec->stackLimit)
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->real = value;
}

//***************************************************************************

void pushByte (char value) {

	StackItemPtr valuePtr = ++g_ablExec->tos;

	if (valuePtr >= g_ablExec->stackLimit)
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->byte = value;
}

//***************************************************************************

void pushAddress (Address address) {

	StackItemPtr valuePtr = ++g_ablExec->tos;

	if (valuePtr >= g_ablExec->stackLimit)
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->address = address;
}

//***************************************************************************

void pushBoolean (bool value) {

	StackItemPtr valuePtr = ++g_ablExec->tos;

	if (valuePtr >= g_ablExec->stackLimit)
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->integer = (value ? 1 : 0);
}

//***************************************************************************
void pushStackFrameHeader (long oldLevel, long newLevel) {

	//-----------------------------------
	// Make space for the return value...
	pushInteger(0);

	StackFrameHeaderPtr headerPtr = reinterpret_cast<StackFrameHeaderPtr>(g_ablExec->stackFrameBasePtr);

	//----------------------------------------------------------------------
	// STATIC LINK
	// Currently, let's not allow functions defined within functions. Assume
	// the old scope level equals the new scope level, for now.
	if (newLevel == -1) {
		//--------------------------------------------------------------------
		// Calling a library function, so push a NULL static link since
		// it's scope is in a different module than the calling function.
		// Note that global variables in libraries should be STATIC, otherwise
		// they're never in scope! Weird "feature" which we may want
		// to fix later...
		pushAddress(nullptr);
		}
	else if (newLevel == oldLevel + 1) {
		//----------------------------------------------------------------
		// Calling a routine nested within the caller, so push the pointer
		// to the caller's stack frame. In ABL, as currently defined
		// (2/22/96), this should only be when a module's code section is
		// calling a function...
		pushAddress(reinterpret_cast<Address>(headerPtr));
		}
	else if (newLevel == oldLevel) {
		//---------------------------------------------------------------
		// Calling a function at the same scope level. We like that! Push
		// pointer to stack frame of their common parent...
		pushAddress(headerPtr->staticLink.address);
		}
	else {
		//-------------------------------------------------------
		// Oops. We don't want nested functions, for now, in ABL.
		runtimeError(ABL_ERR_RUNTIME_NESTED_FUNCTION_CALL);
	}
	pushAddress(reinterpret_cast<Address>(g_ablExec->stackFrameBasePtr));

	//---------------------------
	// Push the return address...
	pushAddress(nullptr);
}

//***************************************************************************

void allocLocal (TypePtr typePtr) {

	if (typePtr == IntegerTypePtr)
		pushInteger(0);
	else if (typePtr == RealTypePtr)
		pushReal(static_cast<float>(0.0));
	else if (typePtr == BooleanTypePtr)
		pushByte(0);
	else if (typePtr == CharTypePtr)
		pushByte(0);
	else
		switch (typePtr->form) {
			case FRM_ENUM:
				pushInteger(0);
				break;
// NOTE: We currently are not supporting sub ranges, until
// we really want 'em...
//			case FRM_SUBRANGE:
//				allocLocal(typePtr->info.subrange.rangeTypePtr);
//				break;
			case FRM_ARRAY:
				char* ptr = static_cast<char*>(ABL_alloc(ABLPool::Stack, typePtr->size));
				if (!ptr)
					ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc local array ");
				pushAddress(reinterpret_cast<Address>(ptr));
				break;
		}
}

//***************************************************************************

void freeLocal (SymTableNodePtr idPtr) {

	//---------------------------------------
	// Frees data allocated on local stack...

	TypePtr typePtr = idPtr->typePtr;

	StackItemPtr itemPtr = nullptr;
	if (((typePtr->form == FRM_ARRAY) /* || (typePtr->form == FRM_RECORD)*/) &&
		(idPtr->defn.key != DFN_REFPARAM)) {
		switch (idPtr->defn.info.data.varType) {
			case VAR_TYPE_NORMAL:
				itemPtr = g_ablExec->stackFrameBasePtr + idPtr->defn.info.data.offset;
				break;
//			case VAR_TYPE_ETERNAL:
//				itemPtr = stack + idPtr->defn.info.data.offset;
//				break;
//			case VAR_TYPE_STATIC:
//				itemPtr = StaticDataPtr + idPtr->defn.info.data.offset;
//				break;
		}
		if (!itemPtr)
			runtimeError(static_cast<RuntimeErrorType>(0));
		else {
			// Mod-tolerance: some old MC2X-compiled .abx (e.g. campaign area46) carry
			// stack-frame layouts whose array-local offset lands on the wrong slot at
			// routine exit, so itemPtr->address reads a garbage value (seen:
			// 0x...FFFFFFFF, an int local + the adjacent -1 word). Passing that to
			// free() faults in RtlFreeHeap (routineExit -> UserHeap::Free). Every real
			// ABLStackMalloc pointer is heap-aligned and non-null, so skip the free for
			// any null / unaligned / all-ones-low-word pointer instead of crashing.
			unsigned long long a = reinterpret_cast<unsigned long long>(itemPtr->address);
			bool plausible = (a != 0ull)
			              && ((a & 0x7ull) == 0ull)                 // 8-byte aligned
			              && ((a & 0xFFFFFFFFull) != 0xFFFFFFFFull); // not the -1 sentinel
			if (plausible) {
				// Belt-and-suspenders: even a "plausible" pointer from a foreign .abx
				// stack-layout mismatch may not actually belong to our heap; an SEH
				// frame guarantees a bad free can never crash the mission (leak at worst).
				MC2_SEH_TRY {
					ABL_free(ABLPool::Stack, itemPtr->address);
				} MC2_SEH_EXCEPT (1 /*EXCEPTION_EXECUTE_HANDLER*/) {
				}
			}
		}
	}
}

//***************************************************************************
// FUNCTION ENTRY/EXIT routines
//***************************************************************************

void routineEntry (SymTableNodePtr routineIdPtr) {

	if (debugger)
		debugger->traceRoutineEntry(routineIdPtr);

	memset(&g_ablExec->returnValue, 0, sizeof(StackItem));

	//------------------------------
	// Switch to new code segment...
	g_ablExec->codeSegmentPtr = routineIdPtr->defn.info.routine.codeSegment;

	//----------------------------------------------
	// Allocate local variables onto system stack...
	for (SymTableNodePtr varIdPtr = routineIdPtr->defn.info.routine.locals;
		 varIdPtr != nullptr;
		 varIdPtr = varIdPtr->next)
		if (varIdPtr->defn.info.data.varType == VAR_TYPE_NORMAL)
			allocLocal(varIdPtr->typePtr);
}

//***************************************************************************

void routineExit (SymTableNodePtr routineIdPtr) {

	if (debugger)
		debugger->traceRoutineExit(routineIdPtr);

	//-----------------------------------------
	// De-alloc parameters & local variables...

	for (SymTableNodePtr idPtr = routineIdPtr->defn.info.routine.params;
		 idPtr != nullptr;
		 idPtr = idPtr->next)
		freeLocal(idPtr);

	for (SymTableNodePtr idPtr = routineIdPtr->defn.info.routine.locals;
		 idPtr != nullptr;
		 idPtr = idPtr->next)
		if (idPtr->defn.info.data.varType == VAR_TYPE_NORMAL)
			freeLocal(idPtr);

	StackFrameHeaderPtr headerPtr = reinterpret_cast<StackFrameHeaderPtr>(g_ablExec->stackFrameBasePtr);
	g_ablExec->codeSegmentPtr = headerPtr->returnAddress.address;

	if (routineIdPtr->typePtr == nullptr)
		g_ablExec->tos = g_ablExec->stackFrameBasePtr - 1;
	else
		g_ablExec->tos = g_ablExec->stackFrameBasePtr;

	g_ablExec->stackFrameBasePtr = reinterpret_cast<StackItemPtr>(headerPtr->dynamicLink.address);
}

//***************************************************************************

void execute (SymTableNodePtr routineIdPtr) {

	SymTableNodePtr thisRoutineIdPtr = g_ablExec->CurRoutineIdPtr;

	g_ablExec->CurRoutineIdPtr = routineIdPtr;

	routineEntry(routineIdPtr);

	//----------------------------------------------------
	// Now, search this module for the function we want...
	if (g_ablExec->CallModuleInit) {
		g_ablExec->CallModuleInit = false;
		SymTableNodePtr initFunctionIdPtr = searchSymTable("init", ModuleRegistry[g_ablExec->CurModule->getHandle()].moduleIdPtr->defn.info.routine.localSymTable);
		if (initFunctionIdPtr) {
			execRoutineCall(initFunctionIdPtr, false);
			//-------------------------------------------------------------------------
			// Since we're calling the function directly, we need to compensate for the
			// codeSegmentPtr being incremented by 1 in the normal execRoutineCall...
			g_ablExec->codeSegmentPtr--;
		}
	}

	if (routineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM) {
		g_ablExec->NewStateSet = true;
		static char stateList[60][256];
		strcpy(SetStateDebugStr, "--");
		while (g_ablExec->NewStateSet) {
			g_ablExec->NumStateTransitions++;
			mc2::fmt::to_buf(stateList[g_ablExec->NumStateTransitions], "{} ({})", mc2::fmt::safe(g_ablExec->CurModule->getState()->name), mc2::fmt::safe(SetStateDebugStr));
			if (g_ablExec->NumStateTransitions == 50) {
				UserFile* userFile = UserFile::getNewFile();
				char errStr[512];
				if (userFile) {
					long err = userFile->open("endless.log");
					if (!err) {
						//char s[1024];
						//sprintf(s, "Current Date: %s\n", GetTime());
						//userFile->write(s);
						userFile->write(ModuleRegistry[g_ablExec->CurModule->getHandle()].fileName);
						for (long i = 1; i < 51; i++)
							userFile->write(stateList[i]);
						userFile->write(" ");
						if (ABLEndlessStateCallback)
							(*ABLEndlessStateCallback)(userFile);
						userFile->close();
					}
				}
				mc2::fmt::to_buf(errStr, " ABL endless state loop in {} [{}:{}] ", mc2::fmt::safe(ModuleRegistry[g_ablExec->CurModule->getHandle()].fileName), mc2::fmt::safe(g_ablExec->CurModule->getState()->name), mc2::fmt::safe(g_ablExec->CurModule->getPrevState()->name));
				#if 0
				ABL_Fatal(g_ablExec->NumStateTransitions, errStr);
				#else
				g_ablExec->NewStateSet = false;
				#endif
				}
			else {
				g_ablExec->NewStateSet = false;
				SymTableNodePtr curState = g_ablExec->CurModule->getState();
				if (!curState)
					ABL_Fatal(0, " ABL.execute: NULL state in FSM ");
				execRoutineCall(curState, false);
				g_ablExec->codeSegmentPtr--;
			}
			//---------------------------------------------
			// In case we exited with a return statement...
			g_ablExec->ExitWithReturn = false;
			g_ablExec->ExitFromTacOrder = false;
		}
		}
	else {
		getCodeToken();
		execStatement();
		//---------------------------------------------
		// In case we exited with a return statement...
		g_ablExec->ExitWithReturn = false;
		g_ablExec->ExitFromTacOrder = false;
	}

	routineExit(routineIdPtr);

	g_ablExec->CurRoutineIdPtr = thisRoutineIdPtr;
}

//***************************************************************************

void executeChild (SymTableNodePtr routineIdPtr, SymTableNodePtr childRoutineIdPtr) {

	// THIS DOES NOT SUPPORT CALLING FUNCTIONS WITH PARAMETERS YET!

	SymTableNodePtr thisRoutineIdPtr = g_ablExec->CurRoutineIdPtr;

	g_ablExec->CurRoutineIdPtr = routineIdPtr;

	routineEntry(routineIdPtr);

	//----------------------------------------------------
	// Now, search this module for the function we want...
	SymTableNodePtr initFunctionIdPtr = nullptr;
	if (g_ablExec->CallModuleInit) {
		g_ablExec->CallModuleInit = false;
		initFunctionIdPtr = searchSymTable("init", routineIdPtr->defn.info.routine.localSymTable);
		if (initFunctionIdPtr) {
			execRoutineCall(initFunctionIdPtr, false);
			//-------------------------------------------------------------------------
			// Since we're calling the function directly, we need to compensate for the
			// codeSegmentPtr being incremented by 1 in the normal execRoutineCall...
			g_ablExec->codeSegmentPtr--;
		}
	}

	if (initFunctionIdPtr != childRoutineIdPtr) {
		//-----------------------------------------------------------------------
		// If we're calling the module's init function, and we just did above,
		// don't call it again! That's why we make the check on the above line...
		execRoutineCall(childRoutineIdPtr, false);
		//-------------------------------------------------------------------------
		// Since we're calling the function directly, we need to compensate for the
		// codeSegmentPtr being incremented by 1 in the normal execRoutineCall...
		g_ablExec->codeSegmentPtr--;
	}

	//---------------------------------------------
	// In case we exited with a return statement...
	g_ablExec->ExitWithReturn = false;
	g_ablExec->ExitFromTacOrder = false;

	routineExit(routineIdPtr);

	g_ablExec->CurRoutineIdPtr = thisRoutineIdPtr;
}

//***************************************************************************
// MISC routines
//***************************************************************************


