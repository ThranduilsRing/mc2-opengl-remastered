//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLENV.CPP
//
//***************************************************************************

#include "mc2_czstring.h"
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<cassert>
#include<new>
#include<set>
#include<string_view>
#include<vector>
#include"platform_str.h"
#include "Gates/gate.h"

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

#ifndef ABLENV_H
#include"ablenv.h"
#include"sim_identity.h"
#endif

#ifndef ABLDBUG_H
#include"abldbug.h"
#endif
#include "../GameOS/gameos/utils/mc2log.h"
#include "mc2fmt.h"
#include "abl_value_archive.h"
#include "abl_module_archive.h"
#include "abl_environment_archive.h"

// ABLFile's callback interface owns the stream; each value record is bounded
// and decoded completely before its values are applied to the VM.
template<class T> static void writeAblRecord(ABLFile* file, T value, uint32_t tag) {

	auto bytes = mc2::save::encodeTaggedRecord(std::move(value), tag);
	if (bytes.empty()) {
		ABL_Fatal(0, "ABL: invalid value record for writing");
		return;
	}
	// writeInt callbacks return a status, while write returns a byte count.
	mc2::save::Archive framed;
	auto size = static_cast<uint32_t>(bytes.size());
	framed(size);
	framed.raw(bytes);
	if (file->write(const_cast<unsigned char*>(framed.bytes().data()), static_cast<long>(framed.bytes().size())) != framed.bytes().size())
		ABL_Fatal(0, "ABL: unable to write value record");
}

template<class T> static T readAblRecord(ABLFile* file, uint32_t tag) {

	const auto size = file->readInt();
	if (size < 8 || size > mc2::save::maxFileBytes || (file->remaining() >= 0 && size > file->remaining()))
		ABL_Fatal(0, "ABL: invalid value record size");
	mc2::save::Bytes bytes(static_cast<size_t>(size));
	T value;
	if (file->read(bytes.data(), size) != size || !mc2::save::decodeTaggedRecord(bytes, value, tag))
		ABL_Fatal(0, "ABL: invalid value record");
	return value;
}

static void readAblVariable(ABLFile* file, StackItem& slot, long size) {

	const auto value = readAblRecord<mc2::save::AblVariableRecord>(file, mc2::save::tagAblVariable);
	if (size < 0 || !mc2::save::restoreAblVariable(value, slot, static_cast<size_t>(size)))
		ABL_Fatal(0, "ABL: variable size differs from compiled declaration");
}

//---------------------------------------------------------------------------
// A scalar static or eternal is saved as a whole StackItem, but only its 4-byte integer / real / byte arm is meaningful. The union also has
// an Address arm: writing the slot as it stands would put the upper half of a process address (or a stale 4 bytes) in the save, so the
// file holds the 4-byte payload zero-extended to the slot size and the file layout is unchanged.
static void writeScalarSlot (ABLFile* file, const StackItem* slot) {

	StackItem image;
	memset(&image, 0, sizeof(image));
	memcpy(&image, slot, sizeof(image.integer));
	file->write(reinterpret_cast<unsigned char*>(&image), sizeof(StackItem));
}

// A saved slot whose upper bytes are not zero was written before the writer zero-extended, or holds a pointer arm; dropping those bytes keeps
// a stale process address out of the script's variables.
static void clearScalarSlotPointerArm (StackItem* slot) {

	if (sizeof(StackItem) <= sizeof(slot->integer))
		return;
	unsigned char* upper = reinterpret_cast<unsigned char*>(slot) + sizeof(slot->integer);
	for (size_t i = 0; i < sizeof(StackItem) - sizeof(slot->integer); i++) {
		if (upper[i]) {
			MC2_LOG_ONCE(Sim, Warn, "[ABL_SAVE_SLOT_POINTER_ARM] a saved ABL scalar slot carries bytes past its 4-byte value; dropped");
			memset(upper, 0, sizeof(StackItem) - sizeof(slot->integer));
			return;
		}
	}
}

//***************************************************************************
int32_t ABLi_preProcess (mc2::czstring sourceFileName,
					  long* numErrors = nullptr,
					  long* numLinesProcessed = nullptr,
					  long* numFilesProcessed = nullptr,
					  bool printLines = false);

ABLModulePtr ABLi_loadLibrary (mc2::czstring sourceFileName,
					   long* numErrors = nullptr,
					   long* numLinesProcessed = nullptr,
					   long* numFilesProcessed = nullptr,
					   bool printLines = false,
					   bool createInstance = true);

//-------------------
// EXTERNAL variables
extern int32_t          level;
extern int32_t          lineNumber;
extern int32_t          FileNumber;
extern long				errorCount;
extern int              execStatementCount;

extern TokenCodeType	curToken;
extern char				wordString[];
extern SymTableNodePtr	symTableDisplay[];
extern bool				blockFlag;
extern BlockType		blockType;
extern bool				printFlag;
extern SymTableNodePtr	CurModuleIdPtr;
extern SymTableNodePtr	CurRoutineIdPtr;
extern long				CurModuleHandle;
extern bool				CallModuleInit;

extern Type				DummyType;
extern char*			codeBuffer;
extern char*			codeBufferPtr;
extern StackItem*		stack;
//extern StackItem*		eternalStack;
extern StackItemPtr		tos;
extern StackItemPtr		stackFrameBasePtr;
extern long				eternalOffset;

extern TokenCodeType	statementStartList[];
extern TokenCodeType	statementEndList[];
extern TokenCodeType	declarationStartList[];

extern char				tokenString[MAXLEN_TOKENSTRING];

extern CharCodeType		charTable[256];
extern ABLFile*			sourceFile;

extern char				sourceBuffer[MAXLEN_SOURCELINE];
extern long				bufferOffset;
extern char*			bufferp;
extern char*			tokenp;

extern long				digitCount;
extern bool				countError;

extern bool				eofFlag;
extern int 				pageNumber;

extern SymTableNodePtr	SymTableDisplay[MAX_NESTING_LEVEL];
extern TypePtr			IntegerTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;

extern unsigned long*	OrderCompletionFlags;
extern StackItemPtr		StaticDataPtr;
extern StackItem		returnValue;

extern DebuggerPtr		debugger;
extern long*			EternalVariablesSizes;

//-----------------------
// CLASS static variables
int32_t             NumModules = 0;

//-----------------
// GLOBAL variables
ModuleEntryPtr		ModuleRegistry = nullptr;
ABLModulePtr*		ModuleInstanceRegistry = nullptr;
int32_t				MaxModules = 0;
int32_t				NumModulesRegistered = 0;
int32_t				NumModuleInstances = 0;
int32_t				MaxWatchesPerModule = 20;
int32_t				MaxBreakPointsPerModule = 20;
ABLModulePtr		CurModule = nullptr;
ABLModulePtr		CurFSM = nullptr;
ABLModulePtr		CurLibrary = nullptr;
ABLModulePtr*		LibraryInstanceRegistry = nullptr;
int32_t				NumStateTransitions = 0;
int32_t				MaxLibraries = 0;
bool				NewStateSet = false;
extern int32_t	    numLibrariesLoaded;

extern int32_t	    NumExecutions;
int32_t				CallStackLevel = 0;

constexpr int MAX_PROFILE_LINELEN = 128;
constexpr int MAX_PROFILE_LINES = 256;

