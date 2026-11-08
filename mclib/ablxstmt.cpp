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
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "abl_script_events.h"
#include "abl_string_append.h"

#ifndef ABLGEN_H
#include"ablgen.h"
#include "gos_crashbundle.h"
#endif

#ifndef ABLERR_H
#include"ablerr.h"
#endif

#ifndef ABLSCAN_H
#include"ablscan.h"
#endif

#ifndef ABLSYMT_H
#include"ablsymt.h"
#endif

#ifndef ABLPARSE_H
#include"ablparse.h"
#endif

#ifndef ABLEXEC_H
#include"ablexec.h"
#endif

#ifndef ABLDBUG_H
#include"abldbug.h"
#endif

// ABL-VM-FACTS-1: peak call-depth hook, defined in code/warrior.cpp. Bare
// forward decl keeps mclib free of the code/ + GameOS diag layer. The impl
// is byte-identical (no-op) when the ABL diag tag is OFF.
void mc2AblNotePeakDepth(int level);

//***************************************************************************

//----------
// EXTERNALS

extern int32_t          NumExecutions;

extern StackItem*		stack;

extern TypePtr			IntegerTypePtr;
extern TypePtr			CharTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;


extern long				MaxLoopIterations;

extern DebuggerPtr		debugger;
extern SymTableNodePtr	CurModuleIdPtr;
extern ModuleEntryPtr	ModuleRegistry;
extern ABLModulePtr*	ModuleInstanceRegistry;
extern ABLModulePtr		CurLibrary;
extern long				ProfileLogFunctionTimeLimit;
extern ABLFile*			ProfileLog;

long	dummyCount = 0;

void execOrderReturn (long returnVal);
void ABL_AddToProfileLog (char* profileString);
void transState (SymTableNodePtr newState);

//***************************************************************************
//
//***************************************************************************

void execStatement (void) {

	if (g_ablExec->codeToken == TKN_STATEMENT_MARKER) {
		g_ablExec->execLineNumber = getCodeStatementMarker();
		g_ablExec->execStatementCount++;

		g_ablExec->statementStartPtr = g_ablExec->codeSegmentPtr;

		if (debugger)
			debugger->traceStatementExecution();

		getCodeToken();
	}

	switch (g_ablExec->codeToken) {
		case TKN_IDENTIFIER: {
			SymTableNodePtr idPtr = getCodeSymTableNodePtr();
			ABL_Assert(idPtr != nullptr, 0, " oops ");
			// Tier-1 crash guard: ABL_Assert is print-and-continue in release.
			// If the parser bailed on a syntax error earlier, the symbol table
			// may have a NULL slot here; executing its .defn.key dereferences
			// a NULL pointer and crashes. Treat a NULL idPtr as a no-op
			// statement so the rest of the module (and the game) can proceed.
			if (!idPtr) {
				static const bool s_ablTrace = mc2::gate::get<"MC2_ABL_TRACE">();
				if (s_ablTrace) {
					crashbundle_printf<512>("", "[ABL_EXEC] NULL idPtr at TKN_IDENTIFIER; skipping statement");
				}
				break;
			}
			if (idPtr->defn.key == DFN_FUNCTION) {
				bool skipOrder = false;
				unsigned char orderDWord = 0;
				unsigned char orderBitMask = 0;
				if ((idPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER) && g_ablExec->CurModule->getOrderCallFlags()) {
					orderDWord = getCodeByte();
					orderBitMask = getCodeByte();
					skipOrder = !g_ablExec->CurModule->isLibrary() && g_ablExec->CurModule->getOrderCallFlag(orderDWord, orderBitMask);
				}
				TypePtr returnType = execRoutineCall(idPtr, skipOrder);
				if (idPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER) {
					if (g_ablExec->AutoReturnFromOrders) {
						//-----------------------------------------------------------------
						// We called an Order function, and we're in an Orders/State block,
						// so do we continue the flow of orders or stop here?
						int returnVal = g_ablExec->tos->integer;
						pop();
						if (returnVal == 0)
							execOrderReturn(returnVal);
						else if (g_ablExec->CurModule->getOrderCallFlags()) {
							g_ablExec->CurModule->setOrderCallFlag(orderDWord, orderBitMask);
						}
					}
					}
				else if (returnType) {
						//------------------------------------------
						// In case this routine returns a value, pop
						// the return value off the stack...
						pop();
				}
				}
			else
				execAssignmentStatement(idPtr);
			}
			break;
		case TKN_CODE: {
			bool wasAutoReturnFromOrders = g_ablExec->AutoReturnFromOrders;
			g_ablExec->AutoReturnFromOrders = ((g_ablExec->CurRoutineIdPtr->defn.info.routine.flags & (ROUTINE_FLAG_ORDER + ROUTINE_FLAG_STATE)) != 0);
			getCodeToken();
			TokenCodeType endToken = TKN_END_FUNCTION;
			if (g_ablExec->CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER)
				endToken = TKN_END_ORDER;
			else if (g_ablExec->CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_STATE)
				endToken = TKN_END_STATE;
			TokenCodeType endTokenFinal = TKN_END_MODULE;
			if (CurLibrary)
				endTokenFinal = TKN_END_LIBRARY;
			else if (g_ablExec->CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM)
				endTokenFinal = TKN_END_FSM;
			while ((g_ablExec->codeToken != endToken) && (g_ablExec->codeToken != endTokenFinal) && !g_ablExec->NewStateSet)
				execStatement();
			if (g_ablExec->NewStateSet)
				return;
			getCodeToken();
			g_ablExec->AutoReturnFromOrders = wasAutoReturnFromOrders;
			}
			break;
		case TKN_FOR:
			execForStatement();
			break;
		case TKN_IF:
			execIfStatement();
			break;
		case TKN_REPEAT:
			execRepeatStatement();
			break;
		case TKN_WHILE:
			execWhileStatement();
			break;
		case TKN_SWITCH:
			execSwitchStatement();
			break;
		case TKN_TRANS:
			execTransStatement();
			break;
		case TKN_TRANS_BACK:
			execTransBackStatement();
			break;
		case TKN_SEMICOLON:
		case TKN_ELSE:
		case TKN_UNTIL:
			break;
		default:
			runtimeError(ABL_ERR_RUNTIME_UNIMPLEMENTED_FEATURE);
	}

	while (g_ablExec->codeToken == TKN_SEMICOLON)
		getCodeToken();
}

