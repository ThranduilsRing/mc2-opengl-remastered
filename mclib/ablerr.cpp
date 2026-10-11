//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLERR.CPP
//
//***************************************************************************

#include<stdio.h>
#include<stdlib.h>
#include "mc2fmt.h"

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLSCAN_H
#include"ablscan.h"
#endif

#ifndef ABLERR_H
#include"ablerr.h"
#endif

#ifndef ABL_H
#include"abl.h"
#endif

// ABL-OPCODE-BOUNDS-HARDEN gates (defined in ablxstd.cpp)
extern bool s_ablArgGuard;
extern bool s_ablRuntimeSoftfail;

//***************************************************************************

//----------
// EXTERNALS
extern char*		tokenp;
extern int          execLineNumber;
extern int32_t      lineNumber;
extern int32_t      FileNumber;
extern char			SourceFiles[MAX_SOURCE_FILES][MAXLEN_FILENAME];
extern ABLModulePtr	CurModule;
extern char			wordString[];

//---------------------------------------------------------------------------

//----------------------
// SYNTAX ERROR messages

const char* syntaxErrorMessages[] = {
	"No syntax error",										// 0
	"Syntax error",
	"Too many errors",
	"Cannot open source file",
	"Unexpected end-of-file",
	"Invalid number",
	"Invalid fraction",
	"Invalid exponent",
	"Too many digits",
	"Real out of range",
	"Integer out of range",									// 10
	"Missing right parenthesis",
	"Invalid expression",
	"Undefined identifier",
	"Redefined identifier",
	"Unexpected token",
	"Incompatible types",
	"Nesting too deep",
	"Code segment overflow",
	"Missing equal",
	"Missing semi-colon",									// 20
	"Invalid constant",
	"Not a constant identifier",
	"No record types",
	"Missing colon (ouch)",
	"Not a type identifier",
	"Invalid type",
	"Missing end",
	"Invalid identifier usage",
	"Too many subscripts",
	"Missing right bracket",								// 30
	"Incompatible assignment",
	"Missing until",
	"Missing then",
	"Invalid for control",
	"Missing identifier",
	"Missing to",
	"Missing period",
	"Missing module",
	"Missing library",
	"Already forwarded",									// 40
	"Invalid reference parameter",
	"Wrong number of parameters",
	"Missing begin",
	"Missing endvar",
	"No function nesting",
	"Missing code",
	"Missing endif",
	"Missing endwhile",
	"Missing endfor",
	"Missing endfunction",									// 50
	"Missing endmodule",
	"Missing endlibrary",
	"Missing do",
	"Invalid index type",
	"Missing comma",
	"Too many static variables",
	"Missing endcase",
	"Missing endswitch",
	"Missing constant",
	"Bad language directive parameter",						// 60
	"Unknown language directive",
	"Too Many Formal Parameters",
	"Too Many Local Variables",
	"Order must have Integer Return Type",
	"Too many orders",
	"Missing state for transition",
	"Missing endfsm"
};

//---------------------------------------------------------------------------

//-----------------------
// RUNTIME ERROR messages

const char* runtimeErrorMessages[] = {
	"Runtime stack overflow",
	"Infinite Loop",
	"Nested function call",
	"Unimplemented feature",
	"Value out of range",
	"Division by zero",
	"Invalid function argument",
	"Invalid case value",
	"Abort",
	"No Previous State"
};

//---------------------------------------------------------------------------

//--------
// GLOBALS

long	errorCount = 0;

extern DebuggerPtr debugger;

//***************************************************************************

void ABL_Fatal (long errCode, const char* s) {

	ABLFatalCallback(errCode, s);
}

//---------------------------------------------------------------------------

void ABL_Assert (bool test, long errCode, const char* s) {

#ifdef _DEBUG
	if (!test)
		ABLFatalCallback(errCode, s);
#endif
}

//***************************************************************************

void syntaxError (int errCode) {

	// Deduplicate: print only the first occurrence of each (file, line, errCode) triple.
	static long s_lastFile = -1, s_lastLine = -1, s_lastCode = -1;
	bool isDup = (FileNumber == s_lastFile && lineNumber == s_lastLine && errCode == s_lastCode);
	s_lastFile = FileNumber; s_lastLine = lineNumber; s_lastCode = errCode;

	char errMessage[MAXLEN_ERROR_MESSAGE];
	mc2::fmt::to_buf(errMessage, "SYNTAX ERROR {} [line {:d}] - (type {:d}) {} \"{}\"\n", mc2::fmt::safe(SourceFiles[FileNumber]), mc2::fmt::as_i32(lineNumber), mc2::fmt::as_i32(errCode), mc2::fmt::safe(syntaxErrorMessages[errCode]), mc2::fmt::safe(wordString));
	if (!isDup)
		ABL_Fatal(0, errMessage);
	
	*tokenp = '\0';
	errorCount++;
	
	if (errorCount > MAX_SYNTAX_ERRORS) {
		mc2::fmt::to_buf(errMessage, "Way too many syntax errors. ABL aborted.\n");
		ABL_Fatal(0, errMessage);
	}
}

//---------------------------------------------------------------------------

void runtimeError (int errCode) {

	char message[512];

	if (debugger) {
		mc2::fmt::to_buf(message, "RUNTIME ERROR:  [{:d}] {}", mc2::fmt::as_i32(errCode), mc2::fmt::safe(runtimeErrorMessages[errCode]));
		debugger->print(message);
		mc2::fmt::to_buf(message, "MODULE {}", mc2::fmt::safe(CurModule->getName()));
		debugger->print(message);
		if (FileNumber > -1)
			mc2::fmt::to_buf(message, "FILE {}", mc2::fmt::safe(CurModule->getSourceFile(FileNumber)));
		else
			mc2::fmt::to_buf(message, "FILE : unavailable");
		debugger->print(message);
		mc2::fmt::to_buf(message, "LINE {:d}", mc2::fmt::as_i32(execLineNumber));
		debugger->print(message);
		debugger->debugMode();
	}

	mc2::fmt::to_buf(message, "ABL RUNTIME ERROR {} [line {:d}] - (type {:d}) {}\n", mc2::fmt::safe((FileNumber > -1) ? CurModule->getSourceFile(FileNumber) : "unavailable"), mc2::fmt::as_i32(execLineNumber), mc2::fmt::as_i32(errCode), mc2::fmt::safe(runtimeErrorMessages[errCode]));
	if (s_ablArgGuard && s_ablRuntimeSoftfail) {
		// H3: log-and-continue instead of aborting the process.
		char softMsg[512];
		const char* srcFile = (FileNumber > -1) ? CurModule->getSourceFile(FileNumber) : "unavailable";
		mc2::fmt::to_buf(softMsg, "[ABL_SOFTFAIL] func=(runtime) module={} file={} line={:d} errCode={:d} — {}", mc2::fmt::safe(CurModule ? CurModule->getName() : "(unknown)"), mc2::fmt::safe(srcFile), mc2::fmt::as_i32(execLineNumber), mc2::fmt::as_i32(errCode), mc2::fmt::safe(runtimeErrorMessages[errCode]));
		printf("%s\n", softMsg); fflush(stdout);
		return;
	}
	ABL_Fatal(-ABL_ERR_RUNTIME_ABORT, message);
}

//***************************************************************************

