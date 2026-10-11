// ABL debugger: the statement / value printers, the execution trace hooks, showValue, assignVariable and the module-instance listing.
// Declared in the class headers; shared names in abldbug_internal.h.
#include "abldbug_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------

void Debugger::sprintStatement (char* dest) {

	//---------------------------------------------------------------------
	// First, rebuild the the current statement from the module code. Then,
	// spit it out as we do so...
	bool done = false;
	char* cp = statementStartPtr;
	
	do {
		TokenCodeType token = (TokenCodeType)*cp;
		cp++;
		switch (token) {
			case TKN_STATEMENT_MARKER:
				done = true;
				break;
			case TKN_SEMICOLON:
			case TKN_END_IF:
			case TKN_END_WHILE:
			case TKN_END_FOR:
			case TKN_END_FUNCTION:
			case TKN_END_MODULE:
			case TKN_END_FSM:
			case TKN_END_LIBRARY:
			case TKN_END_CASE:
			case TKN_END_SWITCH:
			case TKN_END_ORDER:
			case TKN_END_STATE:
			case TKN_THEN:
			//case TKN_UNTIL:
				done = true;
				//--------------------------
				// Do NOT want break here...
			default:
				//done = false;
				switch (token) {
					case TKN_ADDRESS_MARKER:
						cp += sizeof(Address);
						break;
					case TKN_IDENTIFIER:
					case TKN_NUMBER:
					case TKN_STRING: {
						SymTableNodePtr symbol = *((SymTableNodePtr*)cp);
						strcat(dest, " ");
						strcat(dest, symbol->name);
						cp += sizeof(SymTableNodePtr);
						}
						break;
					default:
						strcat(dest, " ");
						strcat(dest, TokenStrings[token]);
						break;
				}
				break;
		}
	} while (!done);
}

//---------------------------------------------------------------------------

void Debugger::sprintLineNumber (char* dest) {

	//--------------------------
	// PRINT LINE NUMBER HERE...
	sprintf(dest, "LINE#");
}

//---------------------------------------------------------------------------

void Debugger::sprintDataValue (char* dest, StackItemPtr data, TypePtr dataType) {

	if ((dataType->form == FRM_ENUM) && (dataType != BooleanTypePtr))
		dataType = IntegerTypePtr;

	if (dataType == IntegerTypePtr)
		sprintf(dest, "%d", data->integer);
	else if (dataType == RealTypePtr)
		sprintf(dest, "%0.6f", data->real);
	else if (dataType == BooleanTypePtr)
		sprintf(dest, "%s", (data->integer == 1) ? "true" : "false");
	else if (dataType == CharTypePtr)
		sprintf(dest, "%c", data->byte);
	else if (dataType->form == FRM_ARRAY) {
		if (dataType->info.array.elementTypePtr == CharTypePtr) {
			// PRINT CHAR ARRAY HERE...
			sprintf(dest, "CHAR ARRAY");
		}
		else {
			// PRINT ARRAY HERE...
			sprintf(dest, "ARRAY");
		}
	}
}

//---------------------------------------------------------------------------