long NumProfileLogLines = 0;
int TotalProfileLogLines = 0;
char ProfileLogBuffer[MAX_PROFILE_LINES][MAX_PROFILE_LINELEN];
ABLFile* ProfileLog = nullptr;
long ProfileLogFunctionTimeLimit = 5;

UserFilePtr			UserFile::files[MAX_USER_FILES];

static int32_t ABL_ENV_MARK = 999;
// Order-completion flags trail the module records (BUGS row 71). The section is written only when some instance has a
// completed order recorded, so a save without one keeps the legacy bytes and a legacy save loads as before (flags clear).
// The ABL save is a file of its own: the section is present only when the bytes left after the module records are exactly
// the mark plus the instances' flag words (BUGS row 77); any other tail is refused whole and every flag stays clear.
static constexpr int32_t ABL_ORDER_FLAGS_MARK = 0x4F524446;

static long orderFlagWords (ABLModule* module) {
	if (!module || !module->getOrderCallFlags())
		return 0;
	long numOrderCalls = ModuleRegistry[module->getHandle()].numOrderCalls;
	return numOrderCalls ? 1 + numOrderCalls / 32 : 0;
}

//***************************************************************************
// PROFILING LOG routines
//***************************************************************************

void DumpProfileLog (void) {

	//----------------
	// Dump to file...
	for (long i = 0; i < NumProfileLogLines; i++)
		ProfileLog->writeString(ProfileLogBuffer[i]);
	NumProfileLogLines = 0;
}

//---------------------------------------------------------------------------

void ABL_CloseProfileLog (void) {

	if (ProfileLog) 
	{
		DumpProfileLog();
		char s[512];
		mc2::fmt::to_buf(s, "\nNum Total Lines = {:d}\n", mc2::fmt::as_i32(TotalProfileLogLines));
		ProfileLog->writeString(s);
		ProfileLog->close();
		delete ProfileLog;
		ProfileLog = nullptr;
		NumProfileLogLines = 0;
		TotalProfileLogLines = 0;
	}
}

//---------------------------------------------------------------------------

void ABL_OpenProfileLog (void) {

	if (ProfileLog)
		ABL_CloseProfileLog();

	NumProfileLogLines = 0;
	ProfileLog = new ABLFile;
	if (!ProfileLog)
		ABL_Fatal(0, " unable to malloc ABL ProfileLog ");
	if (ProfileLog->create("abl.log") != ABL_NO_ERR)
		ABL_Fatal(0, " unable to create ABL ProfileLog ");
}

//---------------------------------------------------------------------------

void ABL_AddToProfileLog (char* profileString) {

	if (NumProfileLogLines == MAX_PROFILE_LINES)
		DumpProfileLog();

	strncpy(ProfileLogBuffer[NumProfileLogLines], profileString, MAX_PROFILE_LINELEN - 1);
	ProfileLogBuffer[NumProfileLogLines][MAX_PROFILE_LINELEN - 1] = '\0';
	NumProfileLogLines++;
	TotalProfileLogLines++;
}

//***************************************************************************
// USER FILE routines
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

void UserFile::dump (void) {

	for (long i = 0; i < numLines; i++)
		filePtr.writeString(lines[i]);
	numLines = 0;
}

//---------------------------------------------------------------------------

void UserFile::close (void) {

	if (inUse) {
		dump();
		char s[512];
		mc2::fmt::to_buf(s, "\nNum Total Lines = {:d}\n", mc2::fmt::as_i32(totalLines));
		filePtr.writeString(s);
		filePtr.close();
		inUse = false;
		numLines = 0;
		totalLines = 0;
	}
}

//---------------------------------------------------------------------------

long UserFile::open (mc2::czstring fileName) {

	numLines = 0;
	totalLines = 0;
	if (filePtr.create(fileName) != ABL_NO_ERR)
		return(-1);

	inUse = true;
	return(0);
}

//---------------------------------------------------------------------------

void UserFile::write (mc2::czstring s) {

	static char buffer[MAX_USER_FILE_LINELEN];
	
	if (numLines == MAX_USER_FILE_LINES)
		dump();
	
    //sebi !NB
	//if (strlen(s) > (MAX_USER_FILE_LINELEN - 1))
	//	s[MAX_USER_FILE_LINELEN - 1] = '\0';

	//sprintf(buffer, "%s\n", s);
	S_snprintf(buffer, MAX_USER_FILE_LINELEN - 1, "%s\n", s);
	
    //sebi 
    if (strlen(s) > (MAX_USER_FILE_LINELEN - 1))
        buffer[MAX_USER_FILE_LINELEN - 1] = '\0';
    //

	strncpy(lines[numLines], buffer, MAX_USER_FILE_LINELEN - 1);
	lines[numLines][MAX_USER_FILE_LINELEN - 1] = '\0';

	numLines++;
	totalLines++;
}

//---------------------------------------------------------------------------

UserFile* UserFile::getNewFile (void) {

	long fileHandle = -1;
    long i = 0;
	for (; i < MAX_USER_FILES; i++)
		if (!files[i]->inUse) {
			fileHandle = i;
			break;
		}
	return(files[i]);
}

//---------------------------------------------------------------------------

void UserFile::setup (void) {

	for (long i = 0; i < MAX_USER_FILES; i++) {
		files[i] = new UserFile;
		files[i]->handle = i;
		files[i]->inUse = false;

	}
}

//---------------------------------------------------------------------------

void UserFile::cleanup (void) {

	for (auto & file : files) 
	{
		if (file) {
			if (file->inUse)
				file->close();
			delete file;
			file = nullptr;
		}
	}
}

//***************************************************************************
// MODULE REGISTRY routines
//***************************************************************************

void initModuleRegistry (long maxModules) {

	//---------------------------------------------------------------------
	// First, set the max number of modules that may be loaded into the ABL
	// environment at a time...
	MaxModules = maxModules;

	//------------------------------
	// Create the module registry...
	ModuleRegistry = static_cast<ModuleEntryPtr>(ABL_alloc(ABLPool::Stack, sizeof(ModuleEntry) * MaxModules));
	if (!ModuleRegistry)
		ABL_Fatal(0, " ABL: Unable to AblStackHeap->malloc Module Registry ");
	memset(ModuleRegistry, 0, sizeof(ModuleEntry) * MaxModules);

	//-------------------------------------------------
	// Create the active (ABLModule) module registry...
	ModuleInstanceRegistry = static_cast<ABLModulePtr*>(ABL_alloc(ABLPool::Stack, sizeof(ABLModulePtr) * MaxModules));
	if (!ModuleInstanceRegistry)
		ABL_Fatal(0, " ABL: Unable to malloc AblStackHeap->Module Instance Registry ");
	memset(ModuleInstanceRegistry, 0, sizeof(ABLModulePtr) * MaxModules);
}

//***************************************************************************

void destroyModuleRegistry (void) {

	//-----------------------------------------------------------
	// First, go through the registry, free'n each module and its
	// associated data...
	for (long i = 0; i < NumModulesRegistered; i++) {
		ABL_free(ABLPool::Stack, ModuleRegistry[i].fileName);
		ModuleRegistry[i].fileName = nullptr;
		ModuleRegistry[i].moduleIdPtr = nullptr;
		for (long j = 0; j < ModuleRegistry[i].numSourceFiles; j++) {
			ABL_free(ABLPool::Stack, ModuleRegistry[i].sourceFiles[j]);
			ModuleRegistry[i].sourceFiles[j] = nullptr;
		}
	}

	ABL_free(ABLPool::Stack, ModuleRegistry);
	ModuleRegistry = nullptr;

	ABL_free(ABLPool::Stack, ModuleInstanceRegistry);
	ModuleInstanceRegistry = nullptr;
}

