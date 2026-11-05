#ifndef _WINDEF_H
#define _WINDEF_H
#include <stdint.h>

// Keep the platform macros usable from C while naming C++ conversions.
#ifdef __cplusplus
#define MC2_PLATFORM_STATIC_CAST(type, value) static_cast<type>(value)
#else
#define MC2_PLATFORM_STATIC_CAST(type, value) ((type)(value))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FAR
#define far
#define NEAR
#define near

#ifndef CONST
#define CONST const
#endif

#undef MAX_PATH
#define MAX_PATH 260

#ifndef NULL

#ifdef __cplusplus
#define NULL 0
#else
#define NULL (MC2_PLATFORM_STATIC_CAST(void*, 0))
#endif

#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#define CDECL //_cdecl
#define STDCALL //__stdcall
#define FASTCALL //__fastcall
#define WINAPI //__stdcall
#define WINAPIV //__cdecl
#define APIENTRY //__stdcall
#define CALLBACK //__stdcall
#define APIPRIVATE //__stdcall

#define DECLSPEC_IMPORT __declspec(dllimport)
#define DECLSPEC_EXPORT __declspec(dllexport)
#define DECLSPEC_NORETURN __declspec(noreturn)
#define DECLARE_STDCALL_P( type ) __stdcall type

#define MAKEWORD(a,b)	(MC2_PLATFORM_STATIC_CAST(WORD, ((MC2_PLATFORM_STATIC_CAST(BYTE, (((DWORD_PTR)(a))&0xFF)))|((MC2_PLATFORM_STATIC_CAST(WORD, (MC2_PLATFORM_STATIC_CAST(BYTE, (((DWORD_PTR)(b))&0xFF)))))<<8))))
#define MAKELONG(a,b)	(MC2_PLATFORM_STATIC_CAST(LONG, ((MC2_PLATFORM_STATIC_CAST(WORD, (((DWORD_PTR)(a))&0xFFFF)))|((MC2_PLATFORM_STATIC_CAST(DWORD, (MC2_PLATFORM_STATIC_CAST(WORD, (((DWORD_PTR)(b))&0xFFFF)))))<<16))))
#define LOWORD(l)	(MC2_PLATFORM_STATIC_CAST(WORD, (((DWORD_PTR)(l))&0xFFFF)))
#define HIWORD(l)	(MC2_PLATFORM_STATIC_CAST(WORD, (((DWORD_PTR)(l)>>16)&0xFFFF)))
#define LOBYTE(w)	(MC2_PLATFORM_STATIC_CAST(BYTE, (((DWORD_PTR)(w))&0xFF)))
#define HIBYTE(w)	(MC2_PLATFORM_STATIC_CAST(BYTE, ((((DWORD_PTR)(w))>>8)&0xFF)))

// !NB sebi, DWORD on Windows is always 32bit!!!    
//typedef unsigned long DWORD;
using DWORD = unsigned int;
using WINBOOL = int;
using PWINBOOL = int*;
using LPWINBOOL = int*;
/* FIXME: Is there a good solution to this? */
#define BOOL WINBOOL
using BYTE = unsigned char;
using PBOOL = BOOL*;
using LPBOOL = BOOL*;
using WORD = unsigned short;
using FLOAT = float;
using PFLOAT = FLOAT*;
using PBYTE = BYTE*;
using LPBYTE = BYTE*;
using PINT = int*;
using LPINT = int*;
using PWORD = WORD*;
using LPWORD = WORD*;
using LPLONG = int32_t*;
using PDWORD = DWORD*;
using LPDWORD = DWORD*;
using PCVOID = CONST void*;
using LPCVOID = CONST void*;
using INT = int;
using UINT = unsigned int;
using PUINT = unsigned int*;
using LPUINT = unsigned int*;
using DOUBLE = double; // sebi, actually in wtypes.h

#ifndef NOMINMAX

#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

#endif

#include "platform_winnt.h"

using WPARAM = UINT_PTR;
using LPARAM = LONG_PTR;
using LRESULT = LONG_PTR;

#ifndef _HRESULT_DEFINED
using HRESULT = LONG;
#define _HRESULT_DEFINED
#endif

using ATOM = WORD;
using HHOOK = HANDLE;
using HGLOBAL = HANDLE;
using HLOCAL = HANDLE;
using GLOBALHANDLE = HANDLE;
using LOCALHANDLE = HANDLE;
using HGDIOBJ = void*;
DECLARE_HANDLE(HACCEL);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HCOLORSPACE);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HDESK);
DECLARE_HANDLE(HENHMETAFILE);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HKEY);
/* FIXME: How to handle these. SM_CMONITORS etc in winuser.h also. */
DECLARE_HANDLE(HMONITOR);
constexpr auto HMONITOR_DECLARED = 1;
DECLARE_HANDLE(HTERMINAL);
DECLARE_HANDLE(HWINEVENTHOOK);

using PHKEY = HKEY*;
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HMETAFILE);
DECLARE_HANDLE(HINSTANCE);
using HMODULE = HINSTANCE;
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HRSRC);
DECLARE_HANDLE(HSTR);
DECLARE_HANDLE(HTASK);
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HWINSTA);
DECLARE_HANDLE(HKL);
using HFILE = int;
using HCURSOR = HICON;
using COLORREF = DWORD;
using FARPROC = int (WINAPI*)();
using NEARPROC = int (WINAPI*)();
using PROC = int (WINAPI*)();
struct tagRECT {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
};
using RECT = tagRECT;
using PRECT = tagRECT*;
using LPRECT = tagRECT*;
using LPCRECT = const RECT*;
struct tagRECTL {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
};
using RECTL = tagRECTL;
using PRECTL = tagRECTL*;
using LPRECTL = tagRECTL*;
using LPCRECTL = const RECTL*;
struct tagPOINT {
	LONG x;
	LONG y;
};
using POINT = tagPOINT;
using POINTL = tagPOINT;
using PPOINT = tagPOINT*;
using LPPOINT = tagPOINT*;
using PPOINTL = tagPOINT*;
using LPPOINTL = tagPOINT*;
struct tagSIZE {
	LONG cx;
	LONG cy;
};
using SIZE = tagSIZE;
using SIZEL = tagSIZE;
using PSIZE = tagSIZE*;
using LPSIZE = tagSIZE*;
using PSIZEL = tagSIZE*;
using LPSIZEL = tagSIZE*;
struct tagPOINTS {
	SHORT x;
	SHORT y;
};
using POINTS = tagPOINTS;
using PPOINTS = tagPOINTS*;
using LPPOINTS = tagPOINTS*;

#ifdef __cplusplus
}
#endif

#endif
