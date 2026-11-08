// Shared surface of the abldbug.cpp translation units (abldbug.cpp, abldbug_print.cpp).
//
// abldbug.cpp keeps the watch and breakpoint managers, the Debugger setup and the command loop; the value printing,
// execution tracing and watch/assign helpers are split out (see DOC in the piece).
#ifndef ABLDBUG_INTERNAL_H
#define ABLDBUG_INTERNAL_H

#include<stdlib.h>
#include<string.h>

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

#ifndef ABLEXEC_H
#include"ablexec.h"
#endif

#ifndef ABLDBUG_H
#include"abldbug.h"
#endif

// Globals defined in abldbug.cpp (or elsewhere in the engine) that the split pieces share.
extern int32_t          lineNumber;
extern char*			codeBuffer;
extern char*			codeBufferPtr;
extern SymTableNodePtr	symTableDisplay[];
extern long				errorCount;
extern char				curChar;
extern TokenCodeType	curToken;
extern Literal			curLiteral;
extern long				bufferOffset;
extern char				sourceBuffer[MAXLEN_SOURCELINE];
extern char*			bufferp;
extern char				wordString[MAXLEN_TOKENSTRING];
extern TypePtr			IntegerTypePtr;
extern TypePtr			CharTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;
extern Type				DummyType;
extern ModuleEntryPtr	ModuleRegistry;
extern ABLModulePtr*	ModuleInstanceRegistry;
extern int32_t				MaxModules;
extern int32_t				NumModulesRegistered;
extern int32_t				NumModuleInstances;
extern int32_t				MaxWatchesPerModule;
extern int32_t				MaxBreakPointsPerModule;
extern const char*			TokenStrings[std::to_underlying(TokenCodeType::NUM_TOKENS)];
extern void CheckMouse(void);
extern long MaxBreaks;
extern long MaxWatches;
extern DebuggerPtr debugger;
extern bool			takeScreenShot;

#endif // ABLDBUG_INTERNAL_H