//***************************************************************************
// LIBRARY REGISTRY routines
//***************************************************************************

void initLibraryRegistry (long maxLibraries) {

	//-----------------------------------------------------------------------
	// First, set the max number of libraries that may be loaded into the ABL
	// environment at a time...
	MaxLibraries = maxLibraries;

	//--------------------------------------------------
	// Create the active (ABLModule) library registry...
	LibraryInstanceRegistry = static_cast<ABLModulePtr*>(ABL_alloc(ABLPool::Stack, sizeof(ABLModulePtr) * MaxLibraries));
	if (!LibraryInstanceRegistry)
		ABL_Fatal(0, " ABL: Unable to malloc AblStackHeap->Library Instance Registry ");
	memset(LibraryInstanceRegistry, 0, sizeof(ABLModulePtr) * MaxLibraries);
}

//***************************************************************************

void destroyLibraryRegistry (void) {

	//-----------------------------------------------------------------
	// Kinda need to do the same thing here as in the normal Registry.
	// Or leak o RAMA!!!!!!!
	// The actual data held by the pointer is removed in destroyModuleRegistry.
	// However, the classes holding the actual ABLModulePtr are then responsible
	// for deleting the ABLModulePtr.  Libraries have no owner class which, I'm guessing,
	// this class was supposed to do.  This class now does that!!!!
	// -fs		1/25/98
	for (long i=0;i<numLibrariesLoaded;i++)
	{
		delete LibraryInstanceRegistry[i];
		LibraryInstanceRegistry[i] = nullptr;
	}

	ABL_free(ABLPool::Stack, LibraryInstanceRegistry);
	LibraryInstanceRegistry = nullptr;
}

//***************************************************************************
// ABLMODULE class
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
long ABLi_getNumModuleInstances (void) {

	return(NumModuleInstances);
}

ABLModulePtr ABLi_getModuleInstance (long index) {

	if ((index >= 0) && (index < NumModuleInstances))
		return(ModuleInstanceRegistry[index]);
	return(nullptr);
}

unsigned long long ABLModule::digestStatics (unsigned long long h) {

	if (!staticData || (handle < 0))
		return(h);
	long numStatics = ModuleRegistry[handle].numStaticVars;
	long* sizeList = ModuleRegistry[handle].sizeStaticVars;
	for (long i = 0; i < numStatics; i++) {
		if (sizeList[i] > 0) {
			if (staticData[i].address)
				h = mc2_sim_identity::fnvBytes(h, staticData[i].address, static_cast<size_t>(sizeList[i]));
		} else
			h = mc2_sim_identity::fnvI32(h, staticData[i].integer);
	}
	return(h);
}

long ABLModule::getRealId (void)
{
	//Scan through the ModuleInstanceRegistry and find the pointer that matches
	// this.  DO NOT COUNT ANY NULLs!!!!  These will go away when we reload a
	// QuickSave!!
	long actualCount = 0;
	bool foundBrain = false;

	for (long i=0;i<NumModuleInstances;i++)
	{
		if (this == ModuleInstanceRegistry[i])
		{
			foundBrain = true;
			break;
		}

		if (ModuleInstanceRegistry[i])
			actualCount++;
	}

	if (!foundBrain)
			ABL_Fatal(0,"Could not find this Brain in the ModuleInstanceRegistry");

	return actualCount;
}

//---------------------------------------------------------------------------

// A2: the debugger managers are created in one place. init() and read() both call this, so the creation order
// (watch manager, then break points) and the ownership (the unique_ptr members) cannot drift apart.
void ABLModule::initDebuggerManagers (void) {

	if (!debugger)
		return;

	watchManager = std::make_unique<WatchManager>();
	if (!watchManager)
		ABL_Fatal(0, " Unable to AblStackHeap->malloc WatchManager ");
	long result = watchManager->init(MaxWatchesPerModule);
	if (result != ABL_NO_ERR)
		ABL_Fatal(0, " Unable to AblStackHeap->malloc WatchManager ");
	breakPointManager = std::make_unique<BreakPointManager>();
	if (!breakPointManager)
		ABL_Fatal(0, " Unable to AblStackHeap->malloc BreakPointManager ");
	result = breakPointManager->init(MaxBreakPointsPerModule);
	if (result != ABL_NO_ERR)
		ABL_Fatal(0, " Unable to AblStackHeap->malloc BreakPointManager ");
}

//---------------------------------------------------------------------------

