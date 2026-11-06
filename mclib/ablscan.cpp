//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLSCAN.CPP
//
//***************************************************************************

#include "mc2_czstring.h"
#include<stdio.h>
#include<stdlib.h>
#include<math.h>
#include<string.h>
#include"platform_str.h"
#include<time.h>
#include "abl_include_path.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/utils/mc2log.h"

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLERR_H
#include"ablerr.h"
#endif

#ifndef ABLSCAN_H
#include"ablscan.h"
#endif

#ifndef ABLPARSE_H
#include"ablparse.h"
#endif

#ifndef ABLENV_H
#include"ablenv.h"
#endif

//***************************************************************************
#pragma warning(disable:4244)

//---------------------
// RESERVED WORD tables

constexpr int MINLEN_RESERVED_WORD = 2;
constexpr int MAXLEN_RESERVED_WORD = 11;

ReservedWord reservedWord2[] = {
	{"if", TKN_IF},
	{"or", TKN_OR},
	{"do", TKN_DO},
	{"to", TKN_TO},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord3[] = {
	{"and", TKN_AND},
	{"for", TKN_FOR},
	{"mod", TKN_MOD},
	{"not", TKN_NOT},
	{"var", TKN_VAR},
	{"fsm", TKN_FSM},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord4[] = {
	{"else", TKN_ELSE},
	{"then", TKN_THEN},
	{"case", TKN_CASE},
	{"code", TKN_CODE},
	{"type", TKN_TYPE},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord5[] = {
	{"const", TKN_CONST},
	{"until", TKN_UNTIL},
	{"while", TKN_WHILE},
	{"endif", TKN_END_IF},
	{"order", TKN_ORDER},
	{"state", TKN_STATE},
	{"trans", TKN_TRANS},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord6[] = {
	{"module", TKN_MODULE},
	{"repeat", TKN_REPEAT},
	{"endfor", TKN_END_FOR},
	{"switch", TKN_SWITCH},
	{"static", TKN_STATIC},
	{"endfsm", TKN_END_FSM},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord7[] = {
	{"endcase", TKN_END_CASE},
	{"eternal", TKN_ETERNAL},
	{"library", TKN_LIBRARY},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord8[] = {
	{"function", TKN_FUNCTION},
	{"endwhile", TKN_END_WHILE},
	{"endorder", TKN_END_ORDER},
	{"endstate", TKN_END_STATE},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord9[] = {
	{"endswitch", TKN_END_SWITCH},
	{"endmodule", TKN_END_MODULE},
	{"transback", TKN_TRANS_BACK},
	{nullptr, TKN_NONE}					//This was missing.  Thank You Symantec!!!!		1/24/97 -fs
};

ReservedWord reservedWord10[] = {
	{"endlibrary", TKN_END_LIBRARY},
	{nullptr, TKN_NONE}
};

ReservedWord reservedWord11[] = {
	{"endfunction", TKN_END_FUNCTION},
	{nullptr, TKN_NONE}
};

ReservedWord* reservedWordTable[] = {
	nullptr,
	nullptr,
	reservedWord2,
	reservedWord3,
	reservedWord4,
	reservedWord5,
	reservedWord6,
	reservedWord7,
	reservedWord8,
	reservedWord9,
	reservedWord10,
	reservedWord11
};

//------------
// TOKEN lists

TokenCodeType statementStartList[] = {
	TKN_FOR,
	TKN_IF,
	TKN_REPEAT,
	TKN_WHILE,
	TKN_SWITCH,
	TKN_TRANS,
	TKN_TRANS_BACK,
	TKN_IDENTIFIER,
	TKN_NONE
};

TokenCodeType statementEndList[] = {
	TKN_SEMICOLON,
	TKN_END_IF,
	TKN_END_WHILE,
	TKN_END_FOR,
	TKN_END_SWITCH,
	TKN_END_FUNCTION,
	TKN_END_ORDER,
	TKN_END_STATE,
	TKN_ELSE,
	TKN_ELSIF,
	TKN_UNTIL,
	TKN_EOF,
	TKN_NONE
};

TokenCodeType declarationStartList[] = {
	TKN_CONST,
	TKN_VAR,
	TKN_FUNCTION,
	TKN_ORDER,
	TKN_STATE,
	TKN_NONE
};

const char* TokenStrings[std::to_underlying(TokenCodeType::NUM_TOKENS)] = {
	"{BAD TOKEN}",
	"{IDENTIFIER}",
	"{NUMBER}",
	"{TYPE}",
	"{STRING}",
	"*",
	"(",
	")",
	"-",
	"+",
	"=",
	"[",
	"]",
	":",
	";",
	"<",
	">",
	",",
	".",
	"/",
	"==",
	"<=",
	">=",
	"<>",
	"{EOF}",
	"{ERROR}",
	"code",
	"order",
	"state",
	"and",
	"switch",
	"case",
	"const",
	"div",
	"do",
	"of",
	"else",
	"endif",
	"endwhile",
	"endfor",
	"endfunction",
	"endorder",
	"endstate",
	"endmodule",
	"endfsm",
	"endlibrary",
	"endvar",
	"endcode",
	"endcase",
	"endswitch",
	"for",
	"function",
	"if",
	"mod",
	"not",
	"or",
	"repeat",
	"then",
	"to",
	"until",
	"var",
	"@",
	"while",
	"elsif",
	"return",
	"module",
	"fsm",
	"trans",
	"transback",
	"library",
	"eternal",
	"static",
	"#",
	"{UNEXPECTED TOKEN}",
	"{STATEMENT MARKER}",
	"{ADDRESS MARKER}"
};

//--------
// GLOBALS

char			curChar;
TokenCodeType	curToken;
Literal			curLiteral;
int32_t         level = 0;
int32_t         lineNumber = 0;
int32_t         FileNumber = 0;
ABLFile*		sourceFile = nullptr;
static ABLSourceFiles* sourceFilesOwner = nullptr; // Borrowed for one preprocessing call.
bool			printFlag = true;
bool			blockFlag = false;
BlockType		blockType = BLOCK_MODULE;
SymTableNodePtr	CurModuleIdPtr = nullptr;
SymTableNodePtr	CurRoutineIdPtr = nullptr;
bool			DumbGetCharOn = false;

long			NumOpenFiles = 0;
long			NumSourceFiles = 0;
char			SourceFiles[MAX_SOURCE_FILES][MAXLEN_FILENAME];
SourceFile		openFiles[MAX_INCLUDE_DEPTH];
char			sourceBuffer[MAXLEN_SOURCELINE];
char			tokenString[MAXLEN_TOKENSTRING];
char			wordString[MAXLEN_TOKENSTRING];
long			bufferOffset = 0;
char*			bufferp = sourceBuffer;
char*			tokenp = tokenString;

long			digitCount = 0;
bool			countError = false;

int             	pageNumber = 0;
int             	lineCount = MAX_LINES_PER_PAGE;

char			sourceName[MAXLEN_FILENAME];
char			date[LEN_DATESTRING];

CharCodeType	charTable[256];

extern bool		AssertEnabled;
extern bool		PrintEnabled;
extern bool		StringFunctionsEnabled;
extern bool		DebugCodeEnabled;

extern ABLModulePtr	CurLibrary;

//***************************************************************************
// ABL FILE routines
//***************************************************************************



//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

//***************************************************************************
// MISC routines
//***************************************************************************

inline CharCodeType calcCharCode (long ch) {

	return(charTable[ch]);
}

//***************************************************************************

long isReservedWord (void) 
{
	long wordLength = static_cast<long>(strlen(wordString));
	if ((wordLength >= MINLEN_RESERVED_WORD) && (wordLength <= MAXLEN_RESERVED_WORD) && reservedWordTable[wordLength])
	{
		for (ReservedWord* rwPtr = reservedWordTable[wordLength]; rwPtr->string != nullptr; rwPtr++)
		{
			if (strcmp(wordString, rwPtr->string) == 0) 
			{
				curToken = rwPtr->tokenCode;
				return(1);
			}
		}
	}
	return(0);
}

//***************************************************************************
// INITIALIZATION routines
//***************************************************************************

ABLSourceFiles::ABLSourceFiles() {
	sourceFilesOwner = this;
}

ABLSourceFiles::~ABLSourceFiles() noexcept {
	// Includes close before their parent; callbacks remain alive throughout preprocessing.
	while (NumOpenFiles > 0)
		closeSourceFile();
	sourceFilesOwner = nullptr;
}

//***************************************************************************
// CHARACTER routines
//***************************************************************************

void skipLineComment (void) {

	//-------------------------------------------------------------------
	// Just blow off the rest of the current source line we've read in...
	*bufferp = '\0';
	curChar = ' ';
}

//***************************************************************************

void skipBlockComment (void) {

	DumbGetCharOn = true;
	getChar();
	getChar();
	do {
		while ((curChar != '*') && (curChar != CHAR_EOF))
			getChar();
		if (curChar == CHAR_EOF) {
			syntaxError(ABL_ERR_SYNTAX_UNEXPECTED_EOF);
			curToken = TKN_ERROR;
		}
		getChar();
	} while (curChar != '/');
	curChar = ' ';
	DumbGetCharOn = false;
}

//***************************************************************************

void skipBlanks (void) {

	while (curChar == ' ')
		getChar();
}

//***************************************************************************

void languageDirective (void) {

	DumbGetCharOn = true;
	getChar();

	char directive[32];
	long directiveLength = 0;
	while ((curChar != ' ') && (curChar != '\n')  && (curChar != '\r') && (directiveLength < 31)) {
		directive[directiveLength++] = curChar;
		getChar();
	}
	directive[directiveLength] = '\0';
	S_strlwr(directive);

	if (strcmp(directive, "include") == 0) {
		getChar();
		if (curChar == '"') {
			getChar();
			char fileName[128];
			long fileNameLength = 0;
			while ((curChar != '"') && (fileNameLength < 127)) {
				fileName[fileNameLength++] = curChar;
				getChar();
			}
			fileName[fileNameLength] = '\0';
			// Include directives occupy a complete source line.
			DumbGetCharOn = false;
			long openErr = ABL_NO_ERR;
			if ((openErr = openSourceFile(fileName)) != ABL_NO_ERR) {
				syntaxError(ABL_ERR_SYNTAX_SOURCE_FILE_OPEN);
				curToken = TKN_ERROR;
			}
			}
		else {
			syntaxError(ABL_ERR_SYNTAX_BAD_LANGUAGE_DIRECTIVE_USAGE);
			curToken = TKN_ERROR;
		}
		}
	else if (strcmp(directive, "include_") == 0) {
		getChar();
		if (curChar == '"') {
			getChar();
			char fileName[128];
			long fileNameLength = 0;
			while ((curChar != '"') && (fileNameLength < 127)) {
				fileName[fileNameLength++] = curChar;
				getChar();
			}
			fileName[fileNameLength] = '\0';
			// Include directives occupy a complete source line.
			DumbGetCharOn = false;
			long openErr = ABL_NO_ERR;

			// The include sits next to the including file (abl_include_path.h; doctest test_abl_include_path.cpp).
			const std::string fullPath = ablIncludePath(SourceFiles[0], fileName);
			if ((openErr = openSourceFile(fullPath.c_str())) != ABL_NO_ERR) {
				syntaxError(ABL_ERR_SYNTAX_SOURCE_FILE_OPEN);
				curToken = TKN_ERROR;
			}
			}
		else {
			syntaxError(ABL_ERR_SYNTAX_BAD_LANGUAGE_DIRECTIVE_USAGE);
			curToken = TKN_ERROR;
		}
		}
	else if (strcmp(directive, "assert_on") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		AssertEnabled = true;
		}
	else if (strcmp(directive, "assert_off") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		AssertEnabled = false;
		}
	else if (strcmp(directive, "print_on") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		PrintEnabled = true;
		}
	else if (strcmp(directive, "print_off") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		PrintEnabled = false;
		}
	else if (strcmp(directive, "stringfuncs_on") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		StringFunctionsEnabled = true;
		}
	else if (strcmp(directive, "stringfuncs_off") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		StringFunctionsEnabled = false;
		}
	else if (strcmp(directive, "debug_on") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		DebugCodeEnabled = true;
		}
	else if (strcmp(directive, "debug_off") == 0) {
		DumbGetCharOn = false;
		curChar = ' ';
		DebugCodeEnabled = false;
		}
	else if (strcmp(directive, "debug_start") == 0) {
		bool stillCutting = true;
		if (!DebugCodeEnabled) {
			long nestedLevel = 1;
			getChar();
			do {
				while ((curChar != '#') && (curChar != CHAR_EOF))
					getChar();
				if (curChar == '#') {
					char directive2[32];
					long directiveLength = 0;
					while ((curChar != ' ') && (curChar != '\n')  && (curChar != '\r') && (directiveLength < 31)) {
						directive2[directiveLength++] = curChar;
						getChar();
					}
					directive2[directiveLength] = '\0';
					S_strlwr(directive2);
					if (strcmp(directive2, "#debug_start") == 0)
						nestedLevel++;
					else if (strcmp(directive2, "#debug_end") == 0) {
						nestedLevel--;
						if (nestedLevel == 0)
							stillCutting = false;
					}
					}
				else if (curChar == CHAR_EOF) {
					syntaxError(ABL_ERR_SYNTAX_UNEXPECTED_EOF);
					curToken = TKN_ERROR;
					}
				else
					getChar();
			} while (stillCutting);
		}
		curChar = ' ';
		DumbGetCharOn = false;
		}
	else if (strcmp(directive, "debug_end") == 0) {
		curChar = ' ';
		DumbGetCharOn = false;
		}
	else {
		syntaxError(ABL_ERR_SYNTAX_UNKNOWN_LANGUAGE_DIRECTIVE);
		curToken = TKN_ERROR;
	}
}

//***************************************************************************

void getChar (void) {

	if (*bufferp == '\0') {
		if (!getSourceLine()) {
			if (NumOpenFiles > 1) {
				closeSourceFile();
				return;
				}
			else {
				curChar = CHAR_EOF;
				return;
			}
		}
		bufferp = sourceBuffer;
		bufferOffset = 0;
	}

	curChar = *bufferp;
	bufferp++;

	if (DumbGetCharOn) {
		bufferOffset++;
		return;
	}

	//-------------------
	// Special Characters
	switch (curChar) {
		case '\t':
			// TAB
			bufferOffset += TAB_SIZE - bufferOffset % TAB_SIZE;
			curChar = ' ';
			break;
		case '\n':
		case '\r':
			// NEWLINE
			bufferOffset++;
			curChar = ' ';
			break;
		case '/':
			if (*bufferp == '/')
				skipLineComment();
			else if (*bufferp == '*')
				skipBlockComment();
			else
				bufferOffset++;
			break;
		case '#':
			languageDirective();
			break;
		default:
			bufferOffset++;
	}
}

//***************************************************************************
// TOKEN routines
//***************************************************************************

void downShiftWord (void) {

	long offset = 'a' - 'A';
	char* wp = wordString;
	char* tp = tokenString;

	long checkLengthWord = static_cast<long>(strlen(wordString));
	long checkLengthToken = static_cast<long>(strlen(tokenString));

	if ((checkLengthWord >= MAXLEN_TOKENSTRING) || (checkLengthToken >= MAXLEN_TOKENSTRING))
		ABL_Fatal(-1," Boy did Glenn screw the pooch here!! ");

	do {
		*wp = ((*tp >= 'A') && (*tp <= 'Z')) ? (*tp + static_cast<char>(offset)) : *tp;
		wp++;
		tp++;
	} while (*tp != '\0');
	*wp = '\0';
}

//***************************************************************************

void getToken(void) {

	skipBlanks();
	tokenp = tokenString;

	switch (calcCharCode(curChar)) {
		case CHR_LETTER:
			getWord();
			break;
		case CHR_DIGIT:
			getNumber();
			break;
		case CHR_DQUOTE:
			getString();
			break;
		case CHR_EOF:
			curToken = TKN_EOF;
			break;
		default:
			getSpecial();
			break;
	}

	if (blockFlag)
		crunchToken();
}

//***************************************************************************

void getWord (void) {

	while ((calcCharCode(curChar) == CHR_LETTER) || (calcCharCode(curChar) == CHR_DIGIT) || (curChar == '_')) {
		*tokenp = curChar;
		tokenp++;
		getChar();
	}

	*tokenp = '\0';
	downShiftWord();

	if (curChar == '.') {
		TokenCodeType endToken = TKN_END_MODULE;
		if (CurLibrary)
			endToken = TKN_END_LIBRARY;
		else if (CurModuleIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM)
			endToken = TKN_END_FSM;
		//-------------------------------------------------------------------
		// If we have an "endmodule" string, stop. Otherwise, keep reading...
		if (strcmp(wordString, TokenStrings[static_cast<int>(endToken)])) {
			*tokenp = curChar;
			tokenp++;
			getChar();
			while ((calcCharCode(curChar) == CHR_LETTER) || (calcCharCode(curChar) == CHR_DIGIT) || (curChar == '_')) {
				*tokenp = curChar;
				tokenp++;
				getChar();
			}
			*tokenp = '\0';
			downShiftWord();
		}
	}


	if (!isReservedWord())
		curToken = TKN_IDENTIFIER;
}

//***************************************************************************

void accumulateValue (float* valuePtr, SyntaxErrorType errCode) {

	float value = *valuePtr;

	//--------------------------------------------
	// Make sure the first character is a digit...
	if (calcCharCode(curChar) != CHR_DIGIT) {
		syntaxError(errCode);
		curToken = TKN_ERROR;
		return;
	}

	do {
		*tokenp = curChar;
		tokenp++;
		if (++digitCount <= MAX_DIGIT_COUNT)
			value = 10 * value + (curChar - '0');
		else
			countError = true;

		getChar();
	} while (calcCharCode(curChar) == CHR_DIGIT);

	*valuePtr = value;
}

//***************************************************************************

void getNumber (void) {

	long wholeCount = 0;
	long decimalOffset = 0;
	char exponentSign = '+';
	long exponent = 0;
	float numberValue = static_cast<float>(0.0);
	float exponentValue = static_cast<float>(0.0);
	digitCount = 0;
	countError = false;
	curToken = TKN_NONE;

	//------------------------------------
	// Assume it's an integer, at first...
	curLiteral.type = LIT_INTEGER;

	accumulateValue(&numberValue, ABL_ERR_SYNTAX_INVALID_NUMBER);
	if (curToken == TKN_ERROR)
		return;
	wholeCount = digitCount;

	if (curChar == '.') {
		getChar();
		curLiteral.type = LIT_REAL;
		*tokenp = '.';
		tokenp++;

		accumulateValue(&numberValue, ABL_ERR_SYNTAX_INVALID_FRACTION);
		if (curToken == TKN_ERROR)
			return;
		decimalOffset = wholeCount - digitCount;
	}

	if ((curChar == 'E') || (curChar == 'e')) {
		curLiteral.type = LIT_REAL;
		*tokenp = curChar;
		tokenp++;

		//-------------------
		// Any exponent sign?
		if ((curChar == '+') || (curChar == '-')) {
			*tokenp = exponentSign = curChar;
			tokenp++;
			getChar();
		}

		accumulateValue(&exponentValue, ABL_ERR_SYNTAX_INVALID_EXPONENT);
		if (curToken == TKN_ERROR)
			return;
		if (exponentSign == '-')
			exponentValue = -exponentValue;
	}

	//-----------------
	// Too many digits?
	if (countError) {
		syntaxError(ABL_ERR_SYNTAX_TOO_MANY_DIGITS);
		curToken = TKN_ERROR;
		return;
	}

	//------------------------------------------------------------
	// Adjust number's value using decimal offset and the exponent
	exponent = exponentValue + decimalOffset;
	if ((exponent + wholeCount < -MAX_EXPONENT) || (exponent + wholeCount > MAX_EXPONENT)) {
		syntaxError(ABL_ERR_SYNTAX_REAL_OUT_OF_RANGE);
		curToken = TKN_ERROR;
		return;
	}

	if (exponent != 0)
		numberValue *= static_cast<float>(pow(10.0, static_cast<double>(exponent)));

	//---------------------------
	// Set the literal's value...
	if (curLiteral.type == LIT_INTEGER) 
	{
		// This line can never be true, right?  
		// If I cast to long it can't be outside that range!!
//		if (((long)numberValue < -MAX_INTEGER) || ((long)numberValue > MAX_INTEGER)) 
//		{
//			syntaxError(ABL_ERR_SYNTAX_INTEGER_OUT_OF_RANGE);
//			curToken = TKN_ERROR;
//			return;
//		}
		curLiteral.value.integer = static_cast<int>(numberValue);
		}
	else
		curLiteral.value.real = numberValue;

	*tokenp = '\0';
	curToken = TKN_NUMBER;
}

//***************************************************************************

//---------------------------------------------------
// NOTE: Currently, ABL does not have STRING types...

void getString (void) {

	char* stringPtr = curLiteral.value.string;
	*tokenp = '\"';
	getChar();

	while (curChar != CHAR_EOF) {
		if (curChar == '\"')
			break;
		*tokenp = curChar;
		tokenp++;
		*stringPtr = curChar;
		stringPtr++;
		getChar();
	}
	curChar = ' ';

	*tokenp = '\0';
	*stringPtr = '\0';
	curToken = TKN_STRING;
	curLiteral.type = LIT_STRING;
}

//***************************************************************************

void getSpecial (void) {

	*tokenp = curChar;
	tokenp++;

	switch (curChar) {
//		case '^':
//			curToken = TKN_UPARROW;
//			getChar();
//			break;
		case '*':
			curToken = TKN_STAR;
			getChar();
			break;
		case '(':
			curToken = TKN_LPAREN;
			getChar();
			break;
		case ')':
			curToken = TKN_RPAREN;
			getChar();
			break;
		case '-':
			curToken = TKN_MINUS;
			getChar();
			break;
		case '+':
			curToken = TKN_PLUS;
			getChar();
			break;
		case '[':
			curToken = TKN_LBRACKET;
			getChar();
			break;
		case ']':
			curToken = TKN_RBRACKET;
			getChar();
			break;
		case ';':
			curToken = TKN_SEMICOLON;
			getChar();
			break;
		case '@':
			curToken = TKN_REF;
			getChar();
			break;
		case '#':
			curToken = TKN_POUND;
			getChar();
			break;
// NOTE: For now, let's use the BEGIN and END keywords instead of braces.
// If popular demand is to get rid of the begin/end keywords and switch
// to braces, we can do so...
//		case '{':
//			curToken = TKN_BEGIN;
//			getChar();
//			break;
//		case '}':
//			curToken = TKN_END;
//			getChar();
//			break;
		case ',':
			curToken = TKN_COMMA;
			getChar();
			break;
		case '/':
			//-----------------------------------------------
			// In theory :), we shouldn't have to worry about
			// comments here, as getChar syphons them out...
			curToken = TKN_FSLASH;
			getChar();
			break;
		case '=':
			getChar();
			if (curChar == '=') {
				*tokenp = '=';
				tokenp++;
				curToken = TKN_EQUALEQUAL;
				getChar();
				}
			else
				curToken = TKN_EQUAL;
			break;
		case '<':
			getChar();
			if (curChar == '=') {
				*tokenp = '=';
				tokenp++;
				curToken = TKN_LE;
				getChar();
				}
			else if (curChar == '>') {
				*tokenp = '>';
				tokenp++;
				curToken = TKN_NE;
				getChar();
				}
			else
				curToken = TKN_LT;
			break;
		case '>':
			getChar();
			if (curChar == '=') {
				*tokenp = '=';
				tokenp++;
				curToken = TKN_GE;
				getChar();
				}
			else
				curToken = TKN_GT;
			break;
		case '.':
			curToken = TKN_PERIOD;
			getChar();
			break;
		case ':':
			curToken = TKN_COLON;
			getChar();
			break;
		default:
			curToken = TKN_ERROR;
			getChar();
			break;
	}

	*tokenp = '\0';
}

//***************************************************************************
// TOKEN TESTER routines
//***************************************************************************

bool tokenIn (TokenCodeType* tokenList) {

	if (!tokenList)
		return(false);

	for (TokenCodeType* tp = &tokenList[0]; *tp != TKN_NONE; tp++)
		if (curToken == *tp)
			return(true);
	return(false);
}

//***************************************************************************

void synchronize (TokenCodeType* tokenList1,
				  TokenCodeType* tokenList2,
				  TokenCodeType* tokenList3) {

	bool badLists = (!tokenIn(tokenList1) && !tokenIn(tokenList2) && !tokenIn(tokenList3));

	if (badLists) {
		syntaxError((curToken == TKN_EOF) ? ABL_ERR_SYNTAX_UNEXPECTED_EOF : ABL_ERR_SYNTAX_UNEXPECTED_TOKEN);

		//----------------------------------------------
		// Now, we need to re-sync by skipping tokens...
		while (!tokenIn(tokenList1) &&
			   !tokenIn(tokenList2) &&
			   !tokenIn(tokenList3) &&
			   (curToken != TKN_EOF))
			getToken();
	}
}

//***************************************************************************
// SOURCE FILE routines
//***************************************************************************

bool getSourceLine (void) {

	if (!sourceFile->eof())
	{
		long numChars = sourceFile->readLineEx(reinterpret_cast<unsigned char*>(sourceBuffer), MAXLEN_SOURCELINE);
		if (numChars == 0)
			return(false);
		lineNumber++;
		if (printFlag) 
		{
			char printBuffer[MAXLEN_SOURCELINE + 9];
			mc2::fmt::to_buf(printBuffer, "{:4d} {:d}: {}", mc2::fmt::as_i32(lineNumber), mc2::fmt::as_i32(level), mc2::fmt::safe(sourceBuffer));
			printLine(printBuffer);
		}
		return(true);
	}
	else
		return(false);
}

//---------------------------------------------------------------------------

long openSourceFile (mc2::czstring sourceFileName) {

	if (!sourceFileName)
		return(-1);

	if (NumOpenFiles == MAX_INCLUDE_DEPTH)
		return(-2);

	if (NumSourceFiles == MAX_SOURCE_FILES)
		return(-3);

	if (!sourceFilesOwner || strlen(sourceFileName) >= MAXLEN_FILENAME) {
		MC2_LOG(Sim, Warn, "[ABL_SOURCE_BAD] source path exceeds the scanner capacity");
		return(-3);
	}
	auto owned = std::make_unique<ABLFile>();
	ABLFile* sFile = owned.get();

	if (sFile->open(sourceFileName) != ABL_NO_ERR) {
		return(-3);
	}

	strcpy(SourceFiles[NumSourceFiles], sourceFileName);
	FileNumber = NumSourceFiles++;

	sourceFile = sFile;
	strcpy(openFiles[NumOpenFiles].fileName, sourceFileName);
	openFiles[NumOpenFiles].filePtr = sFile;
	sourceFilesOwner->files[NumOpenFiles] = std::move(owned);
	openFiles[NumOpenFiles].fileNumber = static_cast<unsigned char>(FileNumber);
	openFiles[NumOpenFiles].lineNumber = 0;
	if (NumOpenFiles > 0)
		openFiles[NumOpenFiles - 1].lineNumber = lineNumber;
	NumOpenFiles++;

	lineNumber = 0;
	sourceBuffer[0] = '\0';
	bufferp = sourceBuffer;
	getChar();

	return(ABL_NO_ERR);
}

//---------------------------------------------------------------------------

long closeSourceFile (void) {

	if (NumOpenFiles == 0)
		return(-1);

	--NumOpenFiles;
	sourceFilesOwner->files[NumOpenFiles].reset();
	openFiles[NumOpenFiles].filePtr = nullptr;
	sourceFile = nullptr;

	if (NumOpenFiles > 0) {
		sourceFile = openFiles[NumOpenFiles - 1].filePtr;
		FileNumber = openFiles[NumOpenFiles - 1].fileNumber;
		lineNumber = openFiles[NumOpenFiles - 1].lineNumber;
	}

	return(ABL_NO_ERR);
}

//***************************************************************************
// PRINTOUT routines
//***************************************************************************

void printLine (char* line) {

	if (++lineCount > MAX_LINES_PER_PAGE) {
		printPageHeader();
		lineCount = 1;
	}

	char* saveChPtr = nullptr;
	char saveCh = ' ';
	if (strlen(line) > MAXLEN_PRINTLINE) {
		saveCh = line[MAXLEN_PRINTLINE];
		saveChPtr = &line[MAXLEN_PRINTLINE];
		*saveChPtr = '\0';
	}
	mc2::fmt::print_out("{}", mc2::fmt::safe(line));
	if (saveChPtr)
		*saveChPtr = saveCh;
}

//***************************************************************************

void initPageHeader (char* fileName) {

	//--------------------------
	// Save the source file name
	strncpy(sourceName, fileName, MAXLEN_FILENAME - 1);

	//-------------------------------
	// Save the current time and date
	time_t timer;
	time(&timer);
	strcpy(date, asctime(localtime(&timer)));
}

//***************************************************************************

void printPageHeader (void) {

	//putchar(CHAR_FORMFEED);
	mc2::fmt::print_out("Page {:d}   {}   {}\n\n", mc2::fmt::as_i32(++pageNumber), mc2::fmt::safe(sourceName), mc2::fmt::safe(date));
}

//***************************************************************************

