//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLXSTD.CPP
//
//***************************************************************************

#include "ablxstd_internal.h"
#include "mc2fmt.h"
#include "sim_trace.h"


inline signed int double2long(double _in)
{
	_in+=6755399441055744.0;
	signed int result;
	memcpy(&result, &_in, sizeof(result));
	return result;
}

//----------
// ABL crash-safety gates (ABL-OPCODE-BOUNDS-HARDEN)
// MC2_ABL_ARG_GUARD=1      — enables dispatch-bounds soft path + out-ptr null guards
// MC2_ABL_RUNTIME_SOFTFAIL=1 — log-and-continue instead of ABL_Fatal (requires ARG_GUARD)
// Both default OFF: behaviour is byte-identical to pre-harden when gates are off.
bool s_ablArgGuard      = mc2::gate::get<"MC2_ABL_ARG_GUARD">();
bool s_ablRuntimeSoftfail = mc2::gate::get<"MC2_ABL_RUNTIME_SOFTFAIL">();

//--------
// GLOBALS

StackItem				returnValue;
bool					eofFlag = false;
bool					ExitWithReturn = false;
bool					ExitFromTacOrder = false;
bool					SkipOrder = false;
TokenCodeType			ExitRoutineCodeSegment[2] = {TKN_END_FUNCTION,
													 TKN_SEMICOLON};
TokenCodeType			ExitOrderCodeSegment[2] = {TKN_END_ORDER,
												   TKN_SEMICOLON};
TokenCodeType			ExitStateCodeSegment[2] = {TKN_END_STATE,
												   TKN_SEMICOLON};

//---------------------------------------------------------------------------

extern ModuleEntryPtr ModuleRegistry;

extern char	SetStateDebugStr[256];

void execStdSetState (void) {

	unsigned int stateHandle = ABLi_popInteger();

	if (stateHandle > 0) {
		SymTableNodePtr stateFunction = ModuleRegistry[CurFSM->getHandle()].stateHandles[stateHandle].state;
		CurFSM->setPrevState(CurFSM->getState());
		CurFSM->setState(stateFunction);
		mc2::fmt::to_buf(SetStateDebugStr, "{}:{}, line {:d}", mc2::fmt::safe(CurFSM->getFileName()), mc2::fmt::safe(stateFunction->name), mc2::fmt::as_i32(execLineNumber));
		NewStateSet = true;
	}
}

//---------------------------------------------------------------------------

void execStdGetFunctionHandle (void) {

    assert(0 && "function handle is pointer cannot pass through int");

	char* name = ABLi_popCharPtr();

	SymTableNodePtr function = CurModule->findFunction(name, false);
	if (function)
		ABLi_pushInteger((unsigned int)reinterpret_cast<size_t>(function));
	else
		ABLi_pushInteger(0);
}

//---------------------------------------------------------------------------

void execStdSetFlag (void) {

	unsigned int bits = (unsigned int)ABLi_popInteger();
	unsigned int flag = (unsigned int)ABLi_popInteger();
	bool set = ABLi_popBoolean();

	bits &= (flag ^ 0xFFFFFFFF);
	if (set)
		bits |= flag;

	ABLi_pushInteger(bits);
}

//---------------------------------------------------------------------------

void execStdGetFlag (void) {

	unsigned int bits = (unsigned int)ABLi_popInteger();
	unsigned int flag = (unsigned int)ABLi_popInteger();

	bool set = ((bits & flag) != 0);

	ABLi_pushInteger(set);
}

//---------------------------------------------------------------------------

void execStdCallFunction (void) {

    assert(0 && "function handle is pointer cannot pass through int, pass as address?");

	unsigned int address = ABLi_popInteger();

	if (address) {
//GLENN: Not functional, yet...
	}
}

//***************************************************************************

void initStandardRoutines (void) {

	//-------------------------------------------------------------
	// Fatal and Assert will have hardcoded keys so we can look for
	// 'em in the rest of the ABL code (example: ignore asserts if
	// the assert_off option has been set).
	enterStandardRoutine("fatal", RTN_FATAL, false, "iC", nullptr, execStdFatal);
	enterStandardRoutine("assert", RTN_ASSERT, false, "biC", nullptr, execStdAssert);
	enterStandardRoutine("getstatehandle", RTN_GET_STATE_HANDLE, false, "C", "i", execStdGetStateHandle);

	enterStandardRoutine("getcurrentstatehandle", -1, false, nullptr, "i", execStdGetCurrentStateHandle);
	enterStandardRoutine("abs", -1, false, "*", "r", execStdAbs);
	enterStandardRoutine("sqrt", -1, false, "*", "r", execStdSqrt);
	enterStandardRoutine("round", -1, false, "r", "i", execStdRound);
	enterStandardRoutine("trunc", -1, false, "r", "i", execStdTrunc);
	enterStandardRoutine("random", -1, false, "i", "i", execStdRandom);
	enterStandardRoutine("seedrandom", -1, false, "i", nullptr, execStdSeedRandom);
	enterStandardRoutine("setmaxloops", -1, false, "i", nullptr, execStdSetMaxLoops);
	enterStandardRoutine("fileopen", -1, false, "C", "i", execStdFileOpen);
	enterStandardRoutine("filewrite", -1, false, "iC", nullptr, execStdFileWrite);
	enterStandardRoutine("fileclose", -1, false, "i", nullptr, execStdFileClose);
	enterStandardRoutine("getmodule", -1, false, "CC", "i", execStdGetModule);
	enterStandardRoutine("resetorders", -1, false, "i", nullptr, execStdResetOrders);
	enterStandardRoutine("setstate", -1, false, "i", nullptr, execStdSetState);
	enterStandardRoutine("getfunctionhandle", -1, false, "C", "i", execStdGetFunctionHandle);
	enterStandardRoutine("callfunction", -1, false, "i", nullptr, execStdCallFunction);
	enterStandardRoutine("setflag", -1, false, "iib", "i", execStdSetFlag);
	enterStandardRoutine("getflag", -1, false, "ii", "b", execStdGetFlag);

}

