// ABL standard routines: the return, print, concat, math, file, module, random and state-handle standard routines.
// Declared in the class headers; shared names in ablxstd_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "ablxstd_internal.h"
#include "abl_script_events.h"
#include "abl_string_append.h"
#include "mc2fmt.h"
#include "sim_trace.h"


//***************************************************************************

void execOrderReturn (long returnVal) {

	//-----------------------------
	// Assignment to function id...
	StackFrameHeaderPtr headerPtr = reinterpret_cast<StackFrameHeaderPtr>(stackFrameBasePtr);
	long delta = level - CurRoutineIdPtr->level - 1;
	while (delta-- > 0)
		headerPtr = reinterpret_cast<StackFrameHeaderPtr>(headerPtr->staticLink.address);

	if (CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_STATE) {
		//----------------------------------
		// Return in a state function, so...
		if (debugger)
			debugger->traceDataStore(CurRoutineIdPtr, CurRoutineIdPtr->typePtr, reinterpret_cast<StackItemPtr>(headerPtr), CurRoutineIdPtr->typePtr);
	
		ExitWithReturn = true;
		ExitFromTacOrder = true;

		if (returnVal == 0) {
			//----------------------------------------------------------
			// Use the "eject" code only if called for a failed Order...
			codeSegmentPtr = reinterpret_cast<char*>(ExitStateCodeSegment);
			getCodeToken();
		}
		
		}
	else {
		//-------------------------------------------------------------------------
		// All Order functions (TacticalOrder/GeneralOrder/ActionOrder) must return
		// an integer error code, so we assume the return type is IntegerTypePtr...
		StackItemPtr targetPtr = reinterpret_cast<StackItemPtr>(headerPtr);
		targetPtr->integer = returnVal;

		//----------------------------------------------------------------------
		// Preserve the return value, in case we need it for the calling user...
		memcpy(&returnValue, targetPtr, sizeof(StackItem));

		if (debugger)
			debugger->traceDataStore(CurRoutineIdPtr, CurRoutineIdPtr->typePtr, reinterpret_cast<StackItemPtr>(headerPtr), CurRoutineIdPtr->typePtr);
	
		ExitWithReturn = true;
		ExitFromTacOrder = true;

		if (returnVal == 0) {
			//----------------------------------------------------------
			// Use the "eject" code only if called for a failed Order...
			codeSegmentPtr = reinterpret_cast<char*>(ExitOrderCodeSegment);
			getCodeToken();
		}
	}
}

//***************************************************************************

void execStdReturn (void) {

	memset(&returnValue, 0, sizeof(StackItem));
	if (CurRoutineIdPtr->typePtr) {
		//-----------------------------
		// Assignment to function id...
		StackFrameHeaderPtr headerPtr = reinterpret_cast<StackFrameHeaderPtr>(stackFrameBasePtr);
		long delta = level - CurRoutineIdPtr->level - 1;
		while (delta-- > 0)
			headerPtr = reinterpret_cast<StackFrameHeaderPtr>(headerPtr->staticLink.address);

		StackItemPtr targetPtr = reinterpret_cast<StackItemPtr>(headerPtr);
		TypePtr targetTypePtr = CurRoutineIdPtr->typePtr;
		getCodeToken();

		//---------------------------------------------------------------
		// Routine execExpression() leaves the expression value on top of
		// stack...
		getCodeToken();
		TypePtr expressionTypePtr = execExpression();

		//--------------------------
		// Now, do the assignment...
		if ((targetTypePtr == RealTypePtr) && (expressionTypePtr == IntegerTypePtr)) {
			//-------------------------
			// integer assigned to real
			targetPtr->real = static_cast<float>((tos->integer));
			}
		else if (targetTypePtr->form == FRM_ARRAY) {
			//-------------------------
			// Copy the array/record...
			char* dest = reinterpret_cast<char*>(targetPtr);
			char* src = tos->address;
			const size_t capacity = std::min(static_cast<size_t>(std::max(0L, targetTypePtr->size)), sizeof(StackItem));
			const size_t sourceSize = src && expressionTypePtr && expressionTypePtr->form == FRM_ARRAY
			    ? static_cast<size_t>(std::max(0L, expressionTypePtr->size)) : 0;
			const size_t size = std::min(capacity, sourceSize);
			memset(dest, 0, sizeof(StackItem));
			if (size) memcpy(dest, src, size);
			if (sourceSize > size) abl::reportTruncated("return_truncated", capacity, sourceSize - size);
			}
		else if ((targetTypePtr == IntegerTypePtr) || (targetTypePtr->form == FRM_ENUM)) {
			//------------------------------------------------------
			// Range check assignment to integer or enum subrange...
			targetPtr->integer = tos->integer;
			}
		else {
			//-----------------------
			// Assign real to real...
			targetPtr->real = tos->real;
		}

		//-----------------------------
		// Grab the expression value...
		pop();

		//----------------------------------------------------------------------
		// Preserve the return value, in case we need it for the calling user...
		memcpy(&returnValue, targetPtr, sizeof(StackItem));
		
		if (debugger)
			debugger->traceDataStore(CurRoutineIdPtr, CurRoutineIdPtr->typePtr, targetPtr, targetTypePtr);
	}

	//-----------------------
	// Grab the semi-colon...
	getCodeToken();

	if (CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER)
		codeSegmentPtr = reinterpret_cast<char*>(ExitOrderCodeSegment);
	else if (CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_STATE)
		codeSegmentPtr = reinterpret_cast<char*>(ExitStateCodeSegment);
	else
		codeSegmentPtr = reinterpret_cast<char*>(ExitRoutineCodeSegment);
	ExitWithReturn = true;

	getCodeToken();
}