long ABLModule::init (int moduleHandle) {

	if (moduleHandle == -1) {
		//----------
		// Clean up!
		return(-1);
	}

	id = NumModules++;
	handle = moduleHandle;
	staticData = nullptr;
	long numStatics = ModuleRegistry[handle].numStaticVars;
	if (numStatics) {
		staticData = static_cast<StackItemPtr>(ABL_alloc(ABLPool::Stack, sizeof(StackItem) * numStatics));
		if (!staticData) {
			char err[255];
			mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc staticData [Module {:d}]", mc2::fmt::as_i32(id));
			ABL_Fatal(0, err);
			throw std::bad_alloc();
		}
		std::memset(staticData, 0, sizeof(StackItem) * numStatics);
		long* sizeList = ModuleRegistry[handle].sizeStaticVars;
		for (long i = 0; i < numStatics; i++)
			if (sizeList[i] > 0) {
				staticData[i].address = static_cast<char*>(ABL_alloc(ABLPool::Stack, sizeList[i]));
				if (!staticData[i].address) {
					char err[255];
					mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc staticData address [Module {:d}]", mc2::fmt::as_i32(id));
					ABL_Fatal(0, err);
					throw std::bad_alloc();
				}
				}
			else
				staticData[i].integer = 0;
	}

	if (ModuleRegistry[handle].numOrderCalls) {
		long numLongs = 1 + ModuleRegistry[handle].numOrderCalls / 32;
		orderCallFlags = static_cast<unsigned long*>(ABL_alloc(ABLPool::Stack, sizeof(unsigned long) * numLongs));
		if (!orderCallFlags) {
			char err[255];
			mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc orderCallFlags [Module {:d}]", mc2::fmt::as_i32(id));
			ABL_Fatal(0, err);
		}
		for (long i = 0; i < numLongs; i++)
			orderCallFlags[i] = 0;
	}
	ModuleRegistry[handle].numInstances++;
	initCalled = false;

	//------------------------------------------------------
	// This Active Module is now on the instance registry...
	ModuleInstanceRegistry[NumModuleInstances++] = this;

	initDebuggerManagers();

	if (ModuleRegistry[handle].moduleIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM) {
		//--------------------------------
		// Always starts in START state...
		SymTableNodePtr startState = searchSymTable("start", ModuleRegistry[handle].moduleIdPtr->defn.info.routine.localSymTable);
		if (!startState) {
			char err[255];
			mc2::fmt::to_buf(err, "ABL: FSM has no Start state [{}]", mc2::fmt::safe(CurModule->getName()));
			ABL_Fatal(0, err);
		}
		prevState = nullptr;
		state = startState;
	}

	//--------------------
	// Can this ever fail?
	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void ABLModule::write (ABLFile* moduleFile, bool values) {

	long numStatics = ModuleRegistry[handle].numStaticVars;
	long* sizeList = ModuleRegistry[handle].sizeStaticVars;
	if (values) {
		mc2::save::AblModuleRecord record{name, handle,
			prevState ? prevState->name : "", state ? state->name : "", initCalled,
			static_cast<uint32_t>(numStatics), {}};
		for (long w = 0; w < orderFlagWords(this); ++w)
			record.orderFlags.push_back(static_cast<uint32_t>(orderCallFlags[w]));
		writeAblRecord(moduleFile, std::move(record), mc2::save::tagAblModule);
		for (long i = 0; i < numStatics; ++i)
			writeAblRecord(moduleFile, mc2::save::captureAblVariable(staticData[i], sizeList[i]), mc2::save::tagAblVariable);
		return;
	}
	moduleFile->writeString(name);
	moduleFile->writeByte('\0');
	moduleFile->writeInt(handle);
	if (prevState == nullptr)
		moduleFile->writeString("NULLPrevState");
	else
		moduleFile->writeString(prevState->name);
	moduleFile->writeByte('\0');
	if (state == nullptr)
		moduleFile->writeString("NULLState");
	else
		moduleFile->writeString(state->name);
	moduleFile->writeByte('\0');
	moduleFile->writeInt(initCalled ? 1 : 0);
	for (long i = 0; i < numStatics; i++) {
		if (sizeList[i] > 0)
			moduleFile->write(reinterpret_cast<unsigned char*>(staticData[i].address), sizeList[i]);
		else
			writeScalarSlot(moduleFile, &staticData[i]);
	}
}

//---------------------------------------------------------------------------

void ABLModule::read (ABLFile* moduleFile, bool values) {

	// Existing modules retain their allocations while their saved data is restored.
	bool fresh = (id == -1);
	mc2::save::AblModuleRecord record;
	if (values) {
		record = readAblRecord<mc2::save::AblModuleRecord>(moduleFile, mc2::save::tagAblModule);
		if (record.handle >= NumModulesRegistered || (!fresh && record.handle != handle))
			ABL_Fatal(0, "ABL: module handle differs from compiled registry");
		std::memcpy(name, record.name.c_str(), record.name.size() + 1);
		if (fresh) {
			id = NumModules++;
			handle = record.handle;
			staticData = nullptr;
		}
		prevState = record.previousState.empty() ? nullptr : findState(record.previousState.c_str());
		state = record.state.empty() ? nullptr : findState(record.state.c_str());
		if ((!record.previousState.empty() && !prevState) || (!record.state.empty() && !state))
			ABL_Fatal(0, "ABL: saved state missing from compiled module");
	}
	else if (fresh) {
		id = NumModules++;
		if (moduleFile->readString(reinterpret_cast<unsigned char*>(name), sizeof(name)) < 0)
			return;
		handle = moduleFile->readInt();
		staticData = nullptr;
		}
	else {
		char tempName[1024];
		if (moduleFile->readString(reinterpret_cast<unsigned char*>(tempName), sizeof(tempName)) < 0)
			return;
		long ignore = moduleFile->readInt();
        (void)ignore;
	}

	if (!values) {
	char stateName[256];
	memset(stateName,0,256);
	if (moduleFile->readString(reinterpret_cast<unsigned char*>(stateName), sizeof(stateName)) < 0)
		return;
	prevState = nullptr;
	if (strcmp(stateName, "NULLPrevState"))
		prevState = findState(stateName);
	
	memset(stateName,0,256);
	if (moduleFile->readString(reinterpret_cast<unsigned char*>(stateName), sizeof(stateName)) < 0)
		return;
	state = nullptr;
	if (strcmp(stateName, "NULLState"))
		state = findState(stateName);
	}

	bool savedInitCalled = values ? record.initCalled : (moduleFile->readInt() == 1);

	long numStatics = ModuleRegistry[handle].numStaticVars;
	if (values && record.statics != numStatics)
		ABL_Fatal(0, "ABL: static count differs from compiled module");
	if (numStatics) {
		if (fresh) {		
			staticData = static_cast<StackItemPtr>(ABL_alloc(ABLPool::Stack, sizeof(StackItem) * numStatics));
			if (!staticData) {
				char err[255];
				mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc staticData [Module {:d}]", mc2::fmt::as_i32(id));
				ABL_Fatal(0, err);
				throw std::bad_alloc();
			}
			std::memset(staticData, 0, sizeof(StackItem) * numStatics);
		}
		long* sizeList = ModuleRegistry[handle].sizeStaticVars;
		for (long i = 0; i < numStatics; i++)
			if (sizeList[i] > 0) {
				if (fresh) {
					staticData[i].address = static_cast<char*>(ABL_alloc(ABLPool::Stack, sizeList[i]));
					if (!staticData[i].address) {
						char err[255];
						mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc staticData address [Module {:d}]", mc2::fmt::as_i32(id));
						ABL_Fatal(0, err);
						throw std::bad_alloc();
					}
				}
				if (values) {
					readAblVariable(moduleFile, staticData[i], sizeList[i]);
					continue;
				}
				long result = moduleFile->read(reinterpret_cast<unsigned char*>(staticData[i].address), sizeList[i]);
				if (!result) {
					char err[255];
					mc2::fmt::to_buf(err, "ABL: Unable to read staticData.address [Module {:d}]", mc2::fmt::as_i32(id));
					ABL_Fatal(0, err);
				}
				}
			else {
				if (values) {
					readAblVariable(moduleFile, staticData[i], 0);
					continue;
				}
				staticData[i].integer = 0;
				long result = moduleFile->read(reinterpret_cast<unsigned char*>(&staticData[i]), sizeof(StackItem));
				clearScalarSlotPointerArm(&staticData[i]);
				if (!result) {
					char err[255];
					mc2::fmt::to_buf(err, "ABL: Unable to read staticData [Module {:d}]", mc2::fmt::as_i32(id));
					ABL_Fatal(0, err);
				}
			}
	}

	if (ModuleRegistry[handle].numOrderCalls) {
		long numLongs = 1 + ModuleRegistry[handle].numOrderCalls / 32;
		if (!orderCallFlags) // an instance read in place keeps its block
			orderCallFlags = static_cast<unsigned long*>(ABL_alloc(ABLPool::Stack, sizeof(unsigned long) * numLongs));
		if (!orderCallFlags) {
			char err[255];
			mc2::fmt::to_buf(err, "ABLModule.read: Unable to AblStackHeap->malloc orderCallFlags [Module {:d}]", mc2::fmt::as_i32(id));
			ABL_Fatal(0, err);
		}
		for (long i = 0; i < numLongs; i++)
			orderCallFlags[i] = 0;
	}
	
	if (values) {
		initCalled = savedInitCalled; // Libraries already exist after recompilation.
		if (record.orderFlags.size() != orderFlagWords(this))
			ABL_Fatal(0, "ABL: order count differs from compiled module");
		for (size_t w = 0; w < record.orderFlags.size(); ++w)
			orderCallFlags[w] = record.orderFlags[w];
	}
	if (fresh) {
		ModuleRegistry[handle].numInstances++;
		initCalled = savedInitCalled;

		ModuleInstanceRegistry[NumModuleInstances++] = this;

		initDebuggerManagers();
	}
}

//---------------------------------------------------------------------------

ABLModule::ABLModule (void) {

	init();
}

//---------------------------------------------------------------------------

ABLModule::~ABLModule (void) {

	destroy();
}

//---------------------------------------------------------------------------

char* ABLModule::getFileName (void) {

	return(ModuleRegistry[handle].fileName);
}

//---------------------------------------------------------------------------

void ABLModule::setName (mc2::czstring _name) {

	strncpy(name, _name, MAX_ABLMODULE_NAME);
	name[MAX_ABLMODULE_NAME-1] = '\0';
}

//---------------------------------------------------------------------------

bool ABLModule::isLibrary (void) {

	return(ModuleRegistry[handle].moduleIdPtr->library != nullptr);
}

//---------------------------------------------------------------------------

void ABLModule::resetOrderCallFlags (void) {

	if (ModuleRegistry[handle].numOrderCalls == 0)
		return;

	long numLongs = 1 + ModuleRegistry[handle].numOrderCalls / 32;
	for (long i = 0; i < numLongs; i++)
		orderCallFlags[i] = 0;
}

//---------------------------------------------------------------------------

void ABLModule::setOrderCallFlag (unsigned char dword, unsigned char bit) {

	orderCallFlags[dword] |= (1 << bit);
}

//---------------------------------------------------------------------------

void ABLModule::clearOrderCallFlag(unsigned char dword, unsigned char bit) {

	orderCallFlags[dword] &= ((1 << bit) ^ 0xFFFFFFFF);
}

//---------------------------------------------------------------------------

int ABLModule::getPrevStateHandle (void) {

	if (!prevState)
		return(0);

	for (long i = 0; i < ModuleRegistry[handle].numStateHandles; i++)
		if (strcmp(prevState->name, ModuleRegistry[handle].stateHandles[i].name) == 0)
			return(i);

	return(0);
}

//---------------------------------------------------------------------------

int ABLModule::getStateHandle (void) {

	if (!state)
		return(0);

	for (long i = 0; i < ModuleRegistry[handle].numStateHandles; i++)
		if (strcmp(state->name, ModuleRegistry[handle].stateHandles[i].name) == 0)
			return(i);

	return(0);
}

//---------------------------------------------------------------------------

// OMNITECH-ABL-EXECEXPRESSION-1 helpers: gate + one-shot-per-module skip report.
// DEFAULT-OFF. Skipping an entire compile-errored module is TOO BLUNT: most modules with
// STOPSYNTAX errors still RUN correctly (the errored bytecode executes fine) — e.g.
// DarkRain's torrinescort.abl / torrin_script.abl (4 errors each) drive working AI attack
// orders. Skipping them broke the AI (mechs patrol but never engage). The actual crash
// (MCO: stack underflow -> READ@0x4 in execExpression) is now prevented by a per-site
// underflow guard in ablxexpr.cpp, so the module no longer needs skipping. This gate stays
// OFF by default (opt-in MC2_ABL_SKIP_ERRORED_MODULES=1) as a last-resort escape hatch only.
// ASAN-3-ABL-COMPILE-FATAL-1: no longer static — ABLi_preProcess (ablrtn.cpp)
// consults the same gate so the load-time compile-error fatal defers to the
// opt-in skip-errored-modules playability mode (decl in abl.h).
bool ablSkipErroredModulesEnabled (void) {
	static const bool s_on = mc2::gate::get<"MC2_ABL_SKIP_ERRORED_MODULES">();
	return s_on;
}
static void ablReportSkippedErroredModule (long handle, mc2::czstring fileName, long errs) {
	static std::set<long> s_reported;
	if (const auto [it, inserted] = s_reported.insert(handle); inserted) {
		mc2::fmt::print_out("[ABL-SKIP] handle={:d} file={} compileErrors={:d} -> execution skipped (MC2_ABL_SKIP_ERRORED_MODULES)\n",
			mc2::fmt::as_long(handle), fileName ? fileName : "?", mc2::fmt::as_long(errs));
		fflush(stdout);
	}
}

long ABLModule::execute (ABLParamPtr paramList) {

	// Errored modules may contain malformed bytecode that underflows the VM stack.
	if (ablSkipErroredModulesEnabled() && ModuleRegistry[handle].compileErrorCount > 0) {
		ablReportSkippedErroredModule(handle, ModuleRegistry[handle].fileName, ModuleRegistry[handle].compileErrorCount);
		return 0;
	}

	CurModule = this;
	if (debugger)
		debugger->setModule(this);

	SymTableNodePtr moduleIdPtr = ModuleRegistry[handle].moduleIdPtr;
	if (moduleIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM)
		CurFSM = this;
	else
		CurFSM = nullptr;
	NumStateTransitions = 0;

	StaticDataPtr = staticData;
	OrderCompletionFlags = orderCallFlags;

	CurModuleIdPtr = nullptr;
	CurRoutineIdPtr = nullptr;
	FileNumber = -1;
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
					if (!dest) {
						char err[255];
						mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc array parameter [Module {:d}]", mc2::fmt::as_i32(id));
						ABL_Fatal(0, err);
					}
					char* src = tos->address;
					char* savePtr = dest;
					memcpy(dest, src, size);
					tos->address = savePtr;
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

	CurModuleHandle = handle;

	// No init function in FSM. Put all init stuff into the start state...
	CallModuleInit = !initCalled;
	initCalled = true;

	NewStateSet = false;
	::execute(moduleIdPtr);

	memcpy(&returnVal, &returnValue, sizeof(StackItem));

	return(execStatementCount);	
}

//---------------------------------------------------------------------------

long ABLModule::execute (ABLParamPtr moduleParamList, SymTableNodePtr functionIdPtr) {

	// OMNITECH-ABL-EXECEXPRESSION-RECON-1: skip errored-compile modules (see overload above).
	if (ablSkipErroredModulesEnabled() && ModuleRegistry[handle].compileErrorCount > 0) {
		ablReportSkippedErroredModule(handle, ModuleRegistry[handle].fileName, ModuleRegistry[handle].compileErrorCount);
		return 0;
	}

	CurModule = this;
	if (debugger)
		debugger->setModule(this);

	//--------------------------
	// Execute the ABL module...
	SymTableNodePtr moduleIdPtr = ModuleRegistry[handle].moduleIdPtr;
	if (moduleIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM)
		CurFSM = this;
	else
		CurFSM = nullptr;
	NumStateTransitions = 0;


	//--------------------------------------------
	// Point to this module's static data space...
	StaticDataPtr = staticData;
	OrderCompletionFlags = orderCallFlags;

	//---------------------------------
	// Init some important variables...
	CurModuleIdPtr = nullptr;
	CurRoutineIdPtr = nullptr;
	FileNumber = -1;
	errorCount = 0;
	execStatementCount = 0;
	NumExecutions++;
	NewStateSet = false;

	//------------------
	// Init the stack...
	stackFrameBasePtr = tos = (stack + eternalOffset);

	//---------------------------------------
	// Initialize the module's stack frame...
	level = 1;
	CallStackLevel = 0;
	stackFrameBasePtr = tos + 1;
	
	//-------------------------
	// Function return value...
	pushInteger(0);
	//---------------
	// Static Link...
	pushAddress(nullptr);
	//----------------
	// Dynamic Link...
	pushAddress(nullptr);
	//------------------
	// Return Address...
	pushAddress(nullptr);

	//initDebugger();

	//----------
	// Run it...

	if (moduleParamList) {
		//------------------------------------------------------------------------------
		// NOTE: Currently, parameter passing of arrays is not functioning. This MUST be
		// done...
		long curParam = 0;
		for (SymTableNodePtr formalIdPtr = moduleIdPtr->defn.info.routine.params;
			 formalIdPtr != nullptr;
			 formalIdPtr = formalIdPtr->next) {

			TypePtr formalTypePtr = formalIdPtr->typePtr;

			if (formalIdPtr->defn.key == DFN_VALPARAM) {

				if (formalTypePtr == RealTypePtr) {
					if (moduleParamList[curParam].type == ABL_PARAM_INTEGER) {
						//---------------------------------------------
						// Real formal parameter, but integer actual...
						pushReal(static_cast<float>((moduleParamList[curParam].integer)));
						}
					else if (moduleParamList[curParam].type == ABL_PARAM_REAL)
						pushReal(moduleParamList[curParam].real);
					}
				else if (formalTypePtr == IntegerTypePtr) {
					if (moduleParamList[curParam].type== ABL_PARAM_INTEGER)
						pushInteger(moduleParamList[curParam].integer);
					else
						return(0);
				}

				//----------------------------------------------------------
				// Formal parameter is an array or record, so make a copy...
				if (formalTypePtr->form == FRM_ARRAY/* || (formalTypePtr->form == FRM_RECORD)*/) {
					//------------------------------------------------------------------------------
					// The following is a little inefficient, but is kept this way to keep it clear.
					// Once it's verified to work, optimize...
					long size = formalTypePtr->size;
					char* dest = static_cast<char*>(ABL_alloc(ABLPool::Stack, static_cast<size_t>(size)));
					if (!dest) {
						char err[255];
						mc2::fmt::to_buf(err, "ABL: Unable to AblStackHeap->malloc array parameter [Module {:d}]", mc2::fmt::as_i32(id));
						ABL_Fatal(0, err);
					}
					char* src = tos->address;
					char* savePtr = dest;
					memcpy(dest, src, size);
					tos->address = savePtr;
				}
				}
			else {
				//-------------------------------
				// pass by reference parameter...
				if (formalTypePtr == RealTypePtr)
					pushAddress(reinterpret_cast<Address>(&(moduleParamList[curParam].real)));
				else if (formalTypePtr == IntegerTypePtr)
					pushAddress(reinterpret_cast<Address>(&(moduleParamList[curParam].integer)));
				else
					return(0);
			}
			curParam++;
		}
	}

	CurModuleHandle = handle;

	CallModuleInit = !initCalled;
	initCalled = true;

	::executeChild(moduleIdPtr, functionIdPtr);

	memcpy(&returnVal, &returnValue, sizeof(StackItem));

	//-----------
	// Summary...
	return(execStatementCount);	
}

//---------------------------------------------------------------------------

SymTableNodePtr ABLModule::findSymbol (mc2::czstring symbolName, SymTableNodePtr curFunction, bool searchLibraries) {

	if (curFunction) {
        char* sn = strdup(symbolName);
		SymTableNodePtr symbol = searchSymTable(S_strlwr(sn), curFunction->defn.info.routine.localSymTable);
        free(sn);
		if (symbol)
			return(symbol);
	}

    char* sn = strdup(symbolName);
    sn = S_strlwr(sn);

	SymTableNodePtr symbol = searchSymTable(sn, ModuleRegistry[handle].moduleIdPtr->defn.info.routine.localSymTable);

	if (!symbol && searchLibraries) {
		for (long i = 0; i < ModuleRegistry[handle].numLibrariesUsed; i++) {
			symbol = searchSymTable(sn, ModuleRegistry[ModuleRegistry[handle].librariesUsed[i]->handle].moduleIdPtr->defn.info.routine.localSymTable);
			if (symbol)
				break;
		}
	}

    free(sn);

	return(symbol);
}

//---------------------------------------------------------------------------

SymTableNodePtr ABLModule::findFunction (mc2::czstring functionName, bool searchLibraries) {

	SymTableNodePtr symbol = searchSymTableForFunction(functionName, ModuleRegistry[handle].moduleIdPtr->defn.info.routine.localSymTable);

	if (!symbol && searchLibraries) 
    {
		for (long i = 0; i < ModuleRegistry[handle].numLibrariesUsed; i++) 
        {
            char temp[1024];
            memset(temp, 0, 1024 );
            strncpy( temp, functionName, (strlen(functionName) > 1020) ? 1020 : strlen(functionName) );

			symbol = searchSymTable(S_strlwr(temp), ModuleRegistry[ModuleRegistry[handle].librariesUsed[i]->handle].moduleIdPtr->defn.info.routine.localSymTable);

			if (symbol)
				break;
		}
	}

	return(symbol);
}

//---------------------------------------------------------------------------

SymTableNodePtr ABLModule::findState (mc2::czstring stateName) {

	SymTableNodePtr symbol = searchSymTableForState(stateName, ModuleRegistry[handle].moduleIdPtr->defn.info.routine.localSymTable);
	return(symbol);
}

//---------------------------------------------------------------------------

int ABLModule::findStateHandle (mc2::czstring stateName) {

	for (long i = 1; i < ModuleRegistry[handle].numStateHandles; i++)
		if (strcmp(stateName, ModuleRegistry[handle].stateHandles[i].name) == 0)
			return(i);
	return(0);
}

//---------------------------------------------------------------------------

int ABLModule::setStaticInteger (char* name, int value) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(1);
	
	if (symbol->typePtr != IntegerTypePtr)
		return(2);

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(3);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	*(reinterpret_cast<int*>(dataPtr)) = value;
	return(0);
}

