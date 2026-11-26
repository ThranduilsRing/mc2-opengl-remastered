//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLERR.CPP
//
//***************************************************************************

#include "mc2_czstring.h"
#include<stdio.h>
#include<stdlib.h>
#include<utility>
#include<stdexcept>
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
extern int32_t      lineNumber;
extern int32_t      FileNumber;
extern char			SourceFiles[MAX_SOURCE_FILES][MAXLEN_FILENAME];
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

void ABL_Fatal (long errCode, mc2::czstring s) {

	ABLFatalCallback(errCode, s);
	throw ABLRuntimeFailure(s ? s : "ABL fatal error");
}

//---------------------------------------------------------------------------

void ABL_Assert (bool test, long errCode, mc2::czstring s) {

#ifdef _DEBUG
	if (!test)
		ABLFatalCallback(errCode, s);
#endif
}

//***************************************************************************

[[noreturn]] void syntaxError (SyntaxErrorType errCode) {
	int code = std::to_underlying(errCode);
	if (code < 0 || code >= std::to_underlying(SyntaxErrorType::NUM_ABL_SYNTAX_ERRORS))
		code = std::to_underlying(SyntaxErrorType::ABL_ERR_SYNTAX_UNEXPECTED_TOKEN);
	mc2::fmt::print_err("SYNTAX ERROR {} [line {:d}] - (type {:d}) {}\n",
		mc2::fmt::safe(SourceFiles[FileNumber]), mc2::fmt::as_i32(lineNumber),
		mc2::fmt::as_i32(code), mc2::fmt::safe(syntaxErrorMessages[code]));
	errorCount = 1;
	throw ABLSyntaxFailure{};
}

//---------------------------------------------------------------------------

void runtimeError (RuntimeErrorType errCode) {

	char message[512];
	int code = std::to_underlying(errCode);

	if (debugger) {
		mc2::fmt::to_buf(message, "RUNTIME ERROR:  [{:d}] {}", mc2::fmt::as_i32(code), mc2::fmt::safe(runtimeErrorMessages[code]));
		debugger->print(message);
		mc2::fmt::to_buf(message, "MODULE {}", mc2::fmt::safe(g_ablExec->CurModule->getName()));
		debugger->print(message);
		if (FileNumber > -1)
			mc2::fmt::to_buf(message, "FILE {}", mc2::fmt::safe(g_ablExec->CurModule->getSourceFile(FileNumber)));
		else
			mc2::fmt::to_buf(message, "FILE : unavailable");
		debugger->print(message);
		mc2::fmt::to_buf(message, "LINE {:d}", mc2::fmt::as_i32(g_ablExec->execLineNumber));
		debugger->print(message);
		debugger->debugMode();
	}

	mc2::fmt::to_buf(message, "ABL RUNTIME ERROR {} [line {:d}] - (type {:d}) {}\n", mc2::fmt::safe((FileNumber > -1) ? g_ablExec->CurModule->getSourceFile(FileNumber) : "unavailable"), mc2::fmt::as_i32(g_ablExec->execLineNumber), mc2::fmt::as_i32(code), mc2::fmt::safe(runtimeErrorMessages[code]));
	if (s_ablArgGuard && s_ablRuntimeSoftfail) {
		// Keep the diagnostic, then unwind just as the normal fatal path does.
		char softMsg[512];
		const char* srcFile = (FileNumber > -1) ? g_ablExec->CurModule->getSourceFile(FileNumber) : "unavailable";
		mc2::fmt::to_buf(softMsg, "[ABL_SOFTFAIL] func=(runtime) module={} file={} line={:d} errCode={:d} — {}", mc2::fmt::safe(g_ablExec->CurModule ? g_ablExec->CurModule->getName() : "(unknown)"), mc2::fmt::safe(srcFile), mc2::fmt::as_i32(g_ablExec->execLineNumber), mc2::fmt::as_i32(code), mc2::fmt::safe(runtimeErrorMessages[code]));
		mc2::fmt::print_out("{}\n", mc2::fmt::safe(softMsg)); fflush(stdout);

	}
	else
		ABL_Fatal(-std::to_underlying(RuntimeErrorType::ABL_ERR_RUNTIME_ABORT), message);
	// A failed instruction must never resume at the same bytecode token.
	throw ABLRuntimeFailure(message);
}

//***************************************************************************

