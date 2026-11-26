//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ENVIRON.H
//
//***************************************************************************

#ifndef ABLENV_H
#define ABLENV_H

#include "mc2_czstring.h"
#include<stdio.h>
#include<memory>

#ifndef DABLENV_H
#include"dablenv.h"
#endif

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLSYMT_H
#include"ablsymt.h"
#endif

#ifndef ABLEXEC_H
#include"ablexec.h"
#endif

#ifndef DABLDBUG_H
#include"dabldbug.h"
#endif

#ifndef ABLSCAN_H
#include"ablscan.h"
#endif

//***************************************************************************

struct _SourceFile {
	char					fileName[MAXLEN_FILENAME];
	unsigned char			fileNumber;
	ABLFile*				filePtr;
	long					lineNumber;
}; using SourceFile = _SourceFile;

//---------------------------------------------------------------------------

inline constexpr int MAX_USER_FILES = 6;
inline constexpr int MAX_USER_FILE_LINES = 50;
inline constexpr int MAX_USER_FILE_LINELEN = 200;

class UserFile {

	public:

		long					handle;
		bool					inUse;
		char					fileName[MAXLEN_FILENAME];
		// close flushes buffered lines before the file member releases its handle.
		ABLFile filePtr;
		long					numLines;
		int                     totalLines;
		char					lines[MAX_USER_FILE_LINES][MAX_USER_FILE_LINELEN];

		static UserFilePtr		files[MAX_USER_FILES];

	public:


			
		void init (void) {
			handle = -1;
			inUse = false;
			fileName[0] = '\0';
			filePtr.close();
			numLines = 0;
			totalLines = 0;
			for (auto & line : lines)
				line[0] = '\0';
		}

		UserFile (void) {
			init();
		}

		void destroy (void) noexcept { close(); }

		~UserFile (void) {
			destroy();
		}

		void dump (void);

		void close (void);

		long open (mc2::czstring fileName);

		void write (mc2::czstring s);

		static void setup (void);

		static void cleanup (void);

		static UserFilePtr getNewFile (void);
};

//---------------------------------------------------------------------------

inline constexpr int MAX_ABLMODULE_NAME = 5;
inline constexpr int MAX_SOURCE_FILES = 256; // per module
inline constexpr int MAX_LIBRARIES_USED = 25; // per module
inline constexpr int MAX_STATE_HANDLES_PER_MODULE = 10;

struct VariableInfo {
	char					name[128];
	int                     size;
};

struct RoutineInfo {
	char					name[128];
	long					codeSegmentSize;
};

struct ModuleInfo {
	char					name[128];
	char					fileName[128];
	int                     numStaticVars;
	int                     totalSizeStaticVars;
	VariableInfo			largestStaticVar;
	long					totalCodeSegmentSize;
	long					numRoutines;
	RoutineInfo				routineInfo[128];
};

struct ModuleEntry {
	char*					fileName;
	SymTableNodePtr			moduleIdPtr;
	long					numSourceFiles;
	char**					sourceFiles;
	long					numLibrariesUsed;
	ABLModulePtr*			librariesUsed;
	int                     numStaticVars;
	long					numOrderCalls;
	long					numStateHandles;
	StateHandleInfoPtr		stateHandles;
	long*					sizeStaticVars;
	long					totalSizeStaticVars;
	long					numInstances;
	long					compileErrorCount;	// OMNITECH-ABL-EXECEXPRESSION: STOPSYNTAX count at compile; gates execute()
};

using ModuleEntryPtr = ModuleEntry*;

class ABLModule {

	private:

		int                     id;
		char					name[MAX_ABLMODULE_NAME];
		int32_t                 handle;
		StackItemPtr			staticData;
		unsigned long*			orderCallFlags;
		StackItem				returnVal;
		bool					initCalled;
		bool runtimeFailed = false;
		long stopAfterRuntimeError(mc2::czstring message);
		SymTableNodePtr			prevState;
		SymTableNodePtr			state;
		// Owned, debugger builds only. The managers keep their ABL-pool operator new/delete, which
		// default_delete honours. Complete types live in abldbug.h, so the ctor/dtor are out of line.
		std::unique_ptr<WatchManager>		watchManager;
		std::unique_ptr<BreakPointManager>	breakPointManager;
		bool					trace;
		bool					step;
		bool					traceEntry;
		bool					traceExit;