//***************************************************************************

void execStdPrint (void) {

	//---------------------------
	// Grab the opening LPAREN...
	getCodeToken();

	//----------------------------
	// Get parameter expression...
	getCodeToken();
	TypePtr paramTypePtr = execExpression();

	char buffer[20];
	char* s = buffer;
	if (paramTypePtr == IntegerTypePtr)
		mc2::fmt::to_buf(buffer, "{:d}", mc2::fmt::as_i32(tos->integer));
	else if (paramTypePtr == BooleanTypePtr)
		mc2::fmt::to_buf(buffer, "{}", mc2::fmt::safe(tos->integer ? "true" : "false"));
	else if (paramTypePtr == CharTypePtr)
		mc2::fmt::to_buf(buffer, "{:c}", mc2::fmt::as_char(tos->byte));
	else if (paramTypePtr == RealTypePtr)
		mc2::fmt::to_buf(buffer, "{:.4f}", tos->real);
	else if ((paramTypePtr->form == FRM_ARRAY) && (paramTypePtr->info.array.elementTypePtr == CharTypePtr))
		s = reinterpret_cast<char*>(tos->address);
	pop();

	if (debugger) {
		char message[512];
		mc2::fmt::to_buf(message, "PRINT:  \"{}\"", mc2::fmt::safe(s));
		debugger->print(message);
		mc2::fmt::to_buf(message, "   MODULE {}", mc2::fmt::safe(CurModule->getName()));
		debugger->print(message);
		mc2::fmt::to_buf(message, "   FILE {}", mc2::fmt::safe(CurModule->getSourceFile(FileNumber)));
		debugger->print(message);
		mc2::fmt::to_buf(message, "   LINE {:d}", mc2::fmt::as_i32(execLineNumber));
		debugger->print(message);
		}
/*	else if (TACMAP) {
		aChatWindow* chatWin = TACMAP->getChatWindow();
		if (chatWin)
			chatWin->processChatString(0, s, -1);
		else {
#ifdef _DEBUG
			mc2::fmt::debug_out(s);
#endif
		}
		}
*/	else {
#ifdef _DEBUG
		ABLDebugPrintCallback(s);
#endif
	}
	//-----------------------
	// Grab closing RPAREN...
	getCodeToken();
}

//***************************************************************************

TypePtr execStdConcat (void) {

	//-------------------
	// Grab the LPAREN...
	getCodeToken();

	//--------------------------
	// Get destination string: a char[N] variable, whose declared size bounds the append.
	const std::span<char> dest = ABLi_popCharBuf();

	//----------------------
	// Get item to append...
	getCodeToken();
	TypePtr paramTypePtr = execExpression();
	char buffer[32];
	const char* item = nullptr;
	if (paramTypePtr == IntegerTypePtr) {
		mc2::fmt::to_buf(buffer, "{:d}", mc2::fmt::as_i32(tos->integer));
		item = buffer;
		}
	else if (paramTypePtr == CharTypePtr) {
		mc2::fmt::to_buf(buffer, "{:c}", mc2::fmt::as_char(tos->byte));
		item = buffer;
		}
	else if (paramTypePtr == RealTypePtr) {
		mc2::fmt::to_buf(buffer, "{:.2f}", tos->real);
		item = buffer;
		}
	else if (paramTypePtr == BooleanTypePtr) {
		mc2::fmt::to_buf(buffer, "{}", mc2::fmt::safe(tos->integer ? "true" : "false"));
		item = buffer;
		}
	else if ((paramTypePtr->form == FRM_ARRAY) && (paramTypePtr->info.array.elementTypePtr == CharTypePtr))
		item = reinterpret_cast<char*>(tos->address);

	if (item) {
		if (const size_t dropped = abl::appendBounded(dest, item))
			abl::reportTruncated("concat_truncated", dest.size(), dropped);
	}

	tos->integer = 0;

	getCodeToken();
	return(IntegerTypePtr);
}

