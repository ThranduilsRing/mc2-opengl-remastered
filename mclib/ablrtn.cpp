//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLRTN.CPP
//
//***************************************************************************

#include "mc2_czstring.h"
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <ankerl/unordered_dense.h>
#include <utility>
#include"platform_str.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include "abl_script_events.h"

#ifndef ABLGEN_H
#include"ablgen.h"
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


#ifndef ABLENV_H
#include"ablenv.h"
#endif

#ifndef ABL_H
#include"abl.h"
#endif

#ifndef ABLDBUG_H
#include"abldbug.h"
#endif

//***************************************************************************

extern long				MaxBreaks;
extern long				MaxWatches;
extern DebuggerPtr		debugger;
extern int32_t          NumModules;
extern ModuleEntryPtr	ModuleRegistry;
extern ABLModulePtr*	ModuleInstanceRegistry;
extern ABLModulePtr*	LibraryInstanceRegistry;
extern int32_t          MaxLibraries;
extern int32_t          MaxModules;
extern int32_t          NumModulesRegistered;
extern int32_t          NumModuleInstances;
extern int32_t          MaxWatchesPerModule;
extern int32_t          MaxBreakPointsPerModule;
extern long				MaxCodeBufferSize;
extern ABLModulePtr		CurLibrary;
extern char*			codeBuffer;
extern char*			codeBufferPtr;
extern StackItem*		stack;
extern long*			StaticVariablesSizes;
extern long*			EternalVariablesSizes;
extern long				MaxEternalVariables;
extern long				eternalOffset;
extern long				MaxStaticVariables;
extern long				NumStaticVariables;
extern long				NumOrderCalls;
extern StateHandleInfo	StateHandleList[MAX_STATE_HANDLES_PER_MODULE];
extern long				NumStateHandles;
extern long				MaxLoopIterations;
extern bool				AssertEnabled;
extern bool				IncludeDebugInfo;
extern bool				ProfileABL;
extern bool				Crunch;
extern bool				EnterStateSymbol;
extern int32_t          level;
extern int32_t		    lineNumber;
extern int32_t		    FileNumber;
extern ABLFile*		sourceFile;
extern bool				printFlag;
extern bool				blockFlag;
extern BlockType		blockType;
extern SymTableNodePtr	CurModuleIdPtr;
extern SymTableNodePtr	CurRoutineIdPtr;
extern bool				DumbGetCharOn;
extern long				NumOpenFiles;
extern long				NumSourceFiles;
extern long				NumLibrariesUsed;
extern ABLModulePtr		LibrariesUsed[MAX_LIBRARIES_USED];
extern long				bufferOffset;
extern char*			bufferp;
extern char*			tokenp;
extern long				digitCount;
extern bool				countError;
extern int				pageNumber;
extern int				lineCount;

extern long				CurAlarm;

extern bool				eofFlag;

extern long				dummyCount;

extern long				errorCount;
extern long				NumSourceFiles;
extern char				SourceFiles[MAX_SOURCE_FILES][MAXLEN_FILENAME];
 
extern TokenCodeType	curToken;
extern char				wordString[];
extern SymTableNodePtr	symTableDisplay[];
extern bool				blockFlag;
extern BlockType		blockType;
extern bool				printFlag;
extern SymTableNodePtr	CurRoutineIdPtr;

extern Type				DummyType;
extern StackItem*		stack;
//extern StackItem*		eternalStack;
extern long				eternalOffset;

extern TokenCodeType	statementStartList[];
extern TokenCodeType	statementEndList[];
extern TokenCodeType	declarationStartList[];

extern char				tokenString[MAXLEN_TOKENSTRING];

extern CharCodeType		charTable[256];

extern char				sourceBuffer[MAXLEN_SOURCELINE];
extern long				bufferOffset;
extern char*			bufferp;
extern char*			tokenp;

extern long				digitCount;
extern bool				countError;

extern bool				eofFlag;

extern SymTableNodePtr	SymTableDisplay[MAX_NESTING_LEVEL];
extern TypePtr			IntegerTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;


extern ModuleEntryPtr	ModuleRegistry;
extern long		        MaxStaticVariables;
extern long		        NumStaticVariables;
extern long		        NumSourceFiles;

extern bool				IncludeDebugInfo;
extern bool				AssertEnabled;
extern bool				PrintEnabled;
extern bool				StringFunctionsEnabled;
extern bool				DebugCodeEnabled;
extern DebuggerPtr		debugger;

bool					ABLenabled = false;
char					buffer[MAXLEN_PRINTLINE];

extern bool				SkipOrder;


extern void transState (SymTableNodePtr newState);

int32_t                 numLibrariesLoaded = 0;
int32_t                 NumExecutions = 0;

// The runtime stack: `stack` is the view the interpreter indexes, this vector owns it (ABLi_init to ABLi_close).
static std::vector<StackItem> stackStore;
void ABL_openArena (void);
void ABL_closeArena (void);
void (*ABLDebugPrintCallback) (mc2::czstring s) = nullptr;
long (*ABLRandomCallback) (long range) = nullptr;
void (*ABLSeedRandomCallback) (unsigned long range) = nullptr;
unsigned long (*ABLGetTimeCallback) (void) = nullptr;
void (*ABLFatalCallback) (long code, mc2::czstring s) = nullptr;
void (*ABLEndlessStateCallback) (UserFile* log) = nullptr;

//***************************************************************************