		// A2: one place creates and initializes the debugger managers (init and read both call it).
		void initDebuggerManagers (void);

		//static long				numModules;

	public:

			
		void init (void) {
			id = -1;
			name[0] = '\0';
			handle = -1;
			staticData = nullptr;
			orderCallFlags = nullptr;
			returnVal.integer = 0;
			initCalled = false;
			prevState = nullptr;
			state = nullptr;
			// watchManager / breakPointManager: unique_ptr members start null (init() runs only from the constructor)
			trace = false;
			step = false;
			traceEntry = false;
			traceExit = false;
		}


		ABLModule (void);
		ABLModule (const ABLModule&) = delete;
		ABLModule& operator= (const ABLModule&) = delete;

		long init (int moduleHandle);
		
		void write (ABLFile* moduleFile, bool values = false);
		
		void read (ABLFile* moduleFile, bool values = false);

		int getId (void) {
			return(id);
		}

		long getRealId (void);

		int getHandle (void) {
			return(handle);
		}

		StackItemPtr getStaticData (void) {
			return(staticData);
		}

		void setInitCalled (bool called) {
			initCalled = called;
		}

		bool getInitCalled (void) {
			return(initCalled);
		}

		// FNV-1a of this instance's static variables (array statics by their bytes, scalars by their value), folded onto h.
		unsigned long long digestStatics (unsigned long long h);

		char* getFileName (void);

		char* getName (void) {
			return(name);
		}

		void setName (mc2::czstring _name);

		unsigned long* getOrderCallFlags (void) {
			return(orderCallFlags);
		}

		void setPrevState (SymTableNodePtr stateSym) {
			prevState = stateSym;
		}

		SymTableNodePtr getPrevState (void) {
			return(prevState);
		}

		int getPrevStateHandle (void);

		void setState (SymTableNodePtr stateSym) {
			state = stateSym;
		}

		SymTableNodePtr getState (void) {
			return(state);
		}

		int getStateHandle (void);

		bool isLibrary (void);

		void resetOrderCallFlags (void);

		void setOrderCallFlag (unsigned char dword, unsigned char bit);

		void clearOrderCallFlag (unsigned char orderDWord, unsigned char orderBitMask);

		bool getOrderCallFlag (unsigned char dword, unsigned char bit) {
			return((orderCallFlags[dword] & (1 << bit)) != 0);
		}

		WatchManagerPtr getWatchManager (void) {
			return(watchManager.get());
		}

		BreakPointManagerPtr getBreakPointManager (void) {
			return(breakPointManager.get());
		}

		void setTrace (bool _trace) {
			trace = _trace;
			traceEntry = _trace;
			traceExit = _trace;
		}

		bool getTrace (void) {
			return(trace);
		}

		void setStep (bool _step) {
			step = _step;
		}

		bool getStep (void) {
			return(step);
		}

		long execute (ABLParamPtr paramList = nullptr);
		long execute (ABLParamPtr moduleParamList, SymTableNodePtr functionIdPtr);

		SymTableNodePtr findSymbol (mc2::czstring symbolName, SymTableNodePtr curFunction = nullptr, bool searchLibraries = false);

		SymTableNodePtr findFunction (mc2::czstring functionName, bool searchLibraries = false);

		SymTableNodePtr findState (mc2::czstring stateName);

		int findStateHandle (mc2::czstring stateName);

		char* getSourceFile (int fileNumber);

		char* getSourceDirectory (int fileNumber, char* directory);

		void getInfo (ModuleInfo* moduleInfo);

		float getReal (void) {
			return(returnVal.real);
		}
		
		int getInteger (void) {
			return(returnVal.integer);
		}

		int setStaticInteger (char* name, int value);

		int getStaticInteger (char* name);

		int setStaticReal (char* name, float value);
		
		float getStaticReal (char* name);

		int setStaticIntegerArray (char* name, int size, int* values);

		int getStaticIntegerArray (char* name, int size, int* values);
		
		int setStaticRealArray (char* name, int size, float* values);

		int getStaticRealArray (char* name, int size, float* values);

		void destroy (void);

		~ABLModule (void);

};

//*************************************************************************

void initModuleRegistry (long maxModules);
void destroyModuleRegistry (void);
void initLibraryRegistry (long maxLibraries);
void destroyLibraryRegistry (void);

//***************************************************************************

#endif
