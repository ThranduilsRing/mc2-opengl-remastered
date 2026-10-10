// Shared surface of the inifile.cpp translation units (inifile.cpp, inifile_{read,arrays}.cpp).
//
// inifile.cpp keeps the constructor, the block parser, the text conversions, open / close and the writers; the rest of
// FitIniFile is split by responsibility (see DOC in each piece).
#ifndef INIFILE_INTERNAL_H
#define INIFILE_INTERNAL_H

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#include<stdio.h>
#include<stdlib.h>
#include<math.h>
//#include<string.h>
#include"platform_str.h"
#include<ctype.h>

#include"err.h"

// sebi:
//#ifndef _MBCS
#include<gameos.hpp>

#undef isspace		//Macro Chokes under Intel Compiler!!

// Globals defined in inifile.cpp (or elsewhere in the engine) that the split pieces share.
extern char fitIniHeader[7];
extern char fitIniFooter[7];

#endif // INIFILE_INTERNAL_H
