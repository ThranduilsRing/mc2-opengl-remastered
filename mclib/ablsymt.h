//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								SYMTABLE.H
//
//***************************************************************************

#ifndef ABLSYMT_H
#define ABLSYMT_H

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLSCAN_H
#include"ablscan.h"
#endif

#ifndef DABLENV_H
#include"dablenv.h"
#endif

//***************************************************************************

//---------------------
// DEFINITION structure

union Value {
	int         integer;
	char		character;
	float		real;
	char*		stringPtr;
};

enum class VariableType {
	VAR_TYPE_NORMAL,
	VAR_TYPE_STATIC,
	VAR_TYPE_ETERNAL,
	VAR_TYPE_REGISTERED
};
using enum VariableType;

enum class DefinitionType {
	DFN_UNDEFINED,
	DFN_CONST,
	DFN_TYPE,
	DFN_VAR,
	DFN_VALPARAM,
	DFN_REFPARAM,
	DFN_MODULE,
	DFN_PROCEDURE,
	DFN_FUNCTION
};
using enum DefinitionType;

enum class RoutineKey {
	RTN_DECLARED,
	RTN_FORWARD,
	RTN_RETURN,
	RTN_PRINT,
	RTN_CONCAT,
	RTN_GET_STATE_HANDLE,
	RTN_ASSERT,
	RTN_FATAL,
	NUM_ABL_ROUTINES
};
using enum RoutineKey;
// NOTE: NUMBER OF ROUTINEKEYS IS LIMITED TO 256!

using SymTableNodePtr = struct _SymTableNode*;
using TypePtr = struct _Type*;

void ablRememberSymbolInsertion(SymTableNodePtr* slot);
void ablRememberTypeUse(TypePtr type);

struct Constant {
	Value					value;
};

struct StateHandleInfo {
	char					name[128];
	SymTableNodePtr			state;
};

using StateHandleInfoPtr = StateHandleInfo*;

inline constexpr int ROUTINE_FLAG_ORDER = 1;
inline constexpr int ROUTINE_FLAG_FSM = 2;
inline constexpr int ROUTINE_FLAG_STATE = 4;

struct _Routine {
	RoutineKey				key;
	unsigned char			flags;
	unsigned short			orderCallIndex;
	unsigned short			numOrderCalls;
	unsigned char			paramCount;
	unsigned char			totalParamSize;
	unsigned short			totalLocalSize;
	SymTableNodePtr			params;
	SymTableNodePtr			locals;
	SymTableNodePtr			localSymTable;
	char*					codeSegment;
	long					codeSegmentSize;
}; using Routine = _Routine;

struct Data {
	VariableType			varType;
	long					offset;
	void*					registeredData;
	//SymTableNodePtr			recordIdPtr;		// Currently not implementing record structures...
};

union DefinitionInfo {
	Constant				constant;
	Routine					routine;
	Data					data;
};

struct Definition {
	DefinitionType			key;
	DefinitionInfo			info;
};

//***************************************************************************

//------------------
// SYMBOL TABLE node	

struct _SymTableNode {
	SymTableNodePtr		left;
	SymTableNodePtr		parent;
	SymTableNodePtr		right;
	SymTableNodePtr		next;
	char*				name;
	char*				info;
	Definition			defn;
	TypePtr				typePtr;
	ABLModulePtr		library;
	unsigned char		level;
	long				labelIndex;		// really for compiling only...
}; using SymTableNode = _SymTableNode;

enum class FunctionParamType {
	PARAM_TYPE_ANYTHING,
	PARAM_TYPE_CHAR,
	PARAM_TYPE_INTEGER,
	PARAM_TYPE_REAL,
	PARAM_TYPE_BOOLEAN,
	PARAM_TYPE_INTEGER_REAL,
	PARAM_TYPE_CHAR_ARRAY,
	PARAM_TYPE_INTEGER_ARRAY,
	PARAM_TYPE_REAL_ARRAY,
	PARAM_TYPE_BOOLEAN_ARRAY,
	PARAM_TYPE_REAL_REF,	// out-param: must be a plain real variable (the exec side reads a symbol reference from the code stream)
	NUM_PARAM_TYPES
};
using enum FunctionParamType;

