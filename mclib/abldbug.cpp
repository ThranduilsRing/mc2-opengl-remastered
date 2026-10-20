//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLDBUG.CPP
//
//***************************************************************************

#include "abldbug_internal.h"
#include "mc2fmt.h"

//extern SymTableNodePtr	CurRoutineIdPtr;

//extern long				MaxLoopIterations;

void CheckMouse(void);

//---------------------------------------------------------------------------

long				MaxBreaks = 50;
long				MaxWatches = 50;
DebuggerPtr			debugger = nullptr;

char Debugger::message[512];

#ifdef USE_IFACE

extern aTextObject* ABLDebuggerIn;
extern ScrollingTextWindow* ABLDebuggerOut;

#endif

//***************************************************************************
// WATCH MANAGER class
//***************************************************************************

void* WatchManager::operator new (size_t mySize) {

	void *result = ABL_alloc(ABLPool::Stack, static_cast<unsigned long>(mySize));
	return(result);
}

//---------------------------------------------------------------------------

void WatchManager::operator delete (void* us) {

	ABL_free(ABLPool::Stack, us);
}

//---------------------------------------------------------------------------

long WatchManager::init (long max) {

	maxWatches = max;
	numWatches = 0;

	watches = static_cast<WatchPtr>(ABL_alloc(ABLPool::Stack, max * sizeof(Watch)));
	if (!watches)
		return(-1);

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void WatchManager::destroy (void) {

	if (watches) {
		ABL_free(ABLPool::Stack, watches);
		watches = nullptr;
	}

	maxWatches = 0;
	numWatches = 0;
}

//---------------------------------------------------------------------------

WatchPtr WatchManager::add (SymTableNodePtr idPtr) {

	//------------------------------------------
	// This routine assumes idPtr is non-NULL...
	switch (idPtr->defn.key) {
		case DFN_CONST:
		case DFN_VAR:
		case DFN_VALPARAM:
		case DFN_REFPARAM:
		//case DFN_FIELD:
			if (idPtr->info)
				return(reinterpret_cast<WatchPtr>(idPtr->info));
			else if (numWatches < maxWatches) {
				idPtr->info = reinterpret_cast<char*>(&watches[numWatches]);
				watches[numWatches].idPtr = idPtr;
				watches[numWatches].store = false;
				watches[numWatches].breakOnStore = false;
				watches[numWatches].fetch = false;
				watches[numWatches].breakOnFetch = false;
				return(&watches[numWatches++]);
			}
			break;
        default: ;
	}

	return(nullptr);
}

//---------------------------------------------------------------------------

long WatchManager::remove (SymTableNodePtr idPtr) {

	if (!idPtr)
		return(1);

	if (!idPtr->info)
		return(2);

	//------------------------------------------
	// Find this id's watch in the watch list...
	long removeIndex;
	for (removeIndex = 0; removeIndex < numWatches; removeIndex++)
		if (&watches[removeIndex] == reinterpret_cast<WatchPtr>(idPtr->info))
			break;

	//--------------------------------------------------------
	// The id no longer points to a watch in the watch list...
	idPtr->info = nullptr;
	numWatches--;

	//-------------------------------------
	// Fill in the gap in the watch list...
	for (long i = removeIndex; removeIndex < numWatches; removeIndex++) {
		watches[i].idPtr = watches[i + 1].idPtr;
		watches[i].store = watches[i + 1].store;
		watches[i].breakOnStore = watches[i + 1].breakOnStore;
		watches[i].fetch = watches[i + 1].fetch;
		watches[i].breakOnFetch = watches[i + 1].breakOnFetch;
		watches[i].idPtr->info = reinterpret_cast<char*>(&watches[i]);
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long WatchManager::removeAll (void) {

	for (long i = 0; i < numWatches; i++) {
		watches[i].idPtr->info = nullptr;
		watches[i].idPtr = nullptr;
		watches[i].store = false;
		watches[i].breakOnStore = false;
		watches[i].fetch = false;
		watches[i].breakOnFetch = false;
	}

	long numRemoved = numWatches;
	numWatches = 0;
	return(numRemoved);
}

//---------------------------------------------------------------------------
		
long WatchManager::setStore (SymTableNodePtr idPtr, bool state, bool breakToDebug) {

	if (!idPtr)
		return(1);

	WatchPtr watch = reinterpret_cast<WatchPtr>(idPtr->info);
	if (state) {
		if (!watch)
			watch = add(idPtr);
		if (!watch)
			return(2);
		watch->store = true;
		watch->breakOnStore = breakToDebug;
		}
	else {
		if (watch) {
			if (watch->fetch) {
				watch->store = false;
				watch->breakOnStore = false;
				}
			else
				remove(idPtr);
		}
	}
	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------
		
long WatchManager::setFetch (SymTableNodePtr idPtr, bool state, bool breakToDebug) {

	if (!idPtr)
		return(1);

	WatchPtr watch = reinterpret_cast<WatchPtr>(idPtr->info);
	if (state) {
		if (!watch)
			watch = add(idPtr);
		if (!watch)
			return(2);
		watch->fetch = true;
		watch->breakOnFetch = breakToDebug;
		}
	else {
		if (watch) {
			if (watch->store) {
				watch->fetch = false;
				watch->breakOnFetch = false;
				}
			else
				remove(idPtr);
		}
	}
	return(ABL_NO_ERR);

}

//---------------------------------------------------------------------------

bool WatchManager::getStore (SymTableNodePtr idPtr) {

	if (!idPtr->info)
		return(false);
	return((reinterpret_cast<WatchPtr>(idPtr->info))->store);
}

//---------------------------------------------------------------------------

bool WatchManager::getFetch (SymTableNodePtr idPtr) {

	if (!idPtr->info)
		return(false);
	return((reinterpret_cast<WatchPtr>(idPtr->info))->fetch);
}

//---------------------------------------------------------------------------

void WatchManager::print (void) {

	for (long i = 0; i < numWatches; i++) {
		//SymTableNodePtr idPtr = watches[i].idPtr;
		// Display info here...
	}
}

//***************************************************************************
// BREAK POINT MANAGER class
//***************************************************************************

void* BreakPointManager::operator new (size_t mySize) {

	void *result = ABL_alloc(ABLPool::Stack, static_cast<unsigned long>(mySize));
	return(result);
}

//---------------------------------------------------------------------------

void BreakPointManager::operator delete (void* us) {

	ABL_free(ABLPool::Stack, us);
}

//---------------------------------------------------------------------------

long BreakPointManager::init (long max) {

	maxBreakPoints = max;
	numBreakPoints = 0;

	breakPoints = static_cast<long*>(ABL_alloc(ABLPool::Stack, max * sizeof(long)));
	if (!breakPoints)
		return(-1);

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void BreakPointManager::destroy (void) {

	if (breakPoints) {
		ABL_free(ABLPool::Stack, breakPoints);
		breakPoints = nullptr;
	}

	maxBreakPoints = 0;
	numBreakPoints = 0;
}

//---------------------------------------------------------------------------

long BreakPointManager::add (long lineNumber) {

	//----------------------------------------------------------------
	// This does not check to make sure the line number is within
	// the source code's range--if it isn't, the break is simply never
	// hit...

	if (numBreakPoints == maxBreakPoints)
		return(1);
	else if (lineNumber <= 0)
		return(2);
	else {
		//------------------------------------------------
		// Insert it into the list (in ascending order)...
		long insertPoint = 0;
		for (insertPoint = 0; insertPoint < numBreakPoints; insertPoint++) {
			if (lineNumber == breakPoints[insertPoint])
				return(ABL_NO_ERR);
			else if (lineNumber < breakPoints[insertPoint])
				break;
		}
		for (long i = insertPoint; i < numBreakPoints; i++)
			breakPoints[i + 1] = breakPoints[i];
		breakPoints[insertPoint] = lineNumber;
		numBreakPoints++;
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long BreakPointManager::remove (long lineNumber) {

	long index;
	for (index = 0; index < numBreakPoints; index++)
		if (lineNumber == breakPoints[index])
			break;

	numBreakPoints--;
	
	//------------------
	// Shift 'em down...
	for (long i = index; i < numBreakPoints; i++)
		breakPoints[i] = breakPoints[i + 1];

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long BreakPointManager::removeAll (void) {

	long numRemoved = numBreakPoints;
	numBreakPoints = 0;
	return(numRemoved);
}

//---------------------------------------------------------------------------

bool BreakPointManager::isBreakPoint (long lineNumber) {

	if (numBreakPoints > 0) {
		for (long i = 0; i < numBreakPoints; i++)
			if (lineNumber == breakPoints[i])
				return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------

void BreakPointManager::print (void) {

	//--------------------------------------------------------------
	// If no line number, do default action--list all breakpoints...
	for (long i = 0; i < numBreakPoints; i++) {
		// print info...
	}
}

//***************************************************************************
// DEBUGGER class
//***************************************************************************

void* Debugger::operator new (size_t mySize) {

	void *result = ABL_alloc(ABLPool::Stack, static_cast<unsigned long>(mySize));
	return(result);
}

//---------------------------------------------------------------------------

void Debugger::operator delete (void* us) {

	ABL_free(ABLPool::Stack, us);
}

//---------------------------------------------------------------------------

long Debugger::init (void (*callback)(const char* s), ABLModulePtr _module) {

	printCallback = callback;
	module = _module;
	
	if (module) {
		watchManager = module->getWatchManager();
		breakPointManager = module->getBreakPointManager();
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void Debugger::destroy (void) {

}

//---------------------------------------------------------------------------

long Debugger::print (const char* s) {

	if (printCallback)
		(*printCallback)(s);
	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void Debugger::setModule (ABLModulePtr _module) {

	module = _module;
	breakPointManager = module->getBreakPointManager();
	watchManager = module->getWatchManager();
	step = module->getStep();
	trace = traceEntry = traceExit = module->getTrace();
}

//---------------------------------------------------------------------------

long Debugger::setWatch (long states) {

	getToken();

	switch (curToken) {
		case TKN_SEMICOLON:
			print("Variables currently watched:\n");
			//-------------------------------------------------------------
			// No variable, so default watch action--display all watches...
			watchManager->print();
			break;
		case TKN_IDENTIFIER:
            {
			SymTableNodePtr idPtr = nullptr;
			searchAndFindAllSymTables(idPtr);
			getToken();
			//-----------------
			// STORE setting...
			bool breakToDebug = ((states & WATCH_BREAK) != 0);
			if (states & WATCH_STORE_OFF)
				watchManager->setStore(idPtr, false, breakToDebug);
			else if (states & WATCH_STORE_ON)
				watchManager->setStore(idPtr, true, breakToDebug);
			//-----------------
			// FETCH setting...
			if (states & WATCH_FETCH_OFF)
				watchManager->setFetch(idPtr, false, breakToDebug);
			else if (states & WATCH_FETCH_ON)
				watchManager->setFetch(idPtr, true, breakToDebug);
            }
			break;
        default:;
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::addBreakPoint (void) {

	getToken();

	switch (curToken) {
		case TKN_SEMICOLON:
			print("Variables currently watched:\n");
			breakPointManager->print();
			break;
		case TKN_NUMBER:
			if (curLiteral.type == LIT_INTEGER)
				breakPointManager->add(curLiteral.value.integer);
			getToken();
			break;
        default:;
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::removeBreakPoint (void) {

	getToken();

	switch (curToken) {
		case TKN_SEMICOLON:
			breakPointManager->removeAll();
			break;
		case TKN_NUMBER:
			if (curLiteral.type == LIT_INTEGER)
				breakPointManager->remove(curLiteral.value.integer);
			getToken();
			break;
        default:;
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void Debugger::processCommand (long commandId, char* strParam1, int numParam1, ABLModulePtr moduleParam1) {

	switch (static_cast<DebugCommandCode>(commandId)) {
		case DEBUG_COMMAND_SET_MODULE:
			if (moduleParam1) {
				debugModule = moduleParam1;
				print(" ");
				sprintf(message, "SET MODULE: %s", debugModule->getName());
				print(message);
				}
			else {
				print(" ");
				displayModuleInstanceRegistry();
				sprintf(message, "CURRENT MODULE: %s", debugModule->getName());
				print(message);
			}
			break;
		case DEBUG_COMMAND_TRACE:
			if (numParam1) {
				debugModule->setTrace(true);
				debugModule->setStep(false);
				if (module == debugModule) {
					trace = true;
					traceEntry = true;
					traceExit = true;
					step = false;
				}
				}
			else {
				debugModule->setTrace(false);
				if (module == debugModule) {
					trace = false;
					traceEntry = false;
					traceExit = false;
				}
			}
			break;
		case DEBUG_COMMAND_STEP:
			if (numParam1) {
				debugModule->setStep(true);
				debugModule->setTrace(false);
				if (module == debugModule) {
					step = true;
					trace = false;
					traceEntry = false;
					traceExit = false;
				}
				}
			else {
				debugModule->setStep(false);
				if (module == debugModule)
					step = false;
			}
			break;
		case DEBUG_COMMAND_BREAKPOINT_SET:
			print(" ");
			debugModule->getBreakPointManager()->add(numParam1);
			sprintf(message, "SET BP: %s (%d)", debugModule->getName(), numParam1);
			print(message);
			break;
		case DEBUG_COMMAND_BREAKPOINT_REMOVE:
			print(" ");
			debugModule->getBreakPointManager()->remove(numParam1);
			sprintf(message, "REMOVE BP: %s (%d)", debugModule->getName(), numParam1);
			print(message);
			break;
		case DEBUG_COMMAND_WATCH_SET: {
			print(" ");
			SymTableNodePtr idPtr = debugModule->findSymbol(strParam1);
			if (!idPtr) {
				print("Unknown identifier in current scope.\n");
				return;
			}
			bool breakToDebug = ((numParam1 & WATCH_BREAK) != 0);
			if (numParam1 & WATCH_STORE_ON) {
				long result = debugModule->getWatchManager()->setStore(idPtr, true, breakToDebug);
				if (result == 2) {
					print("Reached max watch limit--unable to set watch.\n");
					return;
				}
			}
			if (numParam1 & WATCH_FETCH_ON) {
				long result = debugModule->getWatchManager()->setFetch(idPtr, true, breakToDebug);
				if (result == 2) {
					print("Reached max watch limit--unable to set watch.\n");
					return;
				}
			}
			if (numParam1 & WATCH_STORE_OFF)
				debugModule->getWatchManager()->setStore(idPtr, false, breakToDebug);
			if (numParam1 & WATCH_FETCH_OFF)
				debugModule->getWatchManager()->setFetch(idPtr, false, breakToDebug);
			bool store = debugModule->getWatchManager()->getStore(idPtr);
			bool fetch = debugModule->getWatchManager()->getFetch(idPtr);
			if (store || fetch) {
				sprintf(message, "SET WATCH: %s.%s (", debugModule->getName(), strParam1);
				if (store)
					strcat(message, "s");
				if (fetch)
					strcat(message, "f");
				strcat(message, ")");
				}
			else
				sprintf(message, "REMOVE WATCH: %s.%s", debugModule->getName(), strParam1);
			print(message);
			}
			break;
		case DEBUG_COMMAND_WATCH_REMOVE_ALL:
			debugModule->getWatchManager()->removeAll();
			break;
		case DEBUG_COMMAND_PRINT: {
			print(" ");
			long err = sprintValue(message, strParam1);
			switch (err) {
				case ABL_NO_ERR:
					print(message);
					break;
				case 1:
					print("Unknown identifier in current scope.");
					break;
			}
			}
			break;
		case DEBUG_COMMAND_CONTINUE:
			debugCommand = false;
			break;
		case DEBUG_COMMAND_HELP:
			print(" ");
			print("b{+|-} <line#>         set/remove breakpt");
			print("m [0 thru warrior #]   set current module (or list them)");
			print("w[f|s]{+|-}{.} <variable> set/remove variable watch (fetch & store)");
			print("p <variable>           display current value of variable");
			print("s{+|-}                 start/stop step mode");
			print("t{+|-}                 start/stop trace mode");
			print("??                     current module info");
			print("?                      help");
			break;
		case DEBUG_COMMAND_INFO: {
			print(" ");
			sprintf(message, "CURRENT MODULE: %s", debugModule->getName());
			print(message);
			ModuleInfo moduleInfo;
			debugModule->getInfo(&moduleInfo);
			mc2::fmt::to_buf(message, "{:d} static vars, {:d} bytes, {:d} largest", mc2::fmt::as_i32(moduleInfo.numStaticVars), mc2::fmt::as_i32(moduleInfo.totalSizeStaticVars), mc2::fmt::as_i32(moduleInfo.largestStaticVar.size));
			print(message);
			}
			break;
	}
}

//---------------------------------------------------------------------------

void Debugger::debugMode (void) {

	//--------------------------------------------------------------------
	// Some assumptions: this will never be called during a smacker movie.

	//-------------------------------------------------------------------------------
	// When we enter debug mode, we grab control of all message handling from windows
	// until we exit ABL debug mode. This way, the ABL system is preserved for
	// debugging...
	debugModule = module;
	
	message[0] = '\0';
	sprintStatement(message);
	print(message);

#ifdef USE_IFACE
	debugCommand = true;
	while (debugCommand) {
		//----------------------------------------------------------
		// Since MSWindows is locked out, we must Unlock the screen,
		// peek for MSWindows messages, then relock it.
		QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&startTime));
		bool result;
		do {
			MSG	msg;
			result = PeekMessage(&msg,nullptr,0,0,PM_REMOVE);
			if (result)	{
				if (msg.message == WM_QUIT)	{
					debugCommand = false;
					halt = false;
					trace = false;
					step = false;
					traceEntry = false;
					traceExit = false;
					break;
					}
				else {
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
			}
		} while (result);

		 // make sure we lock the screen for a valid ptr before we go to the user routines.
		if (applicationActive) {
			UpdateDisplay(takeScreenShot);
			takeScreenShot = false;
			QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&stopTime));

			if (MPlayer) {
				//WaitForSingleObject(ReceiveMutex, INFINITE);
				//WaitForSingleObject(FreeListMutex, INFINITE);
				MPlayer->processReceiveList();
				//ReleaseMutex(FreeListMutex);
				//ReleaseMutex(ReceiveMutex);
			}

			// Insert our message loop here....
			// and jump to the users idle
			//			.
			//			.
			//			.
			//
			//
			//
			// finished with our user loop...
			CheckMouse();
		}

		prevStart = startTime;
		L_INTEGER totalTime = stopTime - startTime;
		frameRate =  static_cast<float>(countsPerSecond) / static_cast<float>(totalTime) ;
	}
#endif
}

//---------------------------------------------------------------------------

DebuggerPtr ABLi_getDebugger (void) {

	return(debugger);
}