long Debugger::sprintSimpleValue (char* dest, SymTableNodePtr symbol) {

	//--------------------------------------------------------------------
	// This code is adapted from execVariable(). If that function changes,
	// this better too!
	TypePtr	typePtr = symbol->typePtr;

	if (symbol->defn.key == DFN_CONST) {
		if (typePtr == IntegerTypePtr)
			sprintf(dest, "%d", symbol->defn.info.constant.value.integer);
		else if (typePtr == CharTypePtr)
			sprintf(dest, "%c", symbol->defn.info.constant.value.character);
		else
			sprintf(dest, "%.4f", symbol->defn.info.constant.value.real);
		}
	else {
		//--------------------------------------------------------------------
		// First, point to the variable's stack item. If the variable's scope
		// level is less than the current scope level, follow the static links
		// to the proper stack frame base...
		StackItemPtr dataPtr = nullptr;
		switch (symbol->defn.info.data.varType) {
			case VAR_TYPE_NORMAL: {
				StackFrameHeaderPtr headerPtr = (StackFrameHeaderPtr)stackFrameBasePtr;
				long delta = level - symbol->level;
				while (delta-- > 0)
					headerPtr = (StackFrameHeaderPtr)headerPtr->staticLink.address;
				dataPtr = (StackItemPtr)headerPtr + symbol->defn.info.data.offset;
				}
				break;
			case VAR_TYPE_ETERNAL:
				dataPtr = (StackItemPtr)stack + symbol->defn.info.data.offset;
				break;
			case VAR_TYPE_STATIC:
				dataPtr = StaticDataPtr + symbol->defn.info.data.offset;
				break;
            default:;
		}

		//---------------------------------------------------------------
		// If it's a scalar or enumeration reference parameter, that item
		// points to the actual item...
		if ((symbol->defn.key == DFN_REFPARAM) && (typePtr->form != FRM_ARRAY)/* && (typePtr->form != FRM_RECORD)*/)
			dataPtr = (StackItemPtr)dataPtr->address;

		if ((typePtr->form != FRM_ARRAY) /*&& (typePtr->form != FRM_RECORD)*/) {
			ABL_Assert(dataPtr != nullptr, 0, " Debugger.sprintSimpleValue(): dataPtr is NULL ");
			if ((typePtr == IntegerTypePtr) || (typePtr->form == FRM_ENUM))
				sprintf(dest, "%d", *((int*)dataPtr));
			else if (typePtr == CharTypePtr)
				sprintf(dest, "\"%c\"", *((char*)dataPtr));
			else
				sprintf(dest, "%.4f", *((float*)dataPtr));
			}
		else
			sprintf(dest, "ARRAY");
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::sprintArrayValue (char* dest, SymTableNodePtr symbol, char* subscriptString) {

	//--------------------------------------------------------------------
	// This code is adapted from execVariable(). If that function changes,
	// this better too!

	//--------------------------------------------------------------------
	// First, point to the variable's stack item. If the variable's scope
	// level is less than the current scope level, follow the static links
	// to the proper stack frame base...
	if (symbol->defn.key == DFN_CONST)
		sprintf(dest, "\"%s\"", symbol->defn.info.constant.value.stringPtr);
	else {
		StackItemPtr dataPtr = nullptr;
		switch (symbol->defn.info.data.varType) {
			case VAR_TYPE_NORMAL: {
				StackFrameHeaderPtr headerPtr = (StackFrameHeaderPtr)stackFrameBasePtr;
				long delta = level - symbol->level;
				while (delta-- > 0)
					headerPtr = (StackFrameHeaderPtr)headerPtr->staticLink.address;
				dataPtr = (StackItemPtr)headerPtr + symbol->defn.info.data.offset;
				}
				break;
			case VAR_TYPE_ETERNAL:
				dataPtr = (StackItemPtr)stack + symbol->defn.info.data.offset;
				break;
			case VAR_TYPE_STATIC:
				dataPtr = StaticDataPtr + symbol->defn.info.data.offset;
				break;
            default:;
		}

		TypePtr	typePtr = symbol->typePtr;

		ABL_Assert(dataPtr != nullptr, 0, " Debugger.sprintArrayValue(): dataPtr is NULL ");

		Address elementAddress = dataPtr->address;
		if (subscriptString) {
			char* cp = subscriptString;
		
			//-----------------------------
			// Get past the open bracket...
			cp++;

			char* token = strtok(&subscriptString[1], ",]");
			while (token) {
				//----------------
				// Read integer...
				long index = atoi(token);
				//-------------------------
				// Range check the index...
				if ((index < 0) || (index >= typePtr->info.array.elementCount))
					return(1);
				elementAddress += (index * typePtr->info.array.elementTypePtr->size);

				typePtr = typePtr->info.array.elementTypePtr;

				token = strtok(nullptr, ",]");
			}
		}

		if ((typePtr->form != FRM_ARRAY)) {
			if ((typePtr == IntegerTypePtr) || (typePtr->form == FRM_ENUM))
				sprintf(dest, "%d", *((int*)elementAddress));
			else if (typePtr == CharTypePtr)
				sprintf(dest, "\"%c\"", *((char*)elementAddress));
			else
				sprintf(dest, "%.4f", *((float*)elementAddress));
			}
		else if (typePtr->info.array.elementTypePtr == CharTypePtr)
			sprintf(dest, "\"%s\"", (char*)elementAddress);
		else
			sprintf(dest, "Could you be more specific?");
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::sprintValue (char* dest, char* exprString) {

	char* subscript = strchr(exprString, '[');
	if (!subscript) {
		//------------------------------------------
		// Probably a simple variable or constant...
		SymTableNodePtr symbol = debugModule->findSymbol(exprString, CurRoutineIdPtr);
		if (!symbol)
			return(1);
		if (symbol->typePtr->form != FRM_ARRAY) {
			sprintSimpleValue(dest, symbol);
			return(ABL_NO_ERR);
		}
	}

	//-----------------
	// Must be array...
	if (subscript) {
		//--------------------------------
		// Looking for specific element...
		char subscriptString[255];
		strcpy(subscriptString, subscript);
		*subscript = '\0';

		SymTableNodePtr symbol = debugModule->findSymbol(exprString, CurRoutineIdPtr);
		if (!symbol)
			return(1);
		sprintArrayValue(dest, symbol, subscriptString);
		}
	else {
		//-------------------------
		// Print the whole thing...
		SymTableNodePtr symbol = debugModule->findSymbol(exprString, CurRoutineIdPtr);
		if (!symbol)
			return(1);
		sprintArrayValue(dest, symbol, nullptr);
	}

	//-----------------------------------------------
	// We have the symbol, so parse the subscripts...

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::traceStatementExecution (void) {

	bool halt = step;
	//--------------------------------------
	// Do we have a breakpoint on this line?
	if (breakPointManager) {
		if (breakPointManager->isBreakPoint(execLineNumber)) {
			mc2::fmt::to_buf(message, "HIT BP: ({:d}) {} [{:d}]", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::as_i32(execLineNumber));
			print(message);
			halt = true;
		}
	}

//	if (trace) {
//		message[0] = NULL;
//		sprintLineNumber(message);
//		print(message);
//	}

	//------------------------------------------------------------------
	// If we've hit a breakpoint or are currently halted due to
	// some other debugging thingy, wait for another debugger command...
	if (halt)
		debugMode();

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::traceRoutineEntry (SymTableNodePtr idPtr) {

	if (traceEntry) {
		mc2::fmt::to_buf(message, "ENTER ({:d}) {}:{}", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::safe(idPtr->name));
		print(message);
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::traceRoutineExit (SymTableNodePtr idPtr) {

	if (traceExit) {
		mc2::fmt::to_buf(message, "EXIT ({:d}) {}:{}", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::safe(idPtr->name));
		print(message);
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::traceDataStore (SymTableNodePtr id, TypePtr idType, StackItemPtr target, TypePtr targetType) {

	//SymTableNodePtr idPtr = debugModule->findSymbol(strParam1);
	//if (!idPtr) {
	//			print("Unknown identifier in current scope.\n");
	//			return;
	//		}
	//		char message[255];
	//		sprintSymbolValue(message, idPtr);
	//		print(message);
	//		}

	if (id->info && ((WatchPtr)id->info)->store) {
		char valString[255];
		sprintDataValue(valString, target, targetType);
		if (idType->form == FRM_ARRAY)
			mc2::fmt::to_buf(message, "STORE: ({:d}) {} [{:d}] -> {}[#] = {}\n", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::as_i32(execLineNumber), mc2::fmt::safe(id->name), mc2::fmt::safe(valString));
		//else if (idType->form == FRM_RECORD)
		//	sprintf(message, "STORE AT LINE %d - %s.# = %s\n", execLineNumber, id->name, valString);
		else
			mc2::fmt::to_buf(message, "STORE: ({:d}) {} [{:d}] -> {} = {}\n", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::as_i32(execLineNumber), mc2::fmt::safe(id->name), mc2::fmt::safe(valString));
		print(message);
		if (((WatchPtr)id->info)->breakOnStore)
			debugMode();
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long Debugger::traceDataFetch (SymTableNodePtr id, TypePtr idType, StackItemPtr data) {

	TypePtr idTypePtr = id->typePtr;
	if (id->info && ((WatchPtr)id->info)->fetch) {
		char valString[255];
		sprintDataValue(valString, data, idType);
		if (idTypePtr->form == FRM_ARRAY)
			mc2::fmt::to_buf(message, "FETCH: ({:d}) {} [{:d}] - {}[#] = {}\n", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::as_i32(execLineNumber), mc2::fmt::safe(id->name), mc2::fmt::safe(valString));
		//else if (idTypePtr->form == FRM_RECORD)
		//	sprintf(message, "STORE AT LINE %d - %s.# = %s\n", id->name, valString);
		else
			mc2::fmt::to_buf(message, "FETCH: ({:d}) {} [{:d}] - {} = {}\n", mc2::fmt::as_i32(module->getId()), mc2::fmt::safe(module->getName()), mc2::fmt::as_i32(execLineNumber), mc2::fmt::safe(id->name), mc2::fmt::safe(valString));
		print(message);
		if (((WatchPtr)id->info)->breakOnFetch)
			debugMode();
	}

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

void Debugger::showValue (void) {

	getToken();

	if (curToken == TKN_SEMICOLON)
			print("Bad Expression.\n");
	else {
		//------------------------------------------------------------
		// NOTE: Need a SOFT FATAL for parsing expressions here so the
		// debugger's errors don't Fatal out of the game!
		//------------------------------------------------------------

		//---------------------------------------------------------------
		// It's important that the expression parser return an error code
		// rather than fatal!
		TypePtr typePtr = expression();
		if (errorCount > 0)
			return;
			
		char* savedCodeSegmentPtr = codeSegmentPtr;
		TokenCodeType savedCodeToken = codeToken;
		execExpression();

		if (typePtr->form == FRM_ARRAY) {
			print("SHOW ARRAY\n");
			}
		else {
			char message[255];
			sprintDataValue(message, tos, typePtr);
			strcat(message, "\n");
			print(message);
		}

		pop();
		codeSegmentPtr = savedCodeSegmentPtr;
		codeToken = savedCodeToken;
	}

}

//---------------------------------------------------------------------------

void Debugger::assignVariable (void) {

	getToken();

#if 0
	if (curToken == TKN_SEMICOLON)
		print("Need a variable.\n");
	else if (curToken == TKN_IDENTIFIER) {
		//----------------------------------
		// Parse the assignment statement...
		SymTableNodePtr idPtr = nullptr;
		searchAndFindAllSymTables(idPtr);
		assigmentStatement(idPtr);
		if (errorCount > 0)
			return;

		//-------------------
		// Now, execute it...
		char* savedCodeSegmentPtr = codeSegmentPtr;
		long savedCodeToken = codeToken;
		codeSegmentPtr = codeBuffer + 1;
		getCodeToken();
		idPtr = getSymTableCodePtr();
		execAssignmentStatement(idPtr);

		//----------------------------
		// Restore the code segment...
		codeSegmentPtr = savedCodeSegmentPtr;
		codeToken = savedCodeToken;
	}
#endif
}

//---------------------------------------------------------------------------

void Debugger::displayModuleInstanceRegistry (void) {

	char buffer1[200], buffer2[40];
	for (long i = 0; i < ((NumModuleInstances + 1) / 2); i++) {
		mc2::fmt::to_buf(buffer1, "({:02d}) {:<20} ", mc2::fmt::as_i32(ModuleInstanceRegistry[i * 2]->getId()), mc2::fmt::safe(ModuleInstanceRegistry[i * 2]->getName()));
		if ((i * 2 + 1) < NumModuleInstances) {
			mc2::fmt::to_buf(buffer2, "({:02d}) {:<20} ", mc2::fmt::as_i32(ModuleInstanceRegistry[i * 2 + 1]->getId()), mc2::fmt::safe(ModuleInstanceRegistry[i * 2 + 1]->getName()));
			strcat(buffer1, buffer2);
		}
		print(buffer1);
	}

}
