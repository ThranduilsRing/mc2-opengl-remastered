#ifndef _WINNLS_H
#define _WINNLS_H

#ifdef __cplusplus
extern "C" {
#endif

constexpr auto CP_ACP = 0;
constexpr auto LOCALE_SYSTEM_DEFAULT = 0x800;

constexpr auto CT_CTYPE1 = 1;
constexpr auto CT_CTYPE2 = 2;
constexpr auto CT_CTYPE3 = 4;

constexpr auto C1_UPPER = 1;
constexpr auto C1_LOWER = 2;
constexpr auto C1_DIGIT = 4;
constexpr auto C1_SPACE = 8;
constexpr auto C1_PUNCT = 16;
constexpr auto C1_CNTRL = 32;
constexpr auto C1_BLANK = 64;
constexpr auto C1_XDIGIT = 128;
constexpr auto C1_ALPHA = 256;
constexpr auto C2_LEFTTORIGHT = 1;
constexpr auto C2_RIGHTTOLEFT = 2;
constexpr auto C2_EUROPENUMBER = 3;
constexpr auto C2_EUROPESEPARATOR = 4;
constexpr auto C2_EUROPETERMINATOR = 5;
constexpr auto C2_ARABICNUMBER = 6;
constexpr auto C2_COMMONSEPARATOR = 7;
constexpr auto C2_BLOCKSEPARATOR = 8;
constexpr auto C2_SEGMENTSEPARATOR = 9;
constexpr auto C2_WHITESPACE = 10;
constexpr auto C2_OTHERNEUTRAL = 11;
constexpr auto C2_NOTAPPLICABLE = 0;
constexpr auto C3_NONSPACING = 1;
constexpr auto C3_DIACRITIC = 2;
constexpr auto C3_VOWELMARK = 4;
constexpr auto C3_SYMBOL = 8;
constexpr auto C3_KATAKANA = 16;
constexpr auto C3_HIRAGANA = 32;
constexpr auto C3_HALFWIDTH = 64;
constexpr auto C3_FULLWIDTH = 128;
constexpr auto C3_IDEOGRAPH = 256;
constexpr auto C3_KASHIDA = 512;
constexpr auto C3_LEXICAL = 1024;
constexpr auto C3_ALPHA = 32768;
constexpr auto C3_NOTAPPLICABLE = 0;

int WideCharToMultiByte(UINT,DWORD,LPCWSTR,int,LPSTR,int,LPCSTR,LPBOOL);
int MultiByteToWideChar(UINT,DWORD,LPCSTR,int,LPWSTR,int);

#define GetStringTypeEx GetStringTypeExA
BOOL GetStringTypeExA(LCID,DWORD,LPCSTR,int,LPWORD);
BOOL GetStringTypeExW(LCID,DWORD,LPCWSTR,int,LPWORD);

BOOL IsDBCSLeadByte(BYTE);

#ifdef __cplusplus
}
#endif

#endif