//***************************************************************************

void execAssignmentStatement (SymTableNodePtr idPtr) {

	StackItemPtr targetPtr;
	TypePtr targetTypePtr;
	TypePtr expressionTypePtr;

	//--------------------------
	// Assignment to variable...
	targetTypePtr = execVariable(idPtr, USE_TARGET);
	targetPtr = reinterpret_cast<StackItemPtr>(g_ablExec->tos->address);

	//------------------------------
	// Pop off the target address...
	pop();
	
	//------------------------
	// Pop the size, if nec...
	//if (targetTypePtr->form == FRM_ARRAY)
	//	pop();

	//---------------------------------------------------------------
	// Routine execExpression() leaves the expression value on top of
	// stack...
	getCodeToken();
	expressionTypePtr = execExpression();

	//--------------------------
	// Now, do the assignment...
	// OMNITECH-ABL crash guard: malformed bytecode from a STOPSYNTAX module can leave
	// targetTypePtr/targetPtr/tos NULL (execVariable failed) -> the assignment derefs crash
	// (execAssignmentStatement, ablxstmt.cpp). Skip the store when invalid; pop() below still
	// balances the stack. Valid bytecode is unaffected.
	if (targetTypePtr && targetPtr && g_ablExec->tos) {
	if ((targetTypePtr == RealTypePtr) && (expressionTypePtr == IntegerTypePtr)) {
		//-------------------------
		// integer assigned to real
		targetPtr->real = static_cast<float>((g_ablExec->tos->integer));
		}
	else if (targetTypePtr->form == FRM_ARRAY) {
		//-------------------------
		// Copy the array/record...
		char* dest = reinterpret_cast<char*>(targetPtr);
		char* src = g_ablExec->tos->address;
		// Copy no more than the source holds: a literal shorter than the variable must not be read past its end.
		const int srclen = static_cast<int>(strlen(src) + 1);
		long size = targetTypePtr->size;
		if (targetTypePtr->info.array.elementTypePtr == CharTypePtr) {
			// A string longer than the variable is cut and stays terminated: the text is read later with strlen.
			if (const size_t dropped = abl::copyBounded(std::span<char>(dest, static_cast<size_t>(size)), std::string_view(src)))
				abl::reportTruncated("assign_truncated", static_cast<size_t>(size), dropped);
			}
		else
			memcpy(dest, src, /*size*/srclen<size?srclen:size);
		}
	else if ((targetTypePtr == IntegerTypePtr) || (targetTypePtr->form == FRM_ENUM)) {
		//------------------------------------------------------
		// Range check assignment to integer or enum subrange...
		targetPtr->integer = g_ablExec->tos->integer;
		}
	else if (targetTypePtr == CharTypePtr)
		targetPtr->byte = g_ablExec->tos->byte;
	else {
		//-----------------------
		// Assign real to real...
		targetPtr->real = g_ablExec->tos->real;
	}
	}  // OMNITECH-ABL crash guard (targetTypePtr && targetPtr && tos)

	//-----------------------------
	// Grab the expression value...
	pop();

	if (debugger)
		debugger->traceDataStore(idPtr, idPtr->typePtr, targetPtr, targetTypePtr);
}

