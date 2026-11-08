//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								EXECUTOR.H
//
//***************************************************************************

#ifndef ABLEXEC_H
#define ABLEXEC_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLSYMT_H
#include"ablsymt.h"
#endif

#ifndef ABLPARS_H
#include"ablparse.h"
#endif

//***************************************************************************

inline constexpr int STATEMENT_MARKER = 0x70;
inline constexpr int ADDRESS_MARKER = 0x71;

//***************************************************************************

//--------------
// ABL Parameter

inline constexpr int ABL_PARAM_VOID = 0;
inline constexpr int ABL_PARAM_INTEGER = 1;
inline constexpr int ABL_PARAM_REAL = 2;

// Typed label only: ABLStackItem::type stays a long (same values, same layout), so callers
// convert with std::to_underlying / static_cast<ABLStackItemType> at that boundary.
enum class ABLStackItemType : long {
	ABL_STACKITEM_CHAR,
	ABL_STACKITEM_INTEGER,
	ABL_STACKITEM_REAL,
	ABL_STACKITEM_BOOLEAN,
	ABL_STACKITEM_CHAR_PTR,
	ABL_STACKITEM_INTEGER_PTR,
	ABL_STACKITEM_REAL_PTR,
	ABL_STACKITEM_BOOLEAN_PTR,
	NUM_ABL_STACKITEMS
};

struct ABLParam {
	char		type;
	int         integer;
	float		real;
};

using ABLParamPtr = ABLParam*;

//---------------
// RUN-TIME STACK

union StackItem {
	int             integer;
	float			real;
	unsigned char	byte;
	Address			address;
};

using StackItemPtr = StackItem*;

// Typed-address float access: the stack and static data hold a float at a byte Address, read and written as bytes.
inline float ablLoadFloat(const void* p) noexcept {
	float v;
	std::memcpy(&v, p, sizeof v);
	return v;
}

inline void ablStoreFloat(void* p, float v) noexcept {
	std::memcpy(p, &v, sizeof v);
}

struct ABLStackItem {
	long			type;	// an ABLStackItemType value
	union {
		int         integer;
		float		real;
		bool		boolean;
		char		character;
		int*		integerPtr;
		float*		realPtr;
		bool*		booleanPtr;
		char*		characterPtr;
	} data;
};

using ABLStackItemPtr = ABLStackItem*;

struct StackFrameHeader {
	StackItem	functionValue;
	StackItem	staticLink;
	StackItem	dynamicLink;
	StackItem	returnAddress;
};

using StackFrameHeaderPtr = StackFrameHeader*;

//***************************************************************************

#if 0

//class ABLmodule {

//	public:

//		SymTableNodePtr		moduleIdPtr;


//	public:


//};

#endif

//***************************************************************************

extern char*			codeBuffer;
extern char*			codeBufferPtr;
extern StackItem*		stack;	// environment-owned root stack; eternal variables live in [0, eternalOffset)

// The executor state of one ABL run: code cursor, stack, current module/routine, static data base and the
// exit/error flags. The interpreter reads the context bound to g_ablExec. A host call that runs a second
// module binds its own context with AblExecScope, so the caller's run resumes untouched when it returns.
// Eternal variables stay in the root stack and are shared by every context.
struct AblExecContext {
	AblExecContext () = default;
	explicit AblExecContext (std::size_t ownStackItems) : ownStack(ownStackItems) {}
	AblExecContext (const AblExecContext&) = delete;
	AblExecContext& operator= (const AblExecContext&) = delete;

	// Code cursor.
	char*			codeSegmentPtr = nullptr;
	char*			codeSegmentLimit = nullptr;
	char*			statementStartPtr = nullptr;
	TokenCodeType	codeToken{};
	int				execLineNumber = 0;
	int				execStatementCount = 0;
	// Stack.
	StackItemPtr	tos = nullptr;
	StackItemPtr	stackFrameBasePtr = nullptr;
	StackItemPtr	stackLimit = nullptr;	// one past the last usable item
	// Current module, routine and static data.
	StackItemPtr	StaticDataPtr = nullptr;
	unsigned long*	OrderCompletionFlags = nullptr;
	ABLModulePtr	CurModule = nullptr;
	long			CurModuleHandle = 0;
	ABLModulePtr	CurFSM = nullptr;
	SymTableNodePtr	CurRoutineIdPtr = nullptr;
	int32_t			level = 0;
	int32_t			CallStackLevel = 0;
	int32_t			NumStateTransitions = 0;
	bool			CallModuleInit = false;
	bool			NewStateSet = false;
	bool			AutoReturnFromOrders = false;
	// Exit state and result.
	bool			ExitWithReturn = false;
	bool			ExitFromTacOrder = false;
	StackItem		returnValue{};

