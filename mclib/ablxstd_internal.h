// Shared surface of the ablxstd.cpp translation units (ablxstd.cpp, ablxstd_{stack,std}.cpp).
//
// ablxstd.cpp keeps the state / flag / function-call routines, the routine registration and the standard-routine dispatch;
// the interpreter-stack accessors and the general standard routines are split out (see DOC in each piece).
#ifndef ABLXSTD_INTERNAL_H
#define ABLXSTD_INTERNAL_H

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<cassert>
#include"platform_str.h"
#include<math.h>
#include<time.h>
#include "Gates/gate.h"

#ifndef ABL_H
#include"abl.h"
#endif

// Globals defined in ablxstd.cpp (or elsewhere in the engine) that the split pieces share.
extern int32_t          level;
extern int32_t          FileNumber;
extern int              execLineNumber;
extern char*			codeSegmentPtr;
extern TokenCodeType	codeToken;
extern StackItem*		stack;
extern StackItemPtr		tos;
extern StackItemPtr		stackFrameBasePtr;
extern SymTableNodePtr	CurRoutineIdPtr;
extern long				CurModuleHandle;
extern TypePtr			IntegerTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;
extern TypePtr			CharTypePtr;
extern ABLModulePtr		CurModule;
extern ABLModulePtr		CurFSM;
extern long	MaxLoopIterations;
extern DebuggerPtr		debugger;
extern bool				NewStateSet;
extern bool s_ablArgGuard;
extern bool s_ablRuntimeSoftfail;
extern StackItem returnValue;
extern bool eofFlag;
extern bool ExitWithReturn;
extern bool ExitFromTacOrder;
extern bool SkipOrder;
extern TokenCodeType ExitRoutineCodeSegment[2];
extern TokenCodeType ExitOrderCodeSegment[2];
extern TokenCodeType ExitStateCodeSegment[2];

// Free functions defined in the split pieces.
void execOrderReturn (long returnVal);
void execStdReturn (void);
void execStdPrint (void);
TypePtr execStdConcat (void);
void execStdAbs (void);
void execStdRound (void);
void execStdSqrt (void);
void execStdTrunc (void);
void execStdFileOpen (void);
void execStdFileWrite (void);
void execStdFileClose (void);
void execStdGetModule (void);
void execStdSetMaxLoops (void);
void execStdFatal (void);
void execStdAssert (void);
void execStdRandom (void);
void execStdSeedRandom (void);
void execStdResetOrders (void);
void execStdGetStateHandle (void);
void execStdGetCurrentStateHandle (void);

#endif // ABLXSTD_INTERNAL_H