//***************************************************************************

void execStdAbs (void) {

	float val = ABLi_popIntegerReal();

	if (val < 0.0)
		val = -val;

	ABLi_pushReal(val);
}

//*****************************************************************************

void execStdRound (void) {

	float val = ABLi_popReal();

	if (val > 0.0)
		ABLi_pushInteger(static_cast<int>((val + 0.5)));
	else
		ABLi_pushInteger(static_cast<int>((val - 0.5)));
}

//***************************************************************************

void execStdSqrt (void) {

	float val = ABLi_popIntegerReal();

	if (val < 0.0)
		runtimeError(ABL_ERR_RUNTIME_INVALID_FUNCTION_ARGUMENT);
	else
		ABLi_pushReal(static_cast<float>(sqrt(val)));
}

//***************************************************************************

void execStdTrunc (void) {

	float val = ABLi_popReal();
	ABLi_pushInteger(static_cast<int>(val));
}

//***************************************************************************

void execStdFileOpen (void) {

	char* fileName = ABLi_popCharPtr();

	long fileHandle = -1;
	UserFile* userFile = UserFile::getNewFile();
	if (userFile) {
		long err = userFile->open(fileName);
		if (!err)
			fileHandle = userFile->handle;
	}

	ABLi_pushInteger(fileHandle);
}

//---------------------------------------------------------------------------

void execStdFileWrite (void) {

	int fileHandle = ABLi_popInteger();
	char* string = ABLi_popCharPtr();

	UserFile* userFile = UserFile::files[fileHandle];
	if (userFile->inUse)
		userFile->write(string);
}

//---------------------------------------------------------------------------

void execStdFileClose (void) {

	int fileHandle = ABLi_popInteger();

	UserFile* userFile = UserFile::files[fileHandle];
	if (userFile->inUse)
		userFile->close();
}

//***************************************************************************

void execStdGetModule (void) {

	//----------------------------------------------------------
	// Return the handle of the current module being executed...
	// The names go into the script's char[N] variables, bounded by their declared sizes.
	const std::span<char> curBuffer = ABLi_popCharBuf();
	const std::span<char> fsmBuffer = ABLi_popCharBuf();
	if (const size_t dropped = abl::copyBounded(curBuffer, abl::textOf(CurModule->getFileName())))
		abl::reportTruncated("getmodule_truncated", curBuffer.size(), dropped);
	if (const size_t dropped = abl::copyBounded(fsmBuffer, abl::textOf(CurFSM ? CurFSM->getFileName() : "none")))
		abl::reportTruncated("getmodule_truncated", fsmBuffer.size(), dropped);
	ABLi_pushInteger(CurModuleHandle);
}

//***************************************************************************

void execStdSetMaxLoops (void) {

	//----------------------------------------------------------------------
	//
	//	SET MAX LOOPS function:
	//
	//		Sets the max number of loops that may occur (in a for, while or
	//		repeat loop) before an infinite loop run-time error occurs.
	//
	//		PARAMS:	integer
	//
	//		RETURN: none
	//
	//----------------------------------------------------------------------

	MaxLoopIterations = ABLi_popInteger() + 1;
}

//---------------------------------------------------------------------------

void execStdFatal (void) {

	//----------------------------------------------------------------------
	//
	//	FATAL function:
	//
	//		If the debugger is active, this immediately jumps into debug mode.
	//		Otherwise, it causes a fatal and exits the game (displaying the
	//		string passed in).
	//
	//		PARAMS:	integer							fatal code to display
	//
	//				char[]							message
	//
	//		RETURN: none
	//
	//----------------------------------------------------------------------

	int code = ABLi_popInteger();
	char* s = ABLi_popCharPtr();

	char message[512];
	if (debugger) {
		mc2::fmt::to_buf(message, "FATAL:  [{:d}] \"{}\"", mc2::fmt::as_i32(code), mc2::fmt::safe(s));
		debugger->print(message);
		mc2::fmt::to_buf(message, "   MODULE ({:d}) {}", mc2::fmt::as_i32(CurModule->getId()), mc2::fmt::safe(CurModule->getName()));
		debugger->print(message);
		mc2::fmt::to_buf(message, "   FILE {}", mc2::fmt::safe(CurModule->getSourceFile(FileNumber)));
		debugger->print(message);
		mc2::fmt::to_buf(message, "   LINE {:d}", mc2::fmt::as_i32(execLineNumber));
		debugger->print(message);
		debugger->debugMode();
		}
	else {
		mc2::fmt::to_buf(message, "ABL FATAL: [{:d}] {}", mc2::fmt::as_i32(code), mc2::fmt::safe(s));
		ABL_Fatal(0, s);
	}
}