//***************************************************************************

TypePtr execRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder) {

	if (routineIdPtr->defn.info.routine.key == RTN_DECLARED)
		return(execDeclaredRoutineCall(routineIdPtr, skipOrder));
	else
		return(execStandardRoutineCall(routineIdPtr, skipOrder));
}

//***************************************************************************

TypePtr execDeclaredRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder) {

	if (skipOrder) {
		StackItemPtr curStackFrameBase = g_ablExec->tos;
		//----------------------------------------
		// Push parameter values onto the stack...
		getCodeToken();
		if (g_ablExec->codeToken == TKN_LPAREN) {
			execActualParams(routineIdPtr);
			getCodeToken();
		}
		getCodeToken();
		g_ablExec->tos = curStackFrameBase;
		pushInteger(1);
		return(routineIdPtr->typePtr);
	}

	long oldLevel = g_ablExec->level;						// level of caller
	long newLevel = routineIdPtr->level + 1;	// level of callee
	g_ablExec->CallStackLevel++;
	mc2AblNotePeakDepth(g_ablExec->CallStackLevel);  // ABL-VM-FACTS-1 (no-op when OFF)

	//-------------------------------------------
	// First, set up the stack frame of callee...
	StackItemPtr newStackFrameBasePtr = g_ablExec->tos + 1;
	bool isLibraryCall = (routineIdPtr->library && (routineIdPtr->library != g_ablExec->CurRoutineIdPtr->library));
	if (isLibraryCall)
		pushStackFrameHeader(-1, -1);
	else
		pushStackFrameHeader(oldLevel, newLevel);

	//----------------------------------------
	// Push parameter values onto the stack...
	getCodeToken();
	if (g_ablExec->codeToken == TKN_LPAREN) {
		execActualParams(routineIdPtr);
		getCodeToken();
	}

	//-------------------------------------------------
	// Set the return address in the new stack frame...
	g_ablExec->level = newLevel;
	g_ablExec->stackFrameBasePtr = newStackFrameBasePtr;
	StackFrameHeaderPtr headerPtr = reinterpret_cast<StackFrameHeaderPtr>(g_ablExec->stackFrameBasePtr);
	headerPtr->returnAddress.address = g_ablExec->codeSegmentPtr - 1;

	//---------------------------------------------------------
	// If we're calling a library function, we need to set some
	// module-specific info...
	ABLModulePtr PrevModule = nullptr;
	if (isLibraryCall) {
		PrevModule = g_ablExec->CurModule;
		g_ablExec->CurModule = routineIdPtr->library;
		g_ablExec->CurModuleHandle = g_ablExec->CurModule->getHandle();
		if (debugger)
			debugger->setModule(g_ablExec->CurModule);
		g_ablExec->StaticDataPtr = g_ablExec->CurModule->getStaticData();
		g_ablExec->CallModuleInit = !g_ablExec->CurModule->getInitCalled();
		g_ablExec->CurModule->setInitCalled(true);
	//	routineEntry(ModuleRegistry[CurModule->getHandle()].moduleIdPtr);
	}

	if (ProfileLog) {
		long functionStartTime = ABLGetTimeCallback();
		execute(routineIdPtr);
		long functionExecTime = ABLGetTimeCallback() - functionStartTime;

		if (functionExecTime > ProfileLogFunctionTimeLimit) {
			char s[512];
			mc2::fmt::to_buf(s, "[{:08d}] ", mc2::fmt::as_i32(NumExecutions));
			for (long i = 0; i < g_ablExec->CallStackLevel; i++)
				strcat(s, " ");
			char s1[512];
			mc2::fmt::to_buf(s1, "{} ({:d})\n", mc2::fmt::safe(routineIdPtr->name), mc2::fmt::as_i32(functionExecTime));
			strcat(s, s1);
			ABL_AddToProfileLog(s);
		}
		}
	else
		execute(routineIdPtr);

	//----------------------------------------------------------------
	// If we're calling a library function, reset some module-specific
	// info...
	if (isLibraryCall) {
	//	routineExit(ModuleRegistry[CurModule->getHandle()].moduleIdPtr);
		g_ablExec->CurModule = PrevModule;
		g_ablExec->CurModuleHandle = g_ablExec->CurModule->getHandle();
		if (debugger)
			debugger->setModule(g_ablExec->CurModule);
		g_ablExec->StaticDataPtr = g_ablExec->CurModule->getStaticData();
	}

	//-------------------------------------------------------
	// Return from the callee, and grab the first token after
	// the return...
	g_ablExec->level = oldLevel;
	getCodeToken();
	g_ablExec->CallStackLevel--;

	return(routineIdPtr->typePtr);
}