enum class FunctionReturnType {
	RETURN_TYPE_NONE,
	RETURN_TYPE_INTEGER,
	RETURN_TYPE_REAL,
	RETURN_TYPE_BOOLEAN,
	NUM_RETURN_TYPES
};
using enum FunctionReturnType;

// Raised 256 -> 512 for Omnitech ABL stubs. Baseline MC2 needs ~256; our
// 40+11 Omnitech tier-1/2 extensions push registrations to ~313. Overflowing
// the old 256 cap OOB-wrote adjacent BSS (symptom: FunctionCallbackTable[47]
// upper 32 bits zeroed between registration and dispatch, crash with
// `...690B` low-word pattern).
inline constexpr int MAX_STANDARD_FUNCTIONS = 512;
inline constexpr int MAX_FUNCTION_PARAMS = 20;

struct StandardFunctionInfo {
	SymTableNodePtr			symbol;
	long					numParams;
	FunctionParamType		params[MAX_FUNCTION_PARAMS];
	FunctionReturnType		returnType;
};

//***************************************************************************

//---------------
// TYPE structure

enum class FormType {
	FRM_NONE,
	FRM_SCALAR,
	FRM_ENUM,
	FRM_ARRAY
	//FRM_RECORD
};
using enum FormType;

//---------------------------------------------------------------------
// Currently, we are only supporting the basic types: arrays and simple
// variables.

struct _Type {
	long							numInstances;
	FormType						form;
	long							size;
	SymTableNodePtr					typeIdPtr;
	union {
		struct {
			SymTableNodePtr			constIdPtr;
			long					max;
		} enumeration;
		struct {
			TypePtr					indexTypePtr;		// should be Integer
			TypePtr					elementTypePtr;		// should be Real, Integer, Char or array
			long					elementCount;
		} array;
		// Not currently implementing record structures...
		//struct {
		//	SymTableNodePtr		fieldSymTable;
		//} record;
	} info;
}; using Type = _Type;


//***************************************************************************

void searchLocalSymTable (SymTableNodePtr& IdPtr);
SymTableNodePtr searchLocalSymTableForFunction (const char* name);
void searchThisSymTable (SymTableNodePtr& IdPtr, SymTableNodePtr thisTable);
void searchAllSymTables (SymTableNodePtr& IdPtr);
void enterLocalSymTable (SymTableNodePtr& IdPtr);
void enterNameLocalSymTable (SymTableNodePtr& IdPtr, const char* name);
void searchAndFindAllSymTables (SymTableNodePtr& IdPtr);
void searchAndEnterLocalSymTable (SymTableNodePtr& IdPtr);
/*inline*/ void searchAndEnterThisTable (SymTableNodePtr& IdPtr, SymTableNodePtr thisTable);
inline SymTableNodePtr symTableSuccessor (SymTableNodePtr nodeX);

SymTableNodePtr searchSymTable (const char* name, SymTableNodePtr nodePtr);
SymTableNodePtr searchSymTableForFunction (const char* name, SymTableNodePtr nodePtr);
SymTableNodePtr searchSymTableForState (const char* name, SymTableNodePtr nodePtr);
SymTableNodePtr searchSymTableForString (const char* name, SymTableNodePtr nodePtr);
SymTableNodePtr searchSymTableDisplay (const char* name);
SymTableNodePtr enterSymTable (const char* name, SymTableNodePtr* ptrToNodePtr);
SymTableNodePtr insertSymTable (SymTableNodePtr* tableRoot, SymTableNodePtr newNode);
SymTableNodePtr extractSymTable (SymTableNodePtr* tableRoot, SymTableNodePtr nodeKill);
void enterStandardRoutine (const char* name, long routineKey, bool isOrder, const char* paramList, const char* returnType, void (*callback)(void));
void enterScope (SymTableNodePtr symTableRoot);
SymTableNodePtr exitScope (void);
void initSymTable (void);

TypePtr createType (void);
TypePtr setType (TypePtr type);
void clearType (TypePtr& type);

//***************************************************************************

extern StandardFunctionInfo		FunctionInfoTable[MAX_STANDARD_FUNCTIONS];
//extern void*					FunctionCallbackTable[MAX_STANDARD_FUNCTIONS];
extern void						(*FunctionCallbackTable[MAX_STANDARD_FUNCTIONS])(void);
extern long						NumStandardFunctions;

#endif

