#ifndef STRING_UTILS_H
#define STRING_UTILS_H
#include "mc2_czstring.h"

void StringFree(char* s);
void StringFree(wchar_t* s);
char* StringDup(mc2::czstring s);
wchar_t* StringDup(const wchar_t* s);
char* StringDupSafe(mc2::czstring s);
wchar_t* StringDupSafe(const wchar_t* s);

/*
char* WStringToString(const wchar_t* wstring, char* string, int stringMaxLen);
char* WStringToString(const wchar_t* wstring);
wchar_t* StringToWString(const char* string, wchar_t* wstring, int wstringMaxLen);
wchar_t* StringToWString(const char* string);
BOOL CopyToStaticString(char *szStaticString, int32 iStaticStringLen, const char *szSource);
*/

const char* StringGetExtention(mc2::czstring pstr);

// aka C99 snprintf
int StringFormat(char* buf, size_t buf_size, const char* format, ...);

#endif // STRING_UTILS_H