//***************************************************************************

void setOpenArray (TypePtr arrayTypePtr, long size) {

	long numElements = size / arrayTypePtr->size;
	arrayTypePtr->size = size;
	while (arrayTypePtr->info.array.elementTypePtr->form == FRM_ARRAY)
		arrayTypePtr = arrayTypePtr->info.array.elementTypePtr;
	arrayTypePtr->info.array.elementCount = numElements;
}

//***************************************************************************

void execActualParams (SymTableNodePtr routineIdPtr) {

	//--------------------------
	// Execute the parameters...

	for (SymTableNodePtr formalIdPtr = routineIdPtr->defn.info.routine.params;
		 formalIdPtr != nullptr;
		 formalIdPtr = formalIdPtr->next) {

		TypePtr formalTypePtr = formalIdPtr->typePtr;
		getCodeToken();

		if (formalIdPtr->defn.key == DFN_VALPARAM) {
			//-------------------
			// pass by value parameter...
			TypePtr actualTypePtr = execExpression();

			if ((formalTypePtr == RealTypePtr) && (actualTypePtr == IntegerTypePtr)) {
				//---------------------------------------------
				// Real formal parameter, but integer actual...
				g_ablExec->tos->real = static_cast<float>((g_ablExec->tos->integer));
			}

			//----------------------------------------------------------
			// Formal parameter is an array or record, so make a copy...
			if (formalTypePtr->form == FRM_ARRAY/* || (formalTypePtr->form == FRM_RECORD)*/) {
				long size = formalTypePtr->size;
				char* src = g_ablExec->tos->address;
				char* dest = static_cast<char*>(ABL_alloc(ABLPool::Stack, static_cast<size_t>(size)));
				if (!dest) {
					char err[255];
					mc2::fmt::to_buf(err, " ABL: Unable to AblStackHeap->malloc actual array param in module {})", mc2::fmt::safe(g_ablExec->CurModule->getName()));
					ABL_Fatal(0, err);
				}
				// The compile check accepts any char array no larger than the formal as the actual, so copy what the actual declares and zero the
				// rest: the formal must not read past the actual's block. An actual larger than the formal is cut and terminated.
				const long actualSize = (actualTypePtr && actualTypePtr->form == FRM_ARRAY) ? actualTypePtr->size : 0;
				const long copySize = (actualSize < size) ? actualSize : size;
				memcpy(dest, src, static_cast<size_t>(copySize));
				memset(dest + copySize, 0, static_cast<size_t>(size - copySize));
				if (size > 0 && actualSize > size) {
					dest[size - 1] = '\0';
					abl::reportTruncated("array_param_truncated", static_cast<size_t>(size), static_cast<size_t>(actualSize - size));
				}
				char* savePtr = dest;
				g_ablExec->tos->address = savePtr;
			}
			}
		else {
			//-------------------------------
			// pass by reference parameter...
			SymTableNodePtr idPtr = getCodeSymTableNodePtr();
			execVariable(idPtr, USE_REFPARAM);
		}
	}
}

//***************************************************************************