//---------------------------------------------------------------------------

int ABLModule::getStaticInteger (char* name) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(0xFFFFFFFF);
	
	if (symbol->typePtr != IntegerTypePtr)
		return(0xFFFFFFFF);

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(0xFFFFFFFF);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	return(*(reinterpret_cast<int*>(dataPtr)));
}

//---------------------------------------------------------------------------

int ABLModule::setStaticReal (char* name, float value) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(1);
	
	if (symbol->typePtr != RealTypePtr)
		return(2);

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(3);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	dataPtr->real = value;
	return(0);
}

//---------------------------------------------------------------------------

float ABLModule::getStaticReal (char* name) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(-999999.0);
	
	if (symbol->typePtr != RealTypePtr)
    {
        assert(0 && "ABLModule::getStaticReal");
		return(-999999.0);
    }

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
    {
        assert(0 && "ABLModule::getStaticReal");
		return(-999999.0);
    }

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	return(dataPtr->real);
}

//---------------------------------------------------------------------------

int ABLModule::setStaticIntegerArray (char* name, int numValues, int* values) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(1);
	
	//--------------------------------------------------------------------------
	// NOTE: This function is not dummy-proof. Essentially, this routine copies
	// the values data into the array's data space WITHOUT checking to make sure
	// the array really is an array (single or mult-dimensional) of reals. User
	// beware!

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(3);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	memcpy(dataPtr->address, values, sizeof(values[0]) * numValues);
	
	return(0);
}