	// Empty: the context runs on the root stack above the eternals.
	std::vector<StackItem>	ownStack;

	// Points tos and the frame base at the bottom of this context's stack (an entry frame starts here).
	void resetStack ();
};

extern AblExecContext*	g_ablExec;	// the bound context; the root context unless an AblExecScope is live

// Binds a context for the scope (a host call running another module) and restores the caller's on exit.
class AblExecScope {
public:
	explicit AblExecScope (AblExecContext& context) noexcept : previous(g_ablExec) { g_ablExec = &context; }
	~AblExecScope () { g_ablExec = previous; }
	AblExecScope (const AblExecScope&) = delete;
	AblExecScope& operator= (const AblExecScope&) = delete;
private:
	AblExecContext*	previous;
};

//***************************************************************************

//----------
// FUNCTIONS

SymTableNodePtr getSymTableCodePtr (void);
TypePtr execRoutineCall (void);
TypePtr execExpression (void);
TypePtr execVariable (void);

//*************************
// CRUNCH/DECRUNCH routines
//*************************

void crunchToken (void);
void crunchSymTableNodePtr (SymTableNodePtr nodePtr);
char* lastCrunchedNodeSlot (void);
void patchCrunchedNode (char* slot, SymTableNodePtr nodePtr);
void crunchStatementMarker (void);
void uncrunchStatementMarker (void);
char* crunchAddressMarker (Address address);
char* fixupAddressMarker (Address address);
void crunchInteger (int value);
void crunchByte (unsigned char value);
void crunchOffset (Address address);
char* createCodeSegment (long& codeSegmentSize);
SymTableNodePtr getCodeSymTableNodePtr (void);
int getCodeStatementMarker (void);
char* getCodeAddressMarker (void);
int getCodeInteger (void);
unsigned char getCodeByte (void);
char* getCodeAddress (void);

//***************
// STACK routines
//***************

void pop (void);
void getCodeToken (void);
void initializeEntryFrame ();
bool pushEntryValueParameter (TypePtr formalTypePtr, const ABLParam& param);
void pushInteger (int value);
void pushReal (float value);
void pushByte (char value);
void pushAddress (Address address);
void pushBoolean (bool value);
void pushStackFrameHeader (long oldLevel, long newLevel);
void allocLocal (TypePtr typePtr);
void freeData (SymTableNodePtr idPtr);

//*****************************
// FUNCTION ENTRY/EXIT routines
//*****************************

void routineEntry (SymTableNodePtr routineIdPtr);
void routineExit (SymTableNodePtr routineIdPtr);
void execute (SymTableNodePtr routineIdPtr);
void executeChild (SymTableNodePtr routineIdPtr, SymTableNodePtr childRoutineIdPtr);

//******************
// EXECSTMT routines
//******************

void execStatement (void);
void execAssignmentStatement (SymTableNodePtr idPtr);
TypePtr execRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder);
TypePtr execDeclaredRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder);
void execActualParams (SymTableNodePtr routineIdPtr);
void execCompoundStatement (void);
void execCaseStatement (void);
void execForStatement (void);
void execIfStatement (void);
void execRepeatStatement (void);
void execWhileStatement (void);
void execSwitchStatement (void);
void execTransStatement (void);
void execTransBackStatement (void);

//******************
// EXECEXPR routines
//******************

TypePtr execField (void);
TypePtr execSubscripts (TypePtr typePtr);
TypePtr execConstant (SymTableNodePtr idPtr);
TypePtr execVariable (SymTableNodePtr idPtr, UseType use);
TypePtr execFactor (void);
TypePtr execTerm (void);
TypePtr execSimpleExpression (void);
TypePtr execExpression (void);

//*****************
// EXECSTD routines
//*****************

TypePtr execStandardRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder);

//***************************************************************************

#endif