void execSwitchStatement (void) {

	getCodeToken();
		
	char* branchTableLocation = getCodeAddressMarker();

	getCodeToken();
	TypePtr switchExpressionTypePtr = execExpression();

	int switchExpressionValue;
	if ((switchExpressionTypePtr == IntegerTypePtr) || (switchExpressionTypePtr->form == FRM_ENUM))
		switchExpressionValue = g_ablExec->tos->integer;
	else
		switchExpressionValue = g_ablExec->tos->byte;
	pop();

	//---------------------------------------------------------
	// Now, search the branch table for the expression value...
	g_ablExec->codeSegmentPtr = branchTableLocation;
	getCodeToken();
	int caseLabelCount = getCodeInteger();
	bool done = false;
	char* caseBranchLocation = nullptr;
	while (!done && caseLabelCount--) {
		int caseLabelValue = getCodeInteger();
		caseBranchLocation = getCodeAddress();
		done = (caseLabelValue == switchExpressionValue);
	}

	//-----------------------------------------------
	// If found, go to the aprropriate branch code...
	if (caseLabelCount >= 0) {
		g_ablExec->codeSegmentPtr = caseBranchLocation;
		getCodeToken();

		if (g_ablExec->codeToken != TKN_END_CASE)
			do {
				execStatement();
				if (g_ablExec->ExitWithReturn)
					return;
			} while (g_ablExec->codeToken != TKN_END_CASE);
		

		//----------------------------------
		// Grab the end case and semi-colon...
		getCodeToken();
		getCodeToken();
		g_ablExec->codeSegmentPtr = getCodeAddressMarker();
		getCodeToken();
		}
	else {
		//-----------------------------------------------------------------
		// Since the branch table is located at the end of the case blocks,
		// the code directly after the switch statement follows our
		// current code location, already. Just grab the endswitch
		// and semi-colon...
		getCodeToken();
		getCodeToken();
	}
}

//***************************************************************************

void execForStatement (void) {

	getCodeToken();

	//---------------------------------------
	// Grab address of the end of the loop...
	char* loopEndLocation = getCodeAddressMarker();

	//--------------------------------------------------------
	// Get the address of the control variable's stack item...
	getCodeToken();
	SymTableNodePtr controlIdPtr = getCodeSymTableNodePtr();
	TypePtr controlTypePtr = execVariable(controlIdPtr, USE_TARGET);
	StackItemPtr targetPtr = reinterpret_cast<StackItemPtr>(g_ablExec->tos->address);
	
	//------------------------------------
	// Control variable address...
	pop();

	//-------------------------------
	// Eval the initial expression...
	getCodeToken();
	execExpression();
	int initialValue;
	if (controlTypePtr == IntegerTypePtr)
		initialValue = g_ablExec->tos->integer;
	else
		initialValue = g_ablExec->tos->byte;
	//---------------------
	// The initial value...
	pop();

	int deltaValue;
	if (g_ablExec->codeToken == TKN_TO)
		deltaValue = 1;
	else
		deltaValue = -1;

	//----------------------------------
	// Now, eval the final expression...
	getCodeToken();
	execExpression();
	int finalValue;
	if (controlTypePtr == IntegerTypePtr)
		finalValue = g_ablExec->tos->integer;
	else
		finalValue = g_ablExec->tos->byte;
	//-------------------
	// The final value...
	pop();

	//----------------------------
	// Address of start of loop...
	char* loopStartLocation = g_ablExec->codeSegmentPtr;

	int controlValue = initialValue;

	//-----------------------------
	// Now, execute the FOR loop...
	int iterations = 0;
	if (deltaValue == 1)
		while (controlValue <= finalValue) {
			if (controlTypePtr == IntegerTypePtr)
				targetPtr->integer = controlValue;
			else
				targetPtr->byte = static_cast<unsigned char>(controlValue);

			getCodeToken();

			if (g_ablExec->codeToken != TKN_END_FOR)
				do {
					execStatement();
					if (g_ablExec->ExitWithReturn)
						return;
				} while (g_ablExec->codeToken != TKN_END_FOR);

			//---------------------------
			// Check for infinite loop...
			if (++iterations == MaxLoopIterations)
				runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);

			controlValue++;
			g_ablExec->codeSegmentPtr = loopStartLocation;
		}
	else
		while (controlValue >= finalValue) {
			if (controlTypePtr == IntegerTypePtr)
				targetPtr->integer = controlValue;
			else
				targetPtr->byte = static_cast<unsigned char>(controlValue);

			getCodeToken();

			if (g_ablExec->codeToken != TKN_END_FOR)
				do {
					execStatement();
					if (g_ablExec->ExitWithReturn)
						return;
				} while (g_ablExec->codeToken != TKN_END_FOR);

			//---------------------------
			// Check for infinite loop...
			if (++iterations == MaxLoopIterations)
				runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);
			
			controlValue--;
			g_ablExec->codeSegmentPtr = loopStartLocation;
		}

	g_ablExec->codeSegmentPtr = loopEndLocation;
	getCodeToken();
}