//---------------------------------------------------------------------------

void execStdAssert (void) {

	//----------------------------------------------------------------------
	//
	//	ASSERT function:
	//
	//		If the debugger is active, this immediately jumps into debug mode
	//		if expression is FALSE. Otherwise, the assert statement is ignored
	//		unless the #debug directive has been issued in the module. If
	//		so, a fatal occurs and exits the game (displaying the
	//		string passed in).
	//
	//		PARAMS:	boolean							expression
	//
	//				integer							assert code to display
	//
	//				char[]							message
	//
	//		RETURN: none
	//
	//----------------------------------------------------------------------

	int expression = ABLi_popInteger();
	int code = ABLi_popInteger();
	char* s = ABLi_popCharPtr();

	if (!expression) {
		char message[512];
		if (debugger) {
			mc2::fmt::to_buf(message, "ASSERT:  [{:d}] \"{}\"", mc2::fmt::as_i32(code), mc2::fmt::safe(s));
			debugger->print(message);
			mc2::fmt::to_buf(message, "   MODULE ({:d}) {}", mc2::fmt::as_i32(CurModule->getId()), mc2::fmt::safe(CurModule->getName()));
			debugger->print(message);
			mc2::fmt::to_buf(message, "   FILE {}", mc2::fmt::safe(CurModule->getSourceFile(FileNumber)));
			debugger->print(message);
			mc2::fmt::to_buf(message, "   LINE {:d}", mc2::fmt::as_i32(execLineNumber));
			debugger->print(message);
			debugger->debugMode();
			}
		else {
			mc2::fmt::to_buf(message, "ABL ASSERT: [{:d}] {}", mc2::fmt::as_i32(code), mc2::fmt::safe(s));
			ABL_Fatal(0, message);
		}
	}
}

//-----------------------------------------------------------------------------

void execStdRandom (void) {

	int n = ABLi_peekInteger();
	//---------------------------------------------------------------------
	// This is, like, a really bad number generator. But, you get the idea.
	// Once we know which pseudo-random number algorithm we want to use, we
	// should plug it in here. The range for the random number (r), given a
	// param of (n), should be: 0 <= r <= (n-1).
	// A script's random() draws from the simulation stream; the trace names the brain and script line so an extra draw in one
	// run points at the condition that reached it.
	MC2_SIM_TRACE_LINE("ABLRAND mod=%s ln=%d n=%d", CurModule ? CurModule->getName() : "?", static_cast<int>(execLineNumber), n);
	ABLi_pokeInteger(ABLRandomCallback(n));
}

//-----------------------------------------------------------------------------

void execStdSeedRandom (void) {

	int seed  = ABLi_popInteger();

	if (seed == -1)
		ABLSeedRandomCallback(static_cast<unsigned int>(time(nullptr)));
	else
		ABLSeedRandomCallback(static_cast<unsigned int>(seed));
}

//-----------------------------------------------------------------------------

void execStdResetOrders (void) {

	int scope = ABLi_popInteger();

	if (scope == 0)
		CurModule->resetOrderCallFlags();
	else if (scope == 1) {
		long startIndex = CurRoutineIdPtr->defn.info.routine.orderCallIndex;
		long endIndex = startIndex + CurRoutineIdPtr->defn.info.routine.numOrderCalls;
		for (long i = startIndex; i < endIndex; i++) {
			unsigned char orderDWord = static_cast<unsigned char>((i / 32));
			unsigned char orderBitMask = static_cast<unsigned char>((i % 32));
			CurModule->clearOrderCallFlag(orderDWord, orderBitMask);
		}
	}
}

//---------------------------------------------------------------------------

void execStdGetStateHandle (void) {

	char* name = ABLi_popCharPtr();

    int stateHandle = CurFSM->findStateHandle(S_strlwr(name));
	ABLi_pushInteger(stateHandle);
}

//---------------------------------------------------------------------------

void execStdGetCurrentStateHandle (void) {

	int stateHandle = CurFSM->getStateHandle();
	ABLi_pushInteger(stateHandle);
}