//---------------------------------------------------------------------------

int ABLModule::getStaticIntegerArray (char* name, int numValues, int* values) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(0);
	
	//--------------------------------------------------------------------------
	// NOTE: This function is not dummy-proof. Essentially, this routine copies
	// the values data into the array's data space WITHOUT checking to make sure
	// the array really is an array (single or mult-dimensional) of reals. User
	// beware!

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(0);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	memcpy(values, dataPtr->address, sizeof(values[0]) * numValues);
	return(1);
}

//---------------------------------------------------------------------------

int ABLModule::setStaticRealArray (char* name, int numValues, float* values) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(1);

	//--------------------------------------------------------------------------
	// NOTE: This function is not dummy-proof. Essentially, this routine copies
	// the values data into the array's data space WITHOUT checking to make sure
	// the array really is an array (single or mult-dimensional) of reals. User
	// beware!

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(3);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	memcpy(dataPtr->address, values, sizeof(values[0]) * numValues);

	return(0);
}

//---------------------------------------------------------------------------

int ABLModule::getStaticRealArray (char* name, int numValues, float* values) {

	SymTableNodePtr symbol = findSymbol(name);
	if (!symbol)
		return(0);

	//--------------------------------------------------------------------------
	// NOTE: This function is not dummy-proof. Essentially, this routine copies
	// the values data into the array's data space WITHOUT checking to make sure
	// the array really is an array (single or mult-dimensional) of reals. User
	// beware!

	if (symbol->defn.info.data.varType != VAR_TYPE_STATIC)
		return(0);

	StackItemPtr dataPtr = staticData + symbol->defn.info.data.offset;
	memcpy(values, dataPtr->address, sizeof(values[0]) * numValues);

	return(1);
}