//***************************************************************************

void execTransStatement (void) {

	getCodeToken();
	getCodeToken();
	SymTableNodePtr idPtr = getCodeSymTableNodePtr();
	transState(idPtr);
	getCodeToken();
}

//***************************************************************************

void execTransBackStatement (void) {

	SymTableNodePtr prevState = g_ablExec->CurModule->getPrevState();
	if (!prevState)
		runtimeError(ABL_ERR_RUNTIME_NULL_PREVSTATE);
	transState(prevState);
	getCodeToken();
}

//***************************************************************************

void execIfStatement (void) {

	getCodeToken();

	char* falseLocation = getCodeAddressMarker();

	//-------------------------------
	// Eval the boolean expression. Note that, unlike C/C++, the expression
	// must be true(1) or false(0). In C/C++, an expression is true if it's
	// non-zero. Not the case in ABL using this current implementation. Do we
	// want to change this?
	getCodeToken();
	execExpression();
	bool test = (g_ablExec->tos->integer == 1);
	pop();

	if (test) {
		//---------------------------
		// execute the TRUE branch...
		getCodeToken();

		if ((g_ablExec->codeToken != TKN_END_IF) && (g_ablExec->codeToken != TKN_ELSE))
			do {
				execStatement();
				if (g_ablExec->ExitWithReturn)
					return;
			} while ((g_ablExec->codeToken != TKN_END_IF) && (g_ablExec->codeToken != TKN_ELSE));
		
		if (g_ablExec->codeToken == TKN_ELSE) {
			getCodeToken();
			g_ablExec->codeSegmentPtr = getCodeAddressMarker();
			getCodeToken();
		}
		}
	else {
		//----------------------------
		// Execute the FALSE branch...
		g_ablExec->codeSegmentPtr = falseLocation;
		getCodeToken();

		if (g_ablExec->codeToken == TKN_ELSE) {
			getCodeToken();
			getCodeAddressMarker();

			getCodeToken();

			if (g_ablExec->codeToken != TKN_END_IF)
				do {
					execStatement();
					if (g_ablExec->ExitWithReturn)
						return;
				} while (g_ablExec->codeToken != TKN_END_IF);
		}
	}
	getCodeToken();
}

//***************************************************************************

void execRepeatStatement (void) {

	char* loopStartLocation = g_ablExec->codeSegmentPtr;

	int iterations = 0;
	do {
		getCodeToken();

		if (g_ablExec->codeToken != TKN_UNTIL)
			do {
				execStatement();
				if (g_ablExec->ExitWithReturn)
					return;
			} while (g_ablExec->codeToken != TKN_UNTIL);

		//---------------------------
		// Check for infinite loop...
		iterations++;
		if (iterations == MaxLoopIterations)
			runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);

		//-------------------------------
		// Eval the boolean expression...
		getCodeToken();
		execExpression();
		if (g_ablExec->tos->integer == 0)
			g_ablExec->codeSegmentPtr = loopStartLocation;

		//--------------------------
		// Grab the boolean value...
		pop();
	} while (g_ablExec->codeSegmentPtr == loopStartLocation);
}

//***************************************************************************

void execWhileStatement (void) {

	getCodeToken();
	char* loopEndLocation = getCodeAddressMarker();
	char* testLocation = g_ablExec->codeSegmentPtr;

	bool loopDone = false;
	int iterations = 0;
	do {
		//-------------------------------
		// Eval the boolean expression...
		getCodeToken();
		execExpression();
		if (g_ablExec->tos->integer == 0) {
			g_ablExec->codeSegmentPtr = loopEndLocation;
			loopDone = true;
		}
		
		//-------------------------
		// Get the boolean value...
		pop();

		//----------------------------------
		// If TRUE, execute the statement...
		if (!loopDone) {
			getCodeToken();

			if (g_ablExec->codeToken != TKN_END_WHILE)
				do {
					execStatement();
					if (g_ablExec->ExitWithReturn)
						return;
				} while (g_ablExec->codeToken != TKN_END_WHILE);

			g_ablExec->codeSegmentPtr = testLocation;

			//---------------------------
			// Check for infinite loop...
			iterations++;
			if (iterations == MaxLoopIterations)
				runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);
		}
	} while (!loopDone);

	getCodeToken();
}

//***************************************************************************