TokenCodeType followHeaderList[] = {
	TKN_SEMICOLON,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType followModuleIdList[] = {
	TKN_LPAREN,
	TKN_COLON,
	TKN_SEMICOLON,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType followFunctionIdList[] = {
	TKN_LPAREN,
	TKN_COLON,
	TKN_SEMICOLON,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType followParamsList[] = {
	TKN_RPAREN,
	TKN_COMMA,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType followParamList[] = {
	TKN_COMMA,
	TKN_RPAREN,
	TKN_NONE
};

TokenCodeType followModuleDeclsList[] = {
	TKN_SEMICOLON,
	TKN_CODE,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType followRoutineDeclsList[] = {
	TKN_SEMICOLON,
	TKN_CODE,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType followDeclsList[] = {
	TKN_SEMICOLON,
	TKN_EOF,
	TKN_NONE
};

void ABL_CloseProfileLog (void);
void ABL_OpenProfileLog (void);
void ABL_AddToProfileLog (char* profileString);

//***************************************************************************
// ABL library interface routines
//***************************************************************************

long DefaultRandom (long range) {

	return(0);
}

//---------------------------------------------------------------------------

void DefaultSeedRandom (unsigned long seed) {
}

//---------------------------------------------------------------------------

void DefaultDebugPrintCallback (mc2::czstring s) {
}

//---------------------------------------------------------------------------

unsigned long DefaultGetTimeCallback (void) {

	return(0);
}

//---------------------------------------------------------------------------

void ABLi_setRandomCallbacks (void (*seedRandomCallback) (unsigned long seed),
							  long (*randomCallback) (long range)) {

	ABLSeedRandomCallback = seedRandomCallback;
	ABLRandomCallback = randomCallback;
}

//---------------------------------------------------------------------------

void ABLi_setDebugPrintCallback (void (*debugPrintCallback) (mc2::czstring s)) {

	ABLDebugPrintCallback = debugPrintCallback;
}

//---------------------------------------------------------------------------

void ABLi_setGetTimeCallback (unsigned long (*getTimeCallback) (void)) {

	ABLGetTimeCallback = getTimeCallback;
}

//---------------------------------------------------------------------------

void ABLi_setEndlessStateCallback (void (*endlessStateCallback) (UserFile* log)) {

	ABLEndlessStateCallback = endlessStateCallback;
}

//---------------------------------------------------------------------------

void ABLi_init (unsigned long runtimeStackSize,
				unsigned long maxCodeBufferSize,
				unsigned long maxRegisteredModules,
				unsigned long maxStaticVariables,
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
				long (*fileWriteIntCB) (void* file, int32_t byte),
				long (*fileWriteLongCB) (void* file, long value),
				long (*fileWriteStringCB) (void* file, mc2::czstring buffer),
				void (*debuggerPrintCallback) (mc2::czstring s),
				void (*fatalCallback) (long code, mc2::czstring s),
				bool debugInfo,
				bool debug,
				bool profile) {

// HACK, for testing...
#ifdef _DEBUG
profile = true;
#endif
//
	//-------------------------------------------------------------------
	// Let's make sure we have not created too many built-in ABL routines
	// for the lovely user...
	if (std::to_underlying(RoutineKey::NUM_ABL_ROUTINES) > 254)
		ABL_Fatal(0, " MAJOR ABL ERROR: Too Many ABL Routines ");

	ABL_openArena();
	ABLDebugPrintCallback = DefaultDebugPrintCallback;
	ABLRandomCallback = DefaultRandom;
	ABLSeedRandomCallback = DefaultSeedRandom;
	ABLGetTimeCallback = DefaultGetTimeCallback;
	ABLFatalCallback = fatalCallback;

	ABLFile::createCB = fileCreateCB;
	ABLFile::openCB = fileOpenCB;
	ABLFile::closeCB = fileCloseCB;
	ABLFile::eofCB = fileEofCB;
	ABLFile::readCB = fileReadCB;
	ABLFile::readIntCB = fileReadIntCB;
	ABLFile::readLongCB = fileReadLongCB;
	ABLFile::readStringCB = fileReadStringCB;
	ABLFile::readLineExCB = fileReadLineExCB;
	ABLFile::writeCB = fileWriteCB;
	ABLFile::writeByteCB = fileWriteByteCB;
	ABLFile::writeIntCB = fileWriteIntCB;
	ABLFile::writeLongCB = fileWriteLongCB;
	ABLFile::writeStringCB = fileWriteStringCB;

	NumStandardFunctions = std::to_underlying(RoutineKey::NUM_ABL_ROUTINES);

	ABLenabled = true;
	MaxBreaks = 50;
	MaxWatches = 50;
	debugger = nullptr;
	NumModules = 0;
	ModuleRegistry = nullptr;
	ModuleInstanceRegistry = nullptr;
	LibraryInstanceRegistry = nullptr;
	MaxModules = 0;
	MaxLibraries = 0;
	NumModulesRegistered = 0;
	NumModuleInstances = 0;
	MaxWatchesPerModule = 20;
	MaxBreakPointsPerModule = 20;
	g_ablExec->CurModule = nullptr;
	errorCount = 0;
	codeBuffer = nullptr;
	codeBufferPtr = nullptr;
	g_ablExec->codeSegmentPtr = nullptr;
	g_ablExec->codeSegmentLimit = nullptr;
	g_ablExec->statementStartPtr = nullptr;
	g_ablExec->execStatementCount = 0;
	stack = nullptr;
	stackCount = 0;
	g_ablExec->stackLimit = nullptr;
	g_ablExec->tos = nullptr;
	g_ablExec->stackFrameBasePtr = nullptr;
	g_ablExec->StaticDataPtr = nullptr;
	StaticVariablesSizes = nullptr;
	EternalVariablesSizes = nullptr;
	MaxEternalVariables = 5000;
	eternalOffset = 0;
	MaxStaticVariables = 0;
	NumStaticVariables = 0;
	NumOrderCalls = 0;
	NumStateHandles = 1;
	g_ablExec->CurModuleHandle = 0;
	g_ablExec->CallModuleInit = false;
	g_ablExec->AutoReturnFromOrders = false;
	MaxLoopIterations = 100001;
	AssertEnabled = false;
	PrintEnabled = false;
	StringFunctionsEnabled = true;
	IncludeDebugInfo = true;
	ProfileABL = profile;
	Crunch = true;
	level = 0;
	lineNumber = 0;
	FileNumber = 0;
	sourceFile = nullptr;
	printFlag = true;
	blockFlag = false;
	blockType = BLOCK_MODULE;
	CurModuleIdPtr = nullptr;
	CurRoutineIdPtr = nullptr;
	DumbGetCharOn = false;
	NumOpenFiles = 0;
	NumSourceFiles = 0;
	bufferOffset = 0;
	bufferp = sourceBuffer;
	tokenp = tokenString;
	digitCount = 0;
	countError = false;
	pageNumber = 0;
	lineCount = MAX_LINES_PER_PAGE;

	eofFlag = false;
	g_ablExec->ExitWithReturn = false;
	g_ablExec->ExitFromTacOrder = false;
	dummyCount = 0;
	numLibrariesLoaded = 0;
	
	//----------------------------------
	// Initialize the character table...
	for (auto & charTableElem : charTable)
		charTableElem = CHR_SPECIAL;
	for (long curCh = '0'; curCh <= '9'; curCh++)
		charTable[curCh] = CHR_DIGIT;
	for (long curCh = 'A'; curCh <= 'Z'; curCh++)
		charTable[curCh] = CHR_LETTER;
	for (long curCh = 'a'; curCh <= 'z'; curCh++)
		charTable[curCh] = CHR_LETTER;
	charTable['\"'] = CHR_DQUOTE;
	charTable[CHAR_EOF] = CHR_EOF;

	//--------------------------------------------------------------------
	// Allocate the code buffer used during pre-processing/interpreting...
	MaxCodeBufferSize = maxCodeBufferSize;
	codeBuffer = static_cast<char*>(ABL_alloc(ABLPool::Code, maxCodeBufferSize));
	if (!codeBuffer)
		ABL_Fatal(0, " ABL: Unable to AblCodeHeap->malloc preprocess code buffer ");
	
	//----------------------------------------------------------------------
	// Alloc the static variable data block. Ultimately, we may want to just
	// implement the static data construction with a linked list, rather
	// than a set max size...
	MaxStaticVariables = maxStaticVariables;
	NumStaticVariables = 0;
	NumOrderCalls = 0;
	NumStateHandles = 1;
	g_ablExec->StaticDataPtr = nullptr;
	StaticVariablesSizes = nullptr;
	if (MaxStaticVariables > 0) {
		StaticVariablesSizes = static_cast<long*>(ABL_alloc(ABLPool::Stack, sizeof(long) * MaxStaticVariables));
		if (!StaticVariablesSizes)
			ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc StaticVariablesSizes ");
	}

	//------------------------------
	// Allocate the runtime stack...
	stackStore.assign(runtimeStackSize / sizeof(StackItem), StackItem{});
	stack = stackStore.data();
	stackCount = stackStore.size();
	g_ablExec->stackLimit = ablRootStackLimit();

	//-----------------------------------
	// Allocate Eternal Vars Size List...
	if (MaxEternalVariables > 0) {
		EternalVariablesSizes = static_cast<long*>(ABL_alloc(ABLPool::Stack, sizeof(long) * MaxEternalVariables));
		if (!EternalVariablesSizes)
			ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc EternalVariablesSizes ");
	}

	//-------------------------
	// Prep the symbol table...
	initSymTable();

	initModuleRegistry(maxRegisteredModules);

	initLibraryRegistry(10);

	//--------------------------------
	// Init the debugger, if needed...
	IncludeDebugInfo = debugInfo;
	if (debug) {
		IncludeDebugInfo = true;
		debugger = new Debugger;
		if (!debugger)
			ABL_Fatal(0, " Unable to initialize ABL Debugger. ");
		debugger->init(debuggerPrintCallback, nullptr);
	}

	UserFile::setup();

	if (ProfileABL)
		ABL_OpenProfileLog();

	// ASAN-3-ABL-COMPILE-FATAL-1 negative fixture hook. Tests supply an
	// absolute path to a deliberately malformed standalone ABL module; normal
	// runs do no extra preprocessing. The expected default-ON guard exits from
	// ABLi_preProcess with an attributable [ABL_COMPILE_FATAL] line.
	if (mc2::gate::isSet<"MC2_ABL_COMPILE_FATAL_REPRO">()) {
		const std::string reproStr = mc2::gate::str<"MC2_ABL_COMPILE_FATAL_REPRO">();
		const char* reproPath = reproStr.c_str();
		long reproErrors = 0;
		long reproLines = 0;
		mc2::fmt::print_err("[ABL_COMPILE_FATAL_REPRO] file={}\n", mc2::fmt::safe(reproPath));
		fflush(stderr);
		ABLi_preProcess(reproPath, &reproErrors, &reproLines);
	}

#ifdef TEST_ABLFILE
	{
		auto aFile = std::make_unique<ABLFile>();
		aFile->create("ablFile.txt");
		aFile->writeString("Testing writeString");
		aFile->writeByte(nullptr);
		aFile->writeInt(100);
		aFile->close();
	}

	unsigned char s[100];
	{
		auto bFile = std::make_unique<ABLFile>();
		bFile->open("ablFile.txt");
		bFile->readString(s);
		int32_t val = bFile->readInt();
		bFile->close();
	}
#endif
}

//***************************************************************************

namespace {
// Parser allocations and shared symbol mutations belong to one compile until it succeeds.
struct CompileTransaction;
CompileTransaction* activeCompile = nullptr;
struct CompileTransaction {
	struct Allocation { void* address; ABLPool heap; };
	std::vector<Allocation> allocations;
	ankerl::unordered_dense::map<void*, size_t> allocationIndex;
	std::vector<SymTableNodePtr*> insertions;
	std::vector<std::pair<TypePtr, long>> typeUses;
	std::vector<char> savedCode;
	SymTableNodePtr display[MAX_NESTING_LEVEL];
	long savedEternalOffset = eternalOffset;
	bool committed = false;
	// Called by ABL_alloc / ABL_free while this compile is active.
	void remember(void* address, ABLPool heap) {
		allocations.push_back({address, heap});
		try {
			allocationIndex.emplace(address, allocations.size() - 1);
		} catch (...) {
			// ABL_alloc frees the block if tracking fails; do not retain it.
			allocations.pop_back();
			throw;
		}
	}
	void forget(void* address, ABLPool heap) {
		auto found = allocationIndex.find(address);
		if (found == allocationIndex.end())
			return;
		auto& entry = allocations[found->second];
		if (entry.heap == heap) {
			entry.address = nullptr;
			allocationIndex.erase(found);
		}
	}
	CompileTransaction() : savedCode(static_cast<size_t>(MaxCodeBufferSize)) {
		memcpy(savedCode.data(), codeBuffer, savedCode.size());
		std::copy_n(SymTableDisplay, MAX_NESTING_LEVEL, display);
		activeCompile = this;
	}
	~CompileTransaction() {
		activeCompile = nullptr;
		if (committed)
			return;
		for (auto slot = insertions.rbegin(); slot != insertions.rend(); ++slot)
			**slot = nullptr;
		for (auto [type, count] : typeUses)
			type->numInstances = count;
		std::copy_n(display, MAX_NESTING_LEVEL, SymTableDisplay);
		for (long i = savedEternalOffset; i < eternalOffset; ++i) {
			stack[i] = {};
			EternalVariablesSizes[i] = 0;
		}
		eternalOffset = savedEternalOffset;
		memcpy(codeBuffer, savedCode.data(), savedCode.size());
		for (auto entry : allocations)
			if (entry.address)
				ABL_free(entry.heap, entry.address);
		level = 0;
		CurModuleIdPtr = CurRoutineIdPtr = nullptr;
		codeBufferPtr = codeBuffer;
		blockFlag = false;
		DumbGetCharOn = false;
		EnterStateSymbol = false;
		Crunch = true;
		NumLibrariesUsed = NumStaticVariables = NumOrderCalls = 0;
		NumStateHandles = 1;
		std::fill_n(StateHandleList, MAX_STATE_HANDLES_PER_MODULE, StateHandleInfo{});
	}
};
}

// The environment's pools (see ablscan.h). Each block's size is kept beside it: the pool resource needs it to hand the block back.
namespace {
struct AblArena {
	struct Pool {
		std::pmr::unsynchronized_pool_resource memory;
		ankerl::unordered_dense::map<void*, size_t> live; // find/erase/size/contains only, never iterated
	};
	Pool pools[3];
};
std::unique_ptr<AblArena> ablArena;
constexpr size_t kAblAlign = alignof(std::max_align_t);
}

void ABL_openArena (void) {
	ablArena = std::make_unique<AblArena>();
}

void ABL_closeArena (void) {
	ablArena.reset();
}

void* ABL_alloc (ABLPool which, size_t bytes) {
	if (!ablArena)
		ABL_Fatal(0, " ABL: allocation outside the environment ");
	auto& pool = ablArena->pools[static_cast<int>(which)];
	const size_t size = bytes ? bytes : 1;
	void* block = pool.memory.allocate(size, kAblAlign);
	try {
		pool.live.emplace(block, size);
		if (activeCompile)
			activeCompile->remember(block, which);
	} catch (...) {
		pool.live.erase(block);
		pool.memory.deallocate(block, size, kAblAlign);
		throw;
	}
	std::memset(block, 0, size);
	return block;
}

void ABL_free (ABLPool which, void* block) {
	if (!block)
		return;
	if (!ablArena)
		ABL_Fatal(0, " ABL: free after the environment closed ");
	// A block handed back to the wrong kind (all kinds shared one allocator before) is still found and freed by its owner.
	for (int i = 0; i < 3; ++i) {
		const int index = (static_cast<int>(which) + i) % 3;
		auto& pool = ablArena->pools[index];
		auto found = pool.live.find(block);
		if (found == pool.live.end())
			continue;
		if (activeCompile)
			activeCompile->forget(block, static_cast<ABLPool>(index));
		[[maybe_unused]] const auto [liveBlock, size] = *found;
		pool.live.erase(found);
		pool.memory.deallocate(block, size, kAblAlign);
		return;
	}
	ABL_Fatal(0, " ABL: free of a block the environment does not own ");
}

size_t ABL_liveBlocks (void) {
	if (!ablArena)
		return 0;
	size_t count = 0;
	for (const auto& pool : ablArena->pools)
		count += pool.live.size();
	return count;
}

bool ABL_ownsBlock (const void* block) {
	if (!ablArena)
		return false;
	for (const auto& pool : ablArena->pools)
		if (pool.live.contains(const_cast<void*>(block)))
			return true;
	return false;
}

void ablRememberSymbolInsertion(SymTableNodePtr* slot) {
	if (activeCompile)
		activeCompile->insertions.push_back(slot);
}
void ablRememberTypeUse(TypePtr type) {
	if (activeCompile && std::none_of(activeCompile->typeUses.begin(), activeCompile->typeUses.end(),
		[type](const auto& entry) { return std::get<0>(entry) == type; }))
		activeCompile->typeUses.emplace_back(type, type->numInstances);
}

int32_t ABLi_preProcess (mc2::czstring sourceFileName, long* numErrors, long* numLinesProcessed, long* numFilesProcessed, bool printLines) {

	if (numErrors) *numErrors = 0;
	if (numLinesProcessed) *numLinesProcessed = 0;
	if (numFilesProcessed) *numFilesProcessed = 0;
    char* source_fn = strdup(sourceFileName);
    source_fn = S_strlwr(source_fn);
	for (int i = 0; i < NumModulesRegistered; i++)
    {
		if (strcmp(source_fn, ModuleRegistry[i].fileName) == 0)
        {
            free(source_fn);
			return(i);
        }
    }

	ABLSourceFiles sourceFiles;
	CompileTransaction transaction;
	try {

	level = 0;
	lineNumber = 0;
	FileNumber = 0;
	AssertEnabled = (debugger != nullptr);
	PrintEnabled = (debugger != nullptr);
	StringFunctionsEnabled = true;
	DebugCodeEnabled = (debugger != nullptr);
	NumSourceFiles = 0;
	NumLibrariesUsed = 0;
	sourceFile = nullptr;
	printFlag = printLines;
	blockFlag = false;
	blockType = BLOCK_MODULE;
	bufferOffset = 0;
	bufferp = sourceBuffer;
	tokenp = tokenString;
	digitCount = 0;
	countError = false;
	pageNumber = 0;
	errorCount = 0;
	DumbGetCharOn = false;
	g_ablExec->execStatementCount = 0;
	eofFlag = false;
	NumStaticVariables = 0;
	NumOrderCalls = 0;
	NumStateHandles = 1;
	for (auto & StateHandleElem : StateHandleList) {
		StateHandleElem.name[0] = '\0';
		StateHandleElem.state = nullptr;
	}

	long openErr = ABL_NO_ERR;
	if ((openErr = openSourceFile(source_fn)) != ABL_NO_ERR)
	{
		free(source_fn);
		return(openErr);
	}

	codeBufferPtr = codeBuffer;

	getToken();

	SymTableNodePtr moduleIdPtr = moduleHeader();

	if (moduleIdPtr == nullptr)
		syntaxError(ABL_ERR_SYNTAX_MISSING_MODULE);

	CurModuleIdPtr = moduleIdPtr;
	CurRoutineIdPtr = moduleIdPtr;

	synchronize(followHeaderList, declarationStartList, statementStartList);
	if (curToken == TKN_SEMICOLON)
		getToken();
	else if (tokenIn(declarationStartList) || tokenIn(statementStartList))
		syntaxError(ABL_ERR_SYNTAX_MISSING_SEMICOLON);

	declarations(moduleIdPtr, true);
	if ((moduleIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM) == 0) {
		synchronize(followModuleDeclsList, nullptr, nullptr);
		if (curToken != TKN_CODE)
			syntaxError(ABL_ERR_SYNTAX_MISSING_CODE);
		crunchToken();
		blockType = BLOCK_MODULE;
		blockFlag = true;

		getToken();

		TokenCodeType endToken = TKN_END_MODULE;
		if (CurLibrary)
			endToken = TKN_END_LIBRARY;
		if (curToken != endToken)
			do {
				statement();
				while (curToken == TKN_SEMICOLON)
					getToken();
				if (curToken == endToken)
					break;
		
				synchronize(statementStartList, nullptr, nullptr);
			} while (tokenIn(statementStartList));
	}

	if (CurLibrary)
		ifTokenGetElseError(TKN_END_LIBRARY, ABL_ERR_SYNTAX_MISSING_END_LIBRARY);
	else if (moduleIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM)
		ifTokenGetElseError(TKN_END_FSM, ABL_ERR_SYNTAX_MISSING_END_FSM);
	else
		ifTokenGetElseError(TKN_END_MODULE, ABL_ERR_SYNTAX_MISSING_END_MODULE);

	blockFlag = false;

	moduleIdPtr->defn.info.routine.localSymTable = exitScope();
	moduleIdPtr->defn.info.routine.codeSegment = createCodeSegment(moduleIdPtr->defn.info.routine.codeSegmentSize);

	analyzeBlock(moduleIdPtr->defn.info.routine.codeSegment);

	ifTokenGetElseError(TKN_PERIOD, ABL_ERR_SYNTAX_MISSING_PERIOD);

	if (curToken != TKN_EOF)
		syntaxError(ABL_ERR_SYNTAX_UNEXPECTED_TOKEN);

	closeSourceFile();

	ModuleRegistry[NumModulesRegistered].fileName = static_cast<char*>(ABL_alloc(ABLPool::Stack, static_cast<unsigned long>(strlen(source_fn) + 1)));
	if (!ModuleRegistry[NumModulesRegistered].fileName)
		ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc module filename ");
	strcpy(ModuleRegistry[NumModulesRegistered].fileName, source_fn);
	ModuleRegistry[NumModulesRegistered].moduleIdPtr = moduleIdPtr;

	ModuleRegistry[NumModulesRegistered].numSourceFiles = NumSourceFiles;
	ModuleRegistry[NumModulesRegistered].sourceFiles = static_cast<char**>(ABL_alloc(ABLPool::Stack, NumSourceFiles * sizeof(char*)));
	if (!ModuleRegistry[NumModulesRegistered].sourceFiles)
		ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc sourceFiles ");
	for (int i = 0; i < NumSourceFiles; i++) {
		ModuleRegistry[NumModulesRegistered].sourceFiles[i] = static_cast<char*>(ABL_alloc(ABLPool::Stack, static_cast<unsigned long>(strlen(SourceFiles[i]) + 1)));
		strcpy(ModuleRegistry[NumModulesRegistered].sourceFiles[i], SourceFiles[i]);
	}

	if (NumLibrariesUsed > 0) {
		ModuleRegistry[NumModulesRegistered].numLibrariesUsed = NumLibrariesUsed;
		ModuleRegistry[NumModulesRegistered].librariesUsed = static_cast<ABLModulePtr*>(ABL_alloc(ABLPool::Stack, NumLibrariesUsed * sizeof(SymTableNodePtr)));
		if (!ModuleRegistry[NumModulesRegistered].librariesUsed)
			ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc librariesUsed ");
		for (int i = 0; i < NumLibrariesUsed; i++)
			ModuleRegistry[NumModulesRegistered].librariesUsed[i] = LibrariesUsed[i];
	}

	ModuleRegistry[NumModulesRegistered].numStaticVars = NumStaticVariables;
	ModuleRegistry[NumModulesRegistered].sizeStaticVars	= nullptr;
	ModuleRegistry[NumModulesRegistered].totalSizeStaticVars = 0;
	if (NumStaticVariables) {
		ModuleRegistry[NumModulesRegistered].sizeStaticVars = static_cast<long*>(ABL_alloc(ABLPool::Stack, sizeof(long) * NumStaticVariables));
		if (!ModuleRegistry[NumModulesRegistered].sizeStaticVars)
			ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc module sizeStaticVars ");
		memcpy(ModuleRegistry[NumModulesRegistered].sizeStaticVars, StaticVariablesSizes, sizeof(long) * NumStaticVariables);
		ModuleRegistry[NumModulesRegistered].totalSizeStaticVars = sizeof(long) * NumStaticVariables;
		for (long i = 0; i < ModuleRegistry[NumModulesRegistered].numStaticVars; i++)
			ModuleRegistry[NumModulesRegistered].totalSizeStaticVars += ModuleRegistry[NumModulesRegistered].sizeStaticVars[i];
	}
	ModuleRegistry[NumModulesRegistered].numOrderCalls = NumOrderCalls;
	ModuleRegistry[NumModulesRegistered].numInstances = 0;



	ModuleRegistry[NumModulesRegistered].numStateHandles = NumStateHandles;
	if (NumStateHandles > 1) {
		ModuleRegistry[NumModulesRegistered].stateHandles = static_cast<StateHandleInfoPtr>(ABL_alloc(ABLPool::Stack, sizeof(StateHandleInfo) * NumStateHandles));
		memcpy(ModuleRegistry[NumModulesRegistered].stateHandles, StateHandleList, sizeof(StateHandleInfo) * NumStateHandles);
	}

	ModuleRegistry[NumModulesRegistered].compileErrorCount = errorCount;

	NumModulesRegistered++;

	if (numLinesProcessed)
		*numLinesProcessed = lineNumber;

	if (numFilesProcessed)
		*numFilesProcessed = FileNumber;

	if (numErrors)
		*numErrors = errorCount;

	free(source_fn);
	transaction.committed = true;
	return(NumModulesRegistered - 1);
	} catch (const ABLSyntaxFailure&) {
		if (numErrors) *numErrors = errorCount;
		if (numLinesProcessed) *numLinesProcessed = lineNumber;
		if (numFilesProcessed) *numFilesProcessed = NumSourceFiles;
		free(source_fn);
		return -1;
	}
}

//***************************************************************************

void initializeEntryFrame () {
	errorCount = 0;
	g_ablExec->execStatementCount = 0;
	NumExecutions++;
	g_ablExec->resetStack();
	g_ablExec->level = 1;
	g_ablExec->CallStackLevel = 0;
	g_ablExec->stackFrameBasePtr = g_ablExec->tos + 1;
	pushInteger(0);
	pushAddress(nullptr);
	pushAddress(nullptr);
	pushAddress(nullptr);
}

bool pushEntryValueParameter (TypePtr formalTypePtr, const ABLParam& param) {
	if (formalTypePtr == RealTypePtr) {
		if (param.type == ABL_PARAM_INTEGER)
			pushReal(static_cast<float>(param.integer));
		else if (param.type == ABL_PARAM_REAL)
			pushReal(param.real);
	} else if (formalTypePtr == IntegerTypePtr) {
		if (param.type == ABL_PARAM_INTEGER)
			pushInteger(param.integer);
		else
			return false;
	}
	return true;
}

long ABLi_execute (SymTableNodePtr moduleIdPtr, SymTableNodePtr functionIdPtr, ABLParamPtr paramList, StackItemPtr returnVal) {

	CurModuleIdPtr = nullptr;
	CurRoutineIdPtr = nullptr;
	g_ablExec->CurRoutineIdPtr = nullptr;
	initializeEntryFrame();

	if (paramList) {
		long curParam = 0;
		for (SymTableNodePtr formalIdPtr = moduleIdPtr->defn.info.routine.params;
			 formalIdPtr != nullptr;
			 formalIdPtr = formalIdPtr->next) {

			TypePtr formalTypePtr = formalIdPtr->typePtr;

			if (formalIdPtr->defn.key == DFN_VALPARAM) {

				if (!pushEntryValueParameter(formalTypePtr, paramList[curParam]))
					return 0;

				if (formalTypePtr->form == FRM_ARRAY/* || (formalTypePtr->form == FRM_RECORD)*/) {
					long size = formalTypePtr->size;
					char* dest = static_cast<char*>(ABL_alloc(ABLPool::Stack, static_cast<size_t>(size)));
					if (!dest)
						ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc module formal array param ");
					// The entry parameter list holds only integers and reals, so there is no actual array to copy from (the stack top is another
					// frame cell): the formal starts zeroed and the script is told, instead of reading through that cell.
					memset(dest, 0, static_cast<size_t>(size));
					abl::reportBadArgument("ABLi_execute", "array_parameter");
					pushAddress(dest);
				}
				}
			else {
				if (formalTypePtr == RealTypePtr)
					pushAddress(reinterpret_cast<Address>(&(paramList[curParam].real)));
				else if (formalTypePtr == IntegerTypePtr)
					pushAddress(reinterpret_cast<Address>(&(paramList[curParam].integer)));
				else
					return(0);
			}
			curParam++;
		}
	}

	execute(moduleIdPtr);

	if (returnVal)
		memcpy(returnVal, &g_ablExec->returnValue, sizeof(StackItem));

	return(g_ablExec->execStatementCount);
}

//***************************************************************************

long ABLi_deleteModule (SymTableNodePtr moduleIdPtr) {

	return(ABL_NO_ERR);
}

//***************************************************************************

void ABLi_close (void) {

	if (!codeBuffer)
		return;

	UserFile::cleanup();

	destroyModuleRegistry();

	destroyLibraryRegistry();

	if (StaticVariablesSizes) {
		ABL_free(ABLPool::Stack, StaticVariablesSizes);
		StaticVariablesSizes = nullptr;
	}

	if (EternalVariablesSizes) {
		ABL_free(ABLPool::Stack, EternalVariablesSizes);
		EternalVariablesSizes = nullptr;
	}
	
	if (codeBuffer) {
		ABL_free(ABLPool::Code, codeBuffer);
		codeBuffer = nullptr;
	}

	std::vector<StackItem>().swap(stackStore);
	stack = nullptr;
	stackCount = 0;

	if (debugger) {
		delete debugger;
		debugger = nullptr;
	}

	ABL_CloseProfileLog();

	ABLenabled = false;

	// Last: everything above may still hand blocks back.
	ABL_closeArena();
}

//***************************************************************************

ABLModulePtr ABLi_loadLibrary (mc2::czstring sourceFileName, long* numErrors, long* numLinesProcessed, long* numFilesProcessed, bool printLines, bool createInstance) {

	// Library symbols need their owning instance during preprocessing.
	ABLModulePtr library = new ABLModule;
	if (!library)
		ABL_Fatal(0, "ABL: no RAM for library");

	CurLibrary = library;

	int libraryHandle = ABLi_preProcess(sourceFileName, numErrors, numLinesProcessed, numFilesProcessed, printLines);
	if (libraryHandle < 0 || libraryHandle < (NumModulesRegistered - 1)) {
		delete library;
		library = nullptr;
		CurLibrary = nullptr;
		return(nullptr);
	}

	CurLibrary = nullptr;

	LibraryInstanceRegistry[numLibrariesLoaded] = library;
	numLibrariesLoaded++;

	if (!createInstance) {
		CurLibrary = nullptr;
		return(library);
	}

	long err = library->init(libraryHandle);
	ABL_Assert(err == ABL_NO_ERR, err, " Error Loading ABL Library ");

	library->setName(sourceFileName);

	return(library);
}

//***************************************************************************

ABLParamPtr ABLi_createParamList (long numParameters) {

	if (numParameters) {
		ABLParamPtr paramList = (ABLParamPtr)ABL_alloc(ABLPool::Stack, sizeof(ABLParam) * (numParameters + 1));
		if (!paramList)
			ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc paramList ");
		memset(paramList, 0, sizeof(ABLParam) * (numParameters + 1)); 
		return(paramList);
	}
	return(nullptr);
}

//***************************************************************************

void ABLi_setIntegerParam (ABLParamPtr paramList, int index, int value) {

	if (paramList) {
		paramList[index].type = ABL_PARAM_INTEGER;
		paramList[index].integer = value;
	}
}

//***************************************************************************

void ABLi_setRealParam (ABLParamPtr paramList, int index, float value) {

	if (paramList) {
		paramList[index].type = ABL_PARAM_REAL;
		paramList[index].real = value;
	}
}

//***************************************************************************

void ABLi_deleteParamList (ABLParamPtr paramList) {

	if (paramList)
		ABL_free(ABLPool::Stack, paramList);
}

//***************************************************************************

ABLModulePtr ABLi_getModule (long id) {

	if ((id >= 0) && (id < NumModules))
		return(ModuleInstanceRegistry[id]);
	return(nullptr);
}

//***************************************************************************

bool ABLi_enabled (void) {

	return(ABLenabled);
}

//***************************************************************************

void ABLi_addFunction (mc2::czstring name,
					   bool isOrder,
					   const char* paramList,
					   const char* returnType,
					   void (*codeCallback)(void)) {

	enterStandardRoutine(name, -1, isOrder, paramList, returnType, codeCallback);
}

//***************************************************************************

int ABLi_registerInteger (char* name, int* address, int numElements) {

	if (strlen(name) >= MAXLEN_TOKENSTRING)
		ABL_Fatal(0, " ABLi_registerInteger: variable name too long ");

	level = 0;
	strcpy(wordString, name);
	SymTableNodePtr idPtr = nullptr;
	searchAndEnterThisTable (idPtr, SymTableDisplay[0]);

	if (numElements == 1) {
		idPtr->library = nullptr;
		idPtr->defn.key = DFN_VAR;
		idPtr->labelIndex = 0;
		idPtr->typePtr = setType(IntegerTypePtr);
		idPtr->defn.info.data.varType = VAR_TYPE_REGISTERED;
		idPtr->defn.info.data.registeredData = address;
	}
	return(ABL_NO_ERR);
}

//***************************************************************************

int ABLi_registerReal (char* name, float* address, int numElements) {

	if (strlen(name) >= MAXLEN_TOKENSTRING)
		ABL_Fatal(0, " ABLi_registerInteger: variable name too long ");

	level = 0;
	strcpy(wordString, name);
	SymTableNodePtr idPtr = nullptr;
	searchAndEnterThisTable (idPtr, SymTableDisplay[0]);

	idPtr->library = nullptr;
	idPtr->defn.key = DFN_VAR;
	idPtr->labelIndex = 0;
	idPtr->typePtr = setType(RealTypePtr);
	idPtr->defn.info.data.varType = VAR_TYPE_REGISTERED;
	idPtr->defn.info.data.registeredData = address;
	return(ABL_NO_ERR);
}

//***************************************************************************

bool ABLi_getSkipOrder (void) {

	return(SkipOrder);
}

//***************************************************************************

void ABLi_resetOrders (void) {

	g_ablExec->CurModule->resetOrderCallFlags();
}

//***************************************************************************

long ABLi_getCurrentState (void) {

	if (g_ablExec->CurFSM)
		return(g_ablExec->CurFSM->getStateHandle());
	return(0);
}

//***************************************************************************

void ABLi_transState (long newStateHandle) {

	if (g_ablExec->CurFSM && (newStateHandle > 0) && (newStateHandle < ModuleRegistry[g_ablExec->CurFSM->getHandle()].numStateHandles))
		transState(ModuleRegistry[g_ablExec->CurFSM->getHandle()].stateHandles[newStateHandle].state);
}

//***************************************************************************
// MODULE routines
//***************************************************************************

SymTableNodePtr moduleHeader (void) {

	SymTableNodePtr moduleIdPtr = nullptr;
	bool isFSM = false;
	if (CurLibrary)
		ifTokenGetElseError(TKN_LIBRARY, ABL_ERR_SYNTAX_MISSING_LIBRARY);
	else if (curToken == TKN_FSM) {
		isFSM = true;
		getToken();
		}
	else
		ifTokenGetElseError(TKN_MODULE, ABL_ERR_SYNTAX_MISSING_MODULE);

	//------------------------------------------------------------
	// NOTE: If a module is a library, its library pointer will be
	// non-NULL.
	
	if (curToken == TKN_IDENTIFIER) {
		searchAndEnterLocalSymTable(moduleIdPtr);
		moduleIdPtr->defn.key = DFN_MODULE;
		moduleIdPtr->defn.info.routine.key = RTN_DECLARED;
		moduleIdPtr->defn.info.routine.flags = (isFSM ? ROUTINE_FLAG_FSM : 0);
		moduleIdPtr->defn.info.routine.orderCallIndex = 0;
		moduleIdPtr->defn.info.routine.numOrderCalls = 0;
		moduleIdPtr->defn.info.routine.paramCount = 0;
		moduleIdPtr->defn.info.routine.totalParamSize = 0;
		moduleIdPtr->defn.info.routine.totalLocalSize = 0;
		moduleIdPtr->defn.info.routine.params = nullptr;
		moduleIdPtr->defn.info.routine.locals = nullptr;
		moduleIdPtr->defn.info.routine.localSymTable = nullptr;
		moduleIdPtr->defn.info.routine.codeSegment = nullptr;
		moduleIdPtr->library = CurLibrary;
		moduleIdPtr->typePtr = &DummyType;
		moduleIdPtr->labelIndex = 0;
		getToken();
		}
	else
		syntaxError(ABL_ERR_SYNTAX_MISSING_IDENTIFIER);

	//--------------------------------------------
	// For now, modules do not have param lists...
	synchronize(followModuleIdList, declarationStartList, statementStartList);
	enterScope(nullptr);

	if (curToken == TKN_LPAREN) {
		long paramCount;
		long totalParamSize;
		SymTableNodePtr paramListPtr = formalParamList(&paramCount, &totalParamSize);
		//if (forwardFlag)
		//	syntaxError(ABL_ERR_SYNTAX_ALREADY_FORWARDED);
		//else {
			moduleIdPtr->defn.info.routine.paramCount = static_cast<unsigned char>(paramCount);
			moduleIdPtr->defn.info.routine.totalParamSize = static_cast<unsigned char>(totalParamSize);
			moduleIdPtr->defn.info.routine.params = paramListPtr;
		//}
	}
	//else if (!forwardFlag) {
	//	functionIdPtr->defn.info.routine.paramCount = 0;
	//	functionIdPtr->defn.info.routine.totalParamSize = 0;
	//	functionIdPtr->defn.info.routine.params = NULL;
	//}

	//-----------------------------
	// ROBUSTNESS GUARD (2026-06-11, missing-ABL-file playtest incident):
	// moduleIdPtr is only assigned inside the `curToken == TKN_IDENTIFIER` branch above.
	// When the source file opened but holds invalid/garbage content (e.g. a missing
	// warriors/nop.abl whose open silently failed -- ablFileOpenCB's STOP() is a no-op in
	// RelWithDebInfo -- so getToken() scans junk), curToken is never TKN_IDENTIFIER and
	// moduleIdPtr stays NULL.  The writes below would then be a WRITE AV at offset 0x70.
	// Bail out cleanly so the caller (ABLi_preProcess) can fail loud instead of crashing.
	if (moduleIdPtr == nullptr)
		return(nullptr);

	//-----------------------------
	// Now, check if return type...
	moduleIdPtr->typePtr = nullptr;
	SymTableNodePtr typeIdPtr = nullptr;
	if (curToken == TKN_COLON) {
		getToken();
		if (curToken == TKN_IDENTIFIER) {
			searchAndFindAllSymTables(typeIdPtr);
			
			if (typeIdPtr->defn.key != DFN_TYPE)
				syntaxError(ABL_ERR_SYNTAX_INVALID_TYPE);

			//if (!forwardFlag)
				moduleIdPtr->typePtr = typeIdPtr->typePtr;

			getToken();
			}
		else {
			syntaxError(ABL_ERR_SYNTAX_MISSING_IDENTIFIER);
			moduleIdPtr->typePtr = &DummyType;
		}

		//if (forwardFlag)
		//	syntaxError(ABL_ERR_SYNTAX_ALREADY_FORWARDED);
	}

	return(moduleIdPtr);
}

//***************************************************************************
// ROUTINE/FUNCTION routines
//***************************************************************************

void routine (void) {

	SymTableNodePtr routineIdPtr = nullptr;
	
	//------------------------------------------------------------------------
	// Do we want to have "procedures", or do functions handle both cases (with
	// and without return values)? For now, functions handle both...
	routineIdPtr = functionHeader();

	//------------------------------------------------------------------
	// We need to save a pointer to the current routine we're parsing...
	SymTableNodePtr prevRoutineIdPtr = CurRoutineIdPtr;
	CurRoutineIdPtr = routineIdPtr;

	//---------------------
	// Error synchronize...
	synchronize(followHeaderList, declarationStartList, statementStartList);

	if (curToken == TKN_SEMICOLON)
		getToken();
	else if (tokenIn(declarationStartList) || tokenIn(statementStartList))
		syntaxError(ABL_ERR_SYNTAX_MISSING_SEMICOLON);

	if (strcmp(wordString, "forward") != 0) {
		routineIdPtr->defn.info.routine.key = RTN_DECLARED;
		//analyzeRoutineHeader(routineIdPtr);

		routineIdPtr->defn.info.routine.locals = nullptr;

		declarations(routineIdPtr, false);
		synchronize(followRoutineDeclsList, nullptr, nullptr);
		if (curToken != TKN_CODE)
			syntaxError(ABL_ERR_SYNTAX_MISSING_CODE);
		crunchToken();

		blockType = BLOCK_ROUTINE;
		blockFlag = true;

		routineIdPtr->defn.info.routine.orderCallIndex = static_cast<unsigned short>(NumOrderCalls);

		//compoundStatement();
		getToken();

		TokenCodeType endToken = TKN_END_FUNCTION;
		if (routineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_STATE)
			endToken = TKN_END_STATE;
		else if (routineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER)
			endToken = TKN_END_ORDER;
		if (curToken != endToken)
			do {
				statement();
				while (curToken == TKN_SEMICOLON)
					getToken();
				if (curToken == endToken)
					break;
		
				//------------------------------------
				// Synchronize after possible error...
				synchronize(statementStartList, nullptr, nullptr);
			} while (tokenIn(statementStartList));
		ifTokenGetElseError(endToken, ABL_ERR_SYNTAX_MISSING_END_FUNCTION);
		blockFlag = false;


		routineIdPtr->defn.info.routine.numOrderCalls = static_cast<unsigned short>(NumOrderCalls) - routineIdPtr->defn.info.routine.orderCallIndex;
		routineIdPtr->defn.info.routine.codeSegment = createCodeSegment(routineIdPtr->defn.info.routine.codeSegmentSize);
		analyzeBlock(routineIdPtr->defn.info.routine.codeSegment);
		}
	else {
		getToken();
		routineIdPtr->defn.info.routine.key = RTN_FORWARD;
		analyzeRoutineHeader(routineIdPtr);
	}

	if (routineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_STATE) {
		//-----------------------------------
		// Add it to the state handle list...
		if (NumStateHandles == MAX_STATE_HANDLES_PER_MODULE)
			ABL_Fatal(0, "ABL: too many states in fsm [19 max]");
		if (strlen(routineIdPtr->name) >= sizeof(StateHandleList[0].name))
			ABL_Fatal(0, "ABL: state name too long [127 max]");
		strcpy(StateHandleList[NumStateHandles].name, routineIdPtr->name);
		StateHandleList[NumStateHandles].state = routineIdPtr;
		NumStateHandles++;
	}

	routineIdPtr->defn.info.routine.localSymTable = exitScope();

	CurRoutineIdPtr = prevRoutineIdPtr;
}

//***************************************************************************

SymTableNodePtr forwardState (mc2::czstring stateName) {

	SymTableNodePtr stateSymbol = searchSymTableForState(stateName, SymTableDisplay[1]);
	if (stateSymbol)
		ABL_Fatal(0, " ABL.forwardState: State symbol should not exist ");
	stateSymbol = enterSymTable(stateName, &SymTableDisplay[1]);
	stateSymbol->defn.key = DFN_FUNCTION;
	stateSymbol->defn.info.routine.key = RTN_FORWARD;
	stateSymbol->defn.info.routine.paramCount = 0;
	stateSymbol->defn.info.routine.flags = ROUTINE_FLAG_STATE;
	stateSymbol->defn.info.routine.orderCallIndex = 0;
	stateSymbol->defn.info.routine.numOrderCalls = 0;
	stateSymbol->defn.info.routine.totalParamSize = 0;
	stateSymbol->defn.info.routine.totalLocalSize = 0;
	stateSymbol->defn.info.routine.params = nullptr;
	stateSymbol->defn.info.routine.locals = nullptr;
	stateSymbol->defn.info.routine.localSymTable = nullptr;
	stateSymbol->defn.info.routine.codeSegment = nullptr;
	stateSymbol->library = CurLibrary;
	stateSymbol->typePtr = nullptr;
	stateSymbol->labelIndex = 0;
	//-------------------
	// Force the level...
	stateSymbol->level = 1;
	return(stateSymbol);
}

//***************************************************************************

SymTableNodePtr functionHeader (void) {

	bool isState = false;
	bool isOrder = false;
	if (curToken == TKN_STATE)
		isState = true;
	else if (curToken == TKN_ORDER)
		isOrder = true;
	getToken();
	SymTableNodePtr functionIdPtr = nullptr;
	SymTableNodePtr typeIdPtr = nullptr;

	bool forwardFlag = false;

	if (curToken == TKN_IDENTIFIER) {
		functionIdPtr = searchSymTableForFunction(wordString, SymTableDisplay[level]);
		if (!functionIdPtr) {
			enterLocalSymTable(functionIdPtr);
			functionIdPtr->defn.key = DFN_FUNCTION;
			functionIdPtr->defn.info.routine.key = RTN_DECLARED;
			functionIdPtr->defn.info.routine.paramCount = 0;
			functionIdPtr->defn.info.routine.flags = 0;
			functionIdPtr->defn.info.routine.orderCallIndex = 0;
			functionIdPtr->defn.info.routine.numOrderCalls = 0;
			functionIdPtr->defn.info.routine.totalParamSize = 0;
			functionIdPtr->defn.info.routine.totalLocalSize = 0;
			functionIdPtr->defn.info.routine.params = nullptr;
			functionIdPtr->defn.info.routine.locals = nullptr;
			functionIdPtr->defn.info.routine.localSymTable = nullptr;
			functionIdPtr->defn.info.routine.codeSegment = nullptr;
			if (isOrder)
				functionIdPtr->defn.info.routine.flags |= ROUTINE_FLAG_ORDER;
			if (isState)
				functionIdPtr->defn.info.routine.flags |= ROUTINE_FLAG_STATE;
			functionIdPtr->library = CurLibrary;
			functionIdPtr->typePtr = &DummyType;
			functionIdPtr->labelIndex = 0;
			}
		else if ((functionIdPtr->defn.key == DFN_FUNCTION) &&
				 (functionIdPtr->defn.info.routine.key == RTN_FORWARD))
			forwardFlag = true;
		else
			syntaxError(ABL_ERR_SYNTAX_REDEFINED_IDENTIFIER);
		getToken();
	}

	//---------------------
	// Error synchronize...
	synchronize(followFunctionIdList, declarationStartList, statementStartList);

	enterScope(nullptr);

	if (curToken == TKN_LPAREN) {
		long paramCount;
		long totalParamSize;
		SymTableNodePtr paramListPtr = formalParamList(&paramCount, &totalParamSize);
		if (forwardFlag)
			syntaxError(ABL_ERR_SYNTAX_ALREADY_FORWARDED);
		else {
			functionIdPtr->defn.info.routine.paramCount = static_cast<unsigned char>(paramCount);
			functionIdPtr->defn.info.routine.totalParamSize = static_cast<unsigned char>(totalParamSize);
			functionIdPtr->defn.info.routine.params = paramListPtr;
		}
		}
	else if (!forwardFlag) {
		functionIdPtr->defn.info.routine.paramCount = 0;
		functionIdPtr->defn.info.routine.totalParamSize = 0;
		functionIdPtr->defn.info.routine.params = nullptr;
	}

	//--------------------------------------------
	// Now, check if return type and/or forward...
	functionIdPtr->typePtr = nullptr;
	bool hasIntegerReturnValue = false;
	if (curToken == TKN_COLON) {
		getToken();
		if (curToken == TKN_IDENTIFIER) {
			searchAndFindAllSymTables(typeIdPtr);
			
			if (typeIdPtr->defn.key != DFN_TYPE)
				syntaxError(ABL_ERR_SYNTAX_INVALID_TYPE);

			// Stock and mod scripts declare array returns; execStdReturn bounds them to the return slot.

			hasIntegerReturnValue = (typeIdPtr->typePtr == IntegerTypePtr);

			if (!forwardFlag)
				functionIdPtr->typePtr = typeIdPtr->typePtr;

			getToken();
			}
		else {
			syntaxError(ABL_ERR_SYNTAX_MISSING_IDENTIFIER);
			functionIdPtr->typePtr = &DummyType;
		}

		if (forwardFlag)
			syntaxError(ABL_ERR_SYNTAX_ALREADY_FORWARDED);
	}

	if (isOrder && !hasIntegerReturnValue)
		syntaxError(ABL_ERR_SYNTAX_ORDER_RETURNTYPE);

	return(functionIdPtr);
}

//***************************************************************************

SymTableNodePtr formalParamList (long* count, long* totalSize) {

	SymTableNodePtr paramIdPtr;
	SymTableNodePtr firstIdPtr;
	SymTableNodePtr lastIdPtr = nullptr;
	SymTableNodePtr paramListPtr = nullptr;
	SymTableNodePtr typeIdPtr;
	TypePtr paramTypePtr;
	DefinitionType paramDefn = DFN_UNDEFINED;
	long paramCount = 0;
	long paramOffset = STACK_FRAME_HEADER_SIZE;

	getToken();

	while ((curToken == TKN_IDENTIFIER) || (curToken == TKN_REF)) {
		firstIdPtr = nullptr;

		if (curToken == TKN_REF) {
			paramDefn = DFN_REFPARAM;
			getToken();
			}
		else
			paramDefn = DFN_VALPARAM;

		//-----------------------------------
		// Grab the type of this parameter...
		if (curToken == TKN_IDENTIFIER) {
			searchAndFindAllSymTables(typeIdPtr);
			if (typeIdPtr->defn.key != DFN_TYPE)
				syntaxError(ABL_ERR_SYNTAX_INVALID_TYPE);
			paramTypePtr = typeIdPtr->typePtr;
			getToken();
			}
		else {
			syntaxError(ABL_ERR_SYNTAX_MISSING_IDENTIFIER);
			paramTypePtr = &DummyType;
		}

		//------------------------------------------------------------
		// Is this an array parameter? If so, parse the dimensions and
		// whether it's an open array...
		if (curToken == TKN_LBRACKET) {
		}
		
		//-----------------------------------------------
		// Now grab the variable id for this parameter...
		if (curToken == TKN_IDENTIFIER) {
			searchAndEnterLocalSymTable(paramIdPtr);
			paramIdPtr->defn.key = paramDefn;
			paramIdPtr->labelIndex = 0;

			paramIdPtr->typePtr = paramTypePtr;
			paramIdPtr->defn.info.data.offset = paramOffset++;
			
			paramCount++;

			if (paramListPtr == nullptr)
				paramListPtr = paramIdPtr;
			if (lastIdPtr != nullptr)
				lastIdPtr->next = paramIdPtr;
			lastIdPtr = paramIdPtr;
			//if (firstIdPtr == NULL)
			//	firstIdPtr = lastIdPtr = paramIdPtr;
			//else {
			//	lastIdPtr->next = paramIdPtr;
			//	lastIdPtr = paramIdPtr;
			//}

			getToken();
			//ifTokenGet(TKN_COMMA);
			}
		else {
			syntaxError(ABL_ERR_SYNTAX_MISSING_IDENTIFIER);
			//--------------------------------
			// Do we need to synchronize here?
		}


		//------------------------------------------------------------------
		// Assign the offset and the type to all param ids in the sublist...
		//for (paramIdPtr = firstIdPtr; paramIdPtr != NULL; paramIdPtr = paramIdPtr->next) {
		//	paramIdPtr->typePtr = paramTypePtr;
		//	paramIdPtr->defn.info.data.offset = paramOffset++;
		//}

		//-----------------------------------------------
		// Link this list to the list of all param ids...
		//if (prevLastIdPtr != NULL)
		//	prevLastIdPtr->next = firstIdPtr;
		//prevLastIdPtr = lastIdPtr;

		//-------------------------------
		// Error synch... should be ; or )
		synchronize(followParamsList, nullptr, nullptr);
		ifTokenGet(TKN_COMMA);
	}

	ifTokenGetElseError(TKN_RPAREN, ABL_ERR_SYNTAX_MISSING_RPAREN);
	*count = paramCount;
	*totalSize = paramOffset - STACK_FRAME_HEADER_SIZE;

	if (paramCount > 255)
		syntaxError(ABL_ERR_SYNTAX_TOO_MANY_FORMAL_PARAMETERS);

	return(paramListPtr);
}

//***************************************************************************

TypePtr routineCall (SymTableNodePtr routineIdPtr, long paramCheckFlag) {

	SymTableNodePtr thisRoutineIdPtr = CurRoutineIdPtr;
	TypePtr resultType = nullptr;

	if ((routineIdPtr->defn.info.routine.key == RTN_DECLARED) ||
		(routineIdPtr->defn.info.routine.key == RTN_FORWARD) ||
		!paramCheckFlag)
		resultType = declaredRoutineCall(routineIdPtr, paramCheckFlag);
	else
		resultType = standardRoutineCall(routineIdPtr);

	CurRoutineIdPtr = thisRoutineIdPtr;
	return(resultType);
}

//***************************************************************************

TypePtr declaredRoutineCall (SymTableNodePtr routineIdPtr, long paramCheckFlag) {

	actualParamList(routineIdPtr, paramCheckFlag);
	return(/*routineIdPtr->defn.key == DFN_PROCEDURE ? NULL :*/ routineIdPtr->typePtr);
}

//***************************************************************************

void actualParamList (SymTableNodePtr routineIdPtr, long paramCheckFlag) {

	SymTableNodePtr formalParamIdPtr = nullptr;
	DefinitionType formalParamDefn = DFN_UNDEFINED;
	TypePtr formalParamTypePtr = nullptr;
	TypePtr actualParamTypePtr = nullptr;

	if (paramCheckFlag)
		formalParamIdPtr = routineIdPtr->defn.info.routine.params;

	if (curToken == TKN_LPAREN) {
		//-----------------------------------------------
		// Loop to process actual parameter expression...
		do {
			//----------------------------------------------------
			// Obtain info about the corresponding formal param...
			if (paramCheckFlag && (formalParamIdPtr != nullptr)) {
				formalParamDefn = formalParamIdPtr->defn.key;
				formalParamTypePtr = formalParamIdPtr->typePtr;
			}

			getToken();

			if ((formalParamIdPtr == nullptr) || (formalParamDefn == DFN_VALPARAM) || !paramCheckFlag) {
				//--------------
				// VAL Params...
				actualParamTypePtr = expression();
				if (paramCheckFlag && (formalParamIdPtr != nullptr) && !isAssignTypeCompatible(formalParamTypePtr, actualParamTypePtr))
					syntaxError(ABL_ERR_SYNTAX_INCOMPATIBLE_TYPES);
				}
			else {
				//--------------
				// VAR params...
				if (curToken == TKN_IDENTIFIER) {
					SymTableNodePtr idPtr;
					searchAndFindAllSymTables(idPtr);
					actualParamTypePtr = variable(idPtr);

					if (formalParamTypePtr != actualParamTypePtr)
						syntaxError(ABL_ERR_SYNTAX_INCOMPATIBLE_TYPES);
					}
				else {
					//------------------------------------------------------------------
					// Not a variable--parse the expression anyway for error recovery...
					actualParamTypePtr = expression();
					syntaxError(ABL_ERR_SYNTAX_INVALID_REF_PARAM);
				}
			}

			//---------------------------------------
			// More actual params than formal params?
			if (paramCheckFlag) {
				if (formalParamIdPtr == nullptr)
					syntaxError(ABL_ERR_SYNTAX_WRONG_NUMBER_OF_PARAMS);
				else
					formalParamIdPtr = formalParamIdPtr->next;
			}

			//---------------
			// Error synch...
			synchronize(followParamList, statementEndList, nullptr);
		} while (curToken == TKN_COMMA);

		ifTokenGetElseError(TKN_RPAREN, ABL_ERR_SYNTAX_MISSING_RPAREN);
	}

	//----------------------------------------
	// Fewer actual params than formal params?
	if (paramCheckFlag && (formalParamIdPtr != nullptr))
		syntaxError(ABL_ERR_SYNTAX_WRONG_NUMBER_OF_PARAMS);
}

//***************************************************************************

extern char	SetStateDebugStr[256];

void transState (SymTableNodePtr newState) {

	if (g_ablExec->CurFSM) {
		g_ablExec->CurFSM->setPrevState(g_ablExec->CurFSM->getState());
		g_ablExec->CurFSM->setState(newState);
		mc2::fmt::to_buf(SetStateDebugStr, "{}:{}, line {:d}", mc2::fmt::safe(g_ablExec->CurModule->getFileName()), mc2::fmt::safe(newState->name), mc2::fmt::as_i32(g_ablExec->execLineNumber));
		g_ablExec->NewStateSet = true;
	}
}

//***************************************************************************

