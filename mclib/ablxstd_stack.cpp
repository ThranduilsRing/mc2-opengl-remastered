// ABL standard routines: the ABLi_pop / push / peek / poke accessors for the interpreter stack.
// Declared in the class headers; shared names in ablxstd_internal.h.
#include "ablxstd_internal.h"
#include "abl_string_append.h"


//***************************************************************************
// USEFUL ABL HELP ROUTINES
//***************************************************************************

char ABLi_popChar (void) {

	getCodeToken();
	execExpression();
	char val = (char)tos->integer;
	pop();
	
	return(val);
}

//---------------------------------------------------------------------------

int ABLi_popInteger (void) {

	getCodeToken();
	execExpression();
	int val = tos->integer;
	pop();
	
	return(val);
}

//---------------------------------------------------------------------------

float ABLi_popReal (void) {

	getCodeToken();
	execExpression();
	float val = tos->real;
	pop();

	return(val);
}

//---------------------------------------------------------------------------

float ABLi_popIntegerReal (void) {

	getCodeToken();
	TypePtr paramTypePtr = execExpression();
	float val = 0.0;
	if (paramTypePtr == IntegerTypePtr)
		val = (float)tos->integer;
	else
		val = tos->real;
	pop();

	return(val);
}

//---------------------------------------------------------------------------

bool ABLi_popBoolean (void) {

	getCodeToken();
	execExpression();
	int val = tos->integer;
	pop();
	
	return(val == 1);
}

//---------------------------------------------------------------------------

char* ABLi_popCharPtr (void) {

	//--------------------------
	// Get destination string...
	getCodeToken();
	execExpression();
	char* charPtr = (char*)tos->address;
	pop();

	return(charPtr);
}

//---------------------------------------------------------------------------

std::span<char> ABLi_popCharBuf (void) {

	getCodeToken();
	const TypePtr typePtr = execExpression();
	char* const address = tos->address;
	pop();

	const bool isArray = typePtr && typePtr->form == FRM_ARRAY;
	return abl::charBufOf(address, isArray, isArray && typePtr->info.array.elementTypePtr == CharTypePtr,
	                      isArray ? typePtr->info.array.elementCount : 0);
}

//---------------------------------------------------------------------------

int* ABLi_popIntegerPtr (void) {

	getCodeToken();
	SymTableNodePtr idPtr = getCodeSymTableNodePtr();
	execVariable(idPtr, USE_REFPARAM);
	int* integerPtr = (&((StackItemPtr)tos->address)->integer);
	pop();

	return(integerPtr);
}

//---------------------------------------------------------------------------

float* ABLi_popRealPtr (void) {

	getCodeToken();
	SymTableNodePtr idPtr = getCodeSymTableNodePtr();
	execVariable(idPtr, USE_REFPARAM);
	float* realPtr = (&((StackItemPtr)tos->address)->real);
	pop();

	return(realPtr);
}

//---------------------------------------------------------------------------

char* ABLi_popBooleanPtr (void) {

	//--------------------------
	// Get destination string...
	getCodeToken();
	execExpression();
	char* charPtr = (char*)tos->address;
	pop();

	return(charPtr);
}

//---------------------------------------------------------------------------

long ABLi_popAnything (ABLStackItem* value) {

	getCodeToken();
	TypePtr paramTypePtr = execExpression();

	long type = -1;
	if (paramTypePtr == IntegerTypePtr) {
		value->type = type = ABL_STACKITEM_INTEGER;
		value->data.integer = tos->integer;
		}
	else if (paramTypePtr == BooleanTypePtr) {
		value->type = type = ABL_STACKITEM_BOOLEAN;
		value->data.boolean = (tos->integer ? true : false);
		}
	else if (paramTypePtr == CharTypePtr) {
		value->type = type = ABL_STACKITEM_CHAR;
		value->data.character  = tos->byte;
		}
	else if (paramTypePtr == RealTypePtr) {
		value->type = type = ABL_STACKITEM_REAL;
		value->data.real = tos->real;
		}
	else if (paramTypePtr->form == FRM_ARRAY) {
		if (paramTypePtr->info.array.elementTypePtr == CharTypePtr) {
			value->type = type = ABL_STACKITEM_CHAR_PTR;
			value->data.characterPtr = (char*)tos->address;
			}
		else if (paramTypePtr->info.array.elementTypePtr == IntegerTypePtr) {
			value->type = type = ABL_STACKITEM_INTEGER_PTR;
			value->data.integerPtr = (int*)tos->address;
			}
		else if (paramTypePtr->info.array.elementTypePtr == RealTypePtr) {
			value->type = type = ABL_STACKITEM_REAL_PTR;
			value->data.realPtr = (float*)tos->address;
			}
		else if (paramTypePtr->info.array.elementTypePtr == BooleanTypePtr) {
			value->type = type = ABL_STACKITEM_BOOLEAN_PTR;
			value->data.booleanPtr = (bool*)tos->address;
		}
	}
	pop();
	return(type);
}

//---------------------------------------------------------------------------

void ABLi_pushBoolean (bool value) {

	StackItemPtr valuePtr = ++tos;

	if (valuePtr >= &stack[MAXSIZE_STACK])
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->integer = value ? 1 : 0;
}

//---------------------------------------------------------------------------

void ABLi_pushInteger (int value) {

	StackItemPtr valuePtr = ++tos;

	if (valuePtr >= &stack[MAXSIZE_STACK])
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->integer = value;
}

//---------------------------------------------------------------------------

void ABLi_pushReal (float value) {

	StackItemPtr valuePtr = ++tos;

	if (valuePtr >= &stack[MAXSIZE_STACK])
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->real = value;
}

//---------------------------------------------------------------------------

void ABLi_pushChar (char value) {

	StackItemPtr valuePtr = ++tos;

	if (valuePtr >= &stack[MAXSIZE_STACK])
		runtimeError(ABL_ERR_RUNTIME_STACK_OVERFLOW);
	valuePtr->integer = value;
}

//---------------------------------------------------------------------------

int ABLi_peekInteger (void) {

	getCodeToken();
	execExpression();
	return(tos->integer);
}

//---------------------------------------------------------------------------

float ABLi_peekReal (void) {

	getCodeToken();
	execExpression();
	return(tos->real);
}

//---------------------------------------------------------------------------

bool ABLi_peekBoolean (void) {

	getCodeToken();
	execExpression();
	return(tos->integer == 1);
}

//---------------------------------------------------------------------------

char* ABLi_peekCharPtr (void) {

	getCodeToken();
	execExpression();
	return((char*)tos->address);
}

//---------------------------------------------------------------------------

int* ABLi_peekIntegerPtr (void) {

	getCodeToken();
	SymTableNodePtr idPtr = getCodeSymTableNodePtr();
	execVariable(idPtr, USE_REFPARAM);
	return((&((StackItemPtr)tos->address)->integer));
}

//---------------------------------------------------------------------------

float* ABLi_peekRealPtr (void) {

	getCodeToken();
	SymTableNodePtr idPtr = getCodeSymTableNodePtr();
	execVariable(idPtr, USE_REFPARAM);
	return((&((StackItemPtr)tos->address)->real));
}

//---------------------------------------------------------------------------

void ABLi_pokeChar (int val) {

	tos->integer = val;
}

//---------------------------------------------------------------------------

void ABLi_pokeInteger (int val) {

	tos->integer = val;
}

//---------------------------------------------------------------------------

void ABLi_pokeReal (float val) {

	tos->real = val;
}

//---------------------------------------------------------------------------

void ABLi_pokeBoolean (bool val) {

	tos->integer = val ? 1 : 0;
}