//---------------------------------------------------------------------------

char* ABLModule::getSourceFile (int fileNumber) {

	return(ModuleRegistry[handle].sourceFiles[fileNumber]);
}

//---------------------------------------------------------------------------

char* ABLModule::getSourceDirectory (int fileNumber, char* directory) {

	char* fileName = ModuleRegistry[handle].sourceFiles[fileNumber];
	const size_t lastSeparator = std::string_view(fileName).find_last_of("/\\");
	if (lastSeparator == std::string_view::npos)
		return(nullptr);
	strcpy(directory, fileName);
	directory[lastSeparator + 1] = '\0';
	return(directory);
}

//---------------------------------------------------------------------------

void buildRoutineList (SymTableNodePtr curSymbol, ModuleInfo* moduleInfo) {

	if (curSymbol) {
		buildRoutineList(curSymbol->left, moduleInfo);
		if (curSymbol->defn.key == DFN_FUNCTION) {
			if (moduleInfo->numRoutines < 1024) {
				strcpy(moduleInfo->routineInfo[moduleInfo->numRoutines].name, curSymbol->name);
				moduleInfo->routineInfo[moduleInfo->numRoutines].codeSegmentSize = curSymbol->defn.info.routine.codeSegmentSize;
				moduleInfo->numRoutines++;
			}
		}
		buildRoutineList(curSymbol->right, moduleInfo);
	}
}

//---------------------------------------------------------------------------

void ABLModule::getInfo (ModuleInfo* moduleInfo) {

	strcpy(moduleInfo->name, name);
	strcpy(moduleInfo->fileName, ModuleRegistry[handle].fileName);

	moduleInfo->numRoutines = 0;
	buildRoutineList(ModuleRegistry[handle].moduleIdPtr->defn.info.routine.localSymTable, moduleInfo);
	for (long i = 0; i < moduleInfo->numRoutines; i++)
		moduleInfo->totalCodeSegmentSize += moduleInfo->routineInfo[i].codeSegmentSize;

	moduleInfo->numStaticVars = ModuleRegistry[handle].numStaticVars;
	moduleInfo->totalSizeStaticVars = ModuleRegistry[handle].totalSizeStaticVars;

	long largest = 0;
	for (int i = 0; i < moduleInfo->numStaticVars; i++) {
		if (ModuleRegistry[handle].sizeStaticVars[i] > ModuleRegistry[handle].sizeStaticVars[largest])
			largest = i;
	}
	moduleInfo->largestStaticVar.size = 0;
	if (ModuleRegistry[handle].sizeStaticVars)
		moduleInfo->largestStaticVar.size = ModuleRegistry[handle].sizeStaticVars[largest];
	moduleInfo->largestStaticVar.name[0] = '\0';
}

//---------------------------------------------------------------------------

void ABLModule::destroy (void) {

	if ((id > -1) && ModuleInstanceRegistry) {
		//-----------------------------------------------
		// It's on the active registry, so pull it off...
		for (long i = 0; i < NumModuleInstances; i++)
			if (ModuleInstanceRegistry[i] == this) {
				ModuleInstanceRegistry[i] = ModuleInstanceRegistry[NumModuleInstances - 1];
				ModuleInstanceRegistry[NumModuleInstances - 1] = nullptr;
				NumModuleInstances--;
				NumModules--;
				break;
			}
	}

	watchManager.reset();
	breakPointManager.reset();

	// The instance's own blocks go back now, not at ABLi_close (BUGS row 78): the order flags and the sized statics. After
	// ABLi_close the registry is gone and the first free reports the closed environment.
	if (orderCallFlags) {
		ABL_free(ABLPool::Stack, orderCallFlags);
		orderCallFlags = nullptr;
	}

	if (staticData) {
		if (ModuleRegistry) {
			const long* sizeList = ModuleRegistry[handle].sizeStaticVars;
			for (long i = 0; i < ModuleRegistry[handle].numStaticVars; i++)
				if (sizeList[i] > 0)
					ABL_free(ABLPool::Stack, staticData[i].address);
		}
		ABL_free(ABLPool::Stack, staticData);
		staticData = nullptr;
	}
}

//***************************************************************************
// MISC routines
//***************************************************************************

void ABLi_saveEnvironment (ABLFile* ablFile, bool values) {

	if (values) {
		mc2::save::AblEnvironmentRecord record{static_cast<uint32_t>(numLibrariesLoaded),
			static_cast<uint32_t>(NumModules), static_cast<uint32_t>(eternalOffset), {}};
		for (long i = 0; i < NumModulesRegistered; ++i)
			record.files.emplace_back(ModuleRegistry[i].fileName);
		writeAblRecord(ablFile, std::move(record), mc2::save::tagAblEnvironment);
	}
	else {
	ablFile->writeInt(numLibrariesLoaded);
	ablFile->writeInt(NumModulesRegistered);
	ablFile->writeInt(NumModules);
	for (long i = 0; i < NumModulesRegistered; i++) {
		ablFile->writeString(ModuleRegistry[i].fileName);
		ablFile->writeByte('\0');
	}
	ablFile->writeInt(ABL_ENV_MARK);
	}
	for (int i = 0; i < eternalOffset; i++) {
		StackItemPtr dataPtr = reinterpret_cast<StackItemPtr>(stack) + i;
		if (values)
			writeAblRecord(ablFile, mc2::save::captureAblVariable(*dataPtr, EternalVariablesSizes[i]), mc2::save::tagAblVariable);
		else if (EternalVariablesSizes[i] > 0)
			ablFile->write(reinterpret_cast<unsigned char*>(dataPtr->address), EternalVariablesSizes[i]);
		else
			writeScalarSlot(ablFile, dataPtr);
	}
	for (int i = 0; i < NumModules; i++)
	{
		if (ModuleInstanceRegistry[i])
			ModuleInstanceRegistry[i]->write(ablFile, values);
	}
	if (values) return; // Order flags are explicit module fields in this format.
	bool anyCompleted = false;
	for (int i = 0; i < NumModules && !anyCompleted; i++)
		for (long w = 0; w < orderFlagWords(ModuleInstanceRegistry[i]); w++)
			anyCompleted = anyCompleted || ModuleInstanceRegistry[i]->getOrderCallFlags()[w] != 0;
	if (anyCompleted) {
		ablFile->writeInt(ABL_ORDER_FLAGS_MARK);
		for (int i = 0; i < NumModules; i++)
			for (long w = 0; w < orderFlagWords(ModuleInstanceRegistry[i]); w++)
				ablFile->writeInt(static_cast<int32_t>(ModuleInstanceRegistry[i]->getOrderCallFlags()[w]));
	}
}

