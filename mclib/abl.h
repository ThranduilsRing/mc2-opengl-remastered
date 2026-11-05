//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABL.H
//
//***************************************************************************

#ifndef ABL_H
#define	ABL_H

#include "mc2_czstring.h"
#include <span>

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLEXEC_H
#include"ablexec.h"
#endif

#ifndef ABLSYMT_H
#include"ablsymt.h"
#endif

#ifndef ABLENV_H
#include"ablenv.h"
#endif

#ifndef ABLDBUG_H
#include"abldbug.h"
#endif

//***************************************************************************

#define	DEBUGGING

void ABLi_init (unsigned long runtimeStackSize, // = 20480,
				unsigned long maxCodeBufferSize, // = 10240,
				unsigned long maxRegisteredModules, // = 200,
				unsigned long maxStaticVariables, // = 100,
				long (*fileCreateCB) (void** file, mc2::czstring fName),
				long (*fileOpenCB) (void** file, mc2::czstring fName),
				long (*fileCloseCB) (void** file),
				bool (*fileEofCB) (void* file),
				long (*fileReadCB) (void* file, unsigned char* buffer, long length),
				int32_t (*fileReadIntCB) (void* file),
				long (*fileReadLongCB) (void* file),
				long (*fileReadStringCB) (void* file, unsigned char* buffer),
				long (*fileReadLineExCB) (void* file, unsigned char* buffer, long maxLength),
				long (*fileWriteCB) (void* file, unsigned char* buffer, long length),
				long (*fileWriteByteCB) (void* file, unsigned char byte),
				long (*fileWriteIntCB) (void* file, int32_t value),
				long (*fileWriteLongCB) (void* file, long value),
				long (*fileWriteStringCB) (void* file, mc2::czstring buffer),
				void (*debuggerPrintCallback) (mc2::czstring s),
				void (*ablFatalCallback) (long code, mc2::czstring s),
				bool debugInfo = false,
				bool debug = false,
				bool profile = false);

ABLParamPtr ABLi_createParamList (long numParameters);

void ABLi_setIntegerParam (ABLParamPtr paramList, int index, int value);

void ABLi_setRealParam (ABLParamPtr paramList, int index, float value);

void ABLi_deleteParamList (ABLParamPtr paramList);

int32_t ABLi_preProcess (mc2::czstring sourceFileName,
					  long* numErrors = nullptr,
					  long* numLinesProcessed = nullptr,
					  long* numFilesProcessed = nullptr,
					  bool printLines = false);

// Execution guard for modules carrying a compile-error count. New compiles reject
// syntax errors before registration.
bool ablSkipErroredModulesEnabled (void);

ABLModulePtr ABLi_loadLibrary (mc2::czstring sourceFileName,
					   long* numErrors = nullptr,
					   long* numLinesProcessed = nullptr,
					   long* numFilesProcessed = nullptr,
					   bool printLines = false,
					   bool createInstance = true);

long ABLi_execute (SymTableNodePtr moduleIdPtr,
				   SymTableNodePtr functionIdPtr = nullptr,
				   ABLParamPtr paramList = nullptr,
				   StackItemPtr returnVal = nullptr);

long ABLi_deleteModule (SymTableNodePtr moduleIdPtr);

ABLModulePtr ABLi_getModule (long id);

// The live module instances in registry order (the order a save records); for the SIMID digest.
long ABLi_getNumModuleInstances (void);

ABLModulePtr ABLi_getModuleInstance (long index);

DebuggerPtr ABLi_getDebugger (void);

void ABLi_saveEnvironment (ABLFile* ablFile, bool values = false);

void ABLi_loadEnvironment (ABLFile* ablFile, bool malloc, bool values = false);

void ABLi_close (void);

bool ABLi_enabled (void);

void ABLi_addFunction (mc2::czstring name,
					   bool isOrder,
					   const char* paramList,
					   const char* returnType,
					   void (*codeCallback)(void));

void ABLi_setRandomCallbacks (void (*seedRandomCallback) (unsigned long seed),
							  long (*randomCallback) (long range));
void ABLi_setDebugPrintCallback (void (*ABLDebugPrintCallback) (mc2::czstring s));
void ABLi_setGetTimeCallback (unsigned long (*ABLGetTimeCallback) (void));

void ABLi_setEndlessStateCallback (void (*endlessStateCallback) (UserFile* log));

char ABLi_popChar (void);
int ABLi_popInteger (void);
float ABLi_popReal (void);
bool ABLi_popBoolean (void);
float ABLi_popIntegerReal (void);
long ABLi_popAnything (ABLStackItem* value);

char* ABLi_popCharPtr (void);
// A char[] argument that the routine writes into: the span covers the variable's declared size (a script variable is a heap
// block of exactly that many bytes). Empty when the argument is not a char array of known size; writers then store nothing.
std::span<char> ABLi_popCharBuf (void);
int* ABLi_popIntegerPtr (void);
float* ABLi_popRealPtr (void);
char* ABLi_popBooleanPtr (void);

void ABLi_pushInteger (int value);
void ABLi_pushReal (float value);
void ABLi_pushBoolean (bool value);

int ABLi_peekInteger (void);
float ABLi_peekReal (void);
bool ABLi_peekBoolean (void);

char* ABLi_peekCharPtr (void);
int* ABLi_peekIntegerPtr (void);
float* ABLi_peekRealPtr (void);

void ABLi_pokeChar (int val);
void ABLi_pokeInteger (int val);
void ABLi_pokeReal (float val);
void ABLi_pokeBoolean (bool val);

int ABLi_registerInteger (char* name, int* address, int numElements = 0);
int ABLi_registerReal (char* name, float* address, int numElements = 0);

bool ABLi_getSkipOrder (void);
void ABLi_resetOrders (void);
long ABLi_getCurrentState (void);
void ABLi_transState (long newState);

//***************************************************************************

#endif