//-----------------------------------------------------------------------------

TypePtr execStandardRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder) {

	int key = routineIdPtr->defn.info.routine.key;
	switch (key) {
		case RTN_RETURN:
			execStdReturn();
			return(nullptr);
		case RTN_PRINT:
			execStdPrint();
			return(nullptr);
		case RTN_CONCAT:
			return(execStdConcat());
		default: {
			if (key < 0 || key >= NumStandardFunctions) {
				// Mod-tolerance: ABL script called an unregistered routine key
				// (common with Omnitech-extended scripts running on our base
				// engine). ABL_Fatal is soft in Release so we'd OOB-read both
				// FunctionInfoTable[key] and FunctionCallbackTable[key] and
				// then call through a garbage pointer. Hard-bail here.
				char err[255];
				if (s_ablArgGuard && s_ablRuntimeSoftfail) {
					// H2/H3: soft-fail — log and synthesize default return, do NOT abort.
					const char* modName = CurModule ? CurModule->getName() : "(unknown)";
					const char* srcFile = (CurModule && FileNumber > -1) ? CurModule->getSourceFile(FileNumber) : "unavailable";
					mc2::fmt::to_buf(err, "[ABL_SOFTFAIL] key={:d} func=(out-of-range) module={} file={} line={:d} errCode=0 — key >= NumStandardFunctions({:d}), skipping", mc2::fmt::as_i32(key), mc2::fmt::safe(modName), mc2::fmt::safe(srcFile), mc2::fmt::as_i32(execLineNumber), mc2::fmt::as_i32(NumStandardFunctions));
					mc2::fmt::print_out("{}\n", mc2::fmt::safe(err)); fflush(stdout);
					return nullptr;
				}
				mc2::fmt::to_buf(err, " ABL: Undefined ABL RoutineKey {:d} in {}:{:d} (>= {:d}) — bailing", mc2::fmt::as_i32(key), mc2::fmt::safe(CurModule->getName()), mc2::fmt::as_i32(execLineNumber), mc2::fmt::as_i32(NumStandardFunctions));
				ABL_Fatal(0, err);
				return nullptr;
			}
			if (FunctionInfoTable[key].numParams > 0)
				getCodeToken();
			SkipOrder = skipOrder;
			// Guard: FunctionCallbackTable slots can hold non-NULL uninitialized
			// garbage (e.g. 0x00000000DDFD690B — low 16 bits repeat across runs,
			// upper bits are heap noise). NULL check alone is insufficient. Valid
			// code pointers on 64-bit Windows live in the high 2^47 range
			// (0x00007F...); anything below 0x0000_7F00_0000_0000 is bogus. On
			// bogus, skip the call, log key+module+line, synthesize a default
			// return so ABL stack stays balanced and FSM execution continues.
			{
				void (*cb)(void) = FunctionCallbackTable[key];
				uintptr_t cbVal = (uintptr_t)cb;
				// Belt-and-suspenders: root cause was MAX_STANDARD_FUNCTIONS=256
				// overflow (fixed in 2cfad76). This guard stays in place to catch
				// any future bogus-pointer case rather than dispatching into
				// garbage. Log is unconditional because it fires rarely; when it
				// does, we want the data in o.log automatically.
				if (cbVal != 0 && cbVal < (uintptr_t)0x00007F0000000000ULL) {
					char err[255];
					mc2::fmt::to_buf(err, "[ABL_BAD_CB] key={:d} val={:016X} module={} line={:d}", mc2::fmt::as_i32(key), mc2::fmt::as_ptr(cb), mc2::fmt::safe(CurModule->getName()), mc2::fmt::as_i32(execLineNumber));
					mc2::fmt::print_out("{}\n", mc2::fmt::safe(err)); fflush(stdout);
					switch (FunctionInfoTable[key].returnType) {
						case RETURN_TYPE_INTEGER: pushInteger(0); break;
						case RETURN_TYPE_REAL:    pushReal(0.0f); break;
						case RETURN_TYPE_BOOLEAN: pushBoolean(false); break;
						default: break;
					}
				} else if (cb) {
					MC2_SIM_TRACE_LINE("ABL mod=%s ln=%d fn=%s", CurModule ? CurModule->getName() : "?", static_cast<int>(execLineNumber), routineIdPtr->name);
					(*cb)();
				} else {
					char err[255];
					mc2::fmt::to_buf(err, " ABL: Undefined ABL RoutineKey {:d} in {}:{:d}", mc2::fmt::as_i32(key), mc2::fmt::safe(CurModule->getName()), mc2::fmt::as_i32(execLineNumber));
					ABL_Fatal(key, err);
				}
			}

			getCodeToken();
			switch (FunctionInfoTable[key].returnType) {
				case RETURN_TYPE_NONE:
					return(nullptr);
				case RETURN_TYPE_INTEGER:
					return(IntegerTypePtr);
				case RETURN_TYPE_REAL:
					return(RealTypePtr);
				case RETURN_TYPE_BOOLEAN:
					return(BooleanTypePtr);
                default:;
			}
		}
	}
	return(nullptr);
}