//---------------------------------------------------------------------------

void ABLi_loadEnvironment (ABLFile* ablFile, bool malloc, bool values) {

	mc2::save::AblEnvironmentRecord record;
	if (values) {
		record = readAblRecord<mc2::save::AblEnvironmentRecord>(ablFile, mc2::save::tagAblEnvironment);
		if (record.libraries > static_cast<uint32_t>(MaxLibraries) ||
			record.files.size() > static_cast<size_t>(MaxModules) || record.modules > static_cast<uint32_t>(MaxModules))
			ABL_Fatal(0, "ABL: environment exceeds registry capacity");
	}
	long numLibs = values ? static_cast<long>(record.libraries) : ablFile->readInt();
	long numModsRegistered = values ? static_cast<long>(record.files.size()) : ablFile->readInt();
	long numMods = values ? static_cast<long>(record.modules) : ablFile->readInt();

	for (int i = 0; i < numLibs; i++) {
		unsigned char fileName[1024];
		long result = values ? static_cast<long>(record.files[i].size()) : ablFile->readString(fileName, sizeof(fileName));
		if (values) std::memcpy(fileName, record.files[i].c_str(), record.files[i].size() + 1);
		if (result < 0)
			return;
		if (!result) {
			char err[255];
			mc2::fmt::to_buf(err, "ABLi_loadEnvironment: Unable to read filename [Module {:d}]", mc2::fmt::as_i32(i));
			ABL_Fatal(0, err);
		}
		if (malloc) {
			long numErrors, numLinesProcessed;
			ABLModulePtr library = ABLi_loadLibrary(reinterpret_cast<const char*>(fileName), &numErrors, &numLinesProcessed, nullptr, false, false);
			if (!library) {
				char err[255];
				mc2::fmt::to_buf(err, "ABLi_loadEnvironment: Unable to load library [Module {:d}]", mc2::fmt::as_i32(i));
				ABL_Fatal(0, err);
			}
		}
	}

	for (int i = 0; i < (numModsRegistered - numLibs); i++) {
		unsigned char fileName[1024];
		const auto fileIndex = numLibs + i;
		long result = values ? static_cast<long>(record.files[fileIndex].size()) : ablFile->readString(fileName, sizeof(fileName));
		if (values) std::memcpy(fileName, record.files[fileIndex].c_str(), record.files[fileIndex].size() + 1);
		if (result < 0)
			return;
		if (!result) {
			char err[255];
			mc2::fmt::to_buf(err, "ABLi_loadEnvironment: Unable to read filename [Module {:d}]", mc2::fmt::as_i32(i));
			ABL_Fatal(0, err);
		}
		long numErrors, numLinesProcessed;
		if (malloc) {
			int handle = ABLi_preProcess(reinterpret_cast<const char*>(fileName), &numErrors, &numLinesProcessed);
			if (handle < 0) {
				char err[255];
				mc2::fmt::to_buf(err, "ABLi_loadEnvironment: Unable to preprocess [Module {:d}]", mc2::fmt::as_i32(i));
				ABL_Fatal(0, err);
			}
		}
	}

	if (values && record.eternals != eternalOffset)
		ABL_Fatal(0, "ABL: eternal count differs from compiled declarations");
	int mark = values ? ABL_ENV_MARK : ablFile->readInt();
    if(mark != ABL_ENV_MARK) {
        char err[255];
        mc2::fmt::to_buf(err, "ABLi_loadEnvironment: mark vlue is not as expected ({:d}), actual: {:d}\n", mc2::fmt::as_i32(ABL_ENV_MARK), mc2::fmt::as_i32(mark));
        ABL_Fatal(0, err);
    }

	for (int i = 0; i < eternalOffset; i++) {
		StackItemPtr dataPtr = reinterpret_cast<StackItemPtr>(stack) + i;
		if (values)
			readAblVariable(ablFile, *dataPtr, EternalVariablesSizes[i]);
		else if (EternalVariablesSizes[i] > 0)
			ablFile->read(reinterpret_cast<unsigned char*>(dataPtr->address), EternalVariablesSizes[i]);
		else
		{
			ablFile->read(reinterpret_cast<unsigned char*>(dataPtr), sizeof(StackItem));
			clearScalarSlotPointerArm(dataPtr);
		}
	}
	std::vector<ABLModulePtr> readOrder;
	for (int i = 0; i < numLibs; i++) {
		ABLModulePtr library = LibraryInstanceRegistry[i];
		library->read(ablFile, values);
		readOrder.push_back(library);
	}

	for (int i = 0; i < (numMods - numLibs); i++) {
		ABLModulePtr module = nullptr;
		if (malloc)
			module = new ABLModule;
		else
			module = ModuleInstanceRegistry[numLibs + i];
		module->read(ablFile, values);
		readOrder.push_back(module);
	}
	if (values) {
		if (ablFile->remaining() != 0) ABL_Fatal(0, "ABL: trailing value records");
		return;
	}

	// The trailing order-completion section (absent in a save with no completed order and in a legacy save).
	long sectionWords = 0;
	for (ABLModulePtr module : readOrder)
		sectionWords += orderFlagWords(module);
	const long left = ablFile->remaining();
	if (left == 0 || (left < 0 && ablFile->eof()))
		return;
	const long sectionBytes = static_cast<long>(sizeof(int32_t)) * (1 + sectionWords);
	std::vector<int32_t> section(static_cast<size_t>(1 + sectionWords));
	if (left != sectionBytes || ablFile->read(reinterpret_cast<unsigned char*>(section.data()), sectionBytes) != sectionBytes ||
		section[0] != ABL_ORDER_FLAGS_MARK) {
		MC2_LOG(Sim, Error, "[ABL_ORDER_FLAGS_BAD] save tail of {} bytes is not the {}-byte order-flags section; every order flag left clear",
			left, sectionBytes);
		return;
	}
	size_t next = 1;
	for (ABLModulePtr module : readOrder)
		for (long w = 0; w < orderFlagWords(module); w++)
			module->getOrderCallFlags()[w] = static_cast<unsigned long>(static_cast<uint32_t>(section[next++]));
}

//***************************************************************************


