#ifndef _WINNT_H
#define _WINNT_H

#ifdef LINUX_BUILD

#ifdef __cplusplus
extern "C" {
#endif

#include "platform_winerror.h"

#ifndef RC_INVOKED
#include <string.h>

/* FIXME: add more architectures. Is there a way to specify this in GCC? */
#ifdef _X86_
#define UNALIGNED
#else
#define UNALIGNED
#endif

#ifndef DECLSPEC_ALIGN
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#endif

#ifndef FORCEINLINE
#define FORCEINLINE __inline  __attribute__((always_inline))
#endif

#ifndef C_ASSERT
#define C_ASSERT(expr) typedef char __C_ASSERT__[(expr)?1:-1]
#endif

#ifndef VOID
#define VOID void
#endif

using CHAR = char;
using SHORT = short;
using LONG = int32_t;
using CCHAR = char;
using PCCHAR = char*;
using UCHAR = unsigned char;
using PUCHAR = unsigned char*;
using USHORT = unsigned short;
using PUSHORT = unsigned short*;
using ULONG = uint32_t;
using PULONG = ULONG*;
using PSZ = char*;

using PVOID = void*;
using LPVOID = void*;

using HANDLE = void *;

/* FIXME for __WIN64 */
#ifndef  __ptr64
#define __ptr64
#endif
using PVOID64 = void* __ptr64;
/*
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
#ifndef _WCHAR_T_
#define _WCHAR_T_
#undef __need_wchar_t
#ifndef __cplusplus
typedef unsigned short wchar_t;
#endif
#endif
#endif
*/

using WCHAR = wchar_t;
using PWCHAR = WCHAR*;
using LPWCH = WCHAR*;
using PWCH = WCHAR*;
using NWPSTR = WCHAR*;
using LPWSTR = WCHAR*;
using PWSTR = WCHAR*;
using LPCWCH = CONST WCHAR*;
using PCWCH = CONST WCHAR*;
using LPCWSTR = CONST WCHAR*;
using PCWSTR = CONST WCHAR*;
using PCHAR = CHAR*;
using LPCH = CHAR*;
using PCH = CHAR*;
using NPSTR = CHAR*;
using LPSTR = CHAR*;
using PSTR = CHAR*;
using LPCCH = CONST CHAR*;
using PCSTR = CONST CHAR*;
using LPCSTR = CONST CHAR*;

#ifndef _TCHAR_DEFINED
#define _TCHAR_DEFINED

/* Cannot use __AW(). */
#ifdef UNICODE
/*
 * NOTE: This tests UNICODE, which is different from the _UNICODE define
 *       used to differentiate standard C runtime calls.
 */
using TCHAR = WCHAR;
using _TCHAR = WCHAR;
/*
 * __TEXT is a private macro whose specific use is to force the expansion of a
 * macro passed as an argument to the macro TEXT.  DO NOT use this
 * macro within your programs.  It's name and function could change without
 * notice.
 */
#define __TEXT(q) L##q
#else
using TCHAR = CHAR;
using _TCHAR = CHAR;
#define __TEXT(q) q
#endif

#endif

using TBYTE = TCHAR;
using PTCH = TCHAR*;
using PTBYTE = TCHAR*;
using LPTCH = TCHAR*;
using PTSTR = TCHAR*;
using LPTSTR = TCHAR*;
using LP = TCHAR*;
using PTCHAR = TCHAR*;
using LPCTSTR = const TCHAR*;

/*
 * UNICODE a constant string when UNICODE is defined, else returns the string
 * unmodified.
 * The corresponding macros  _TEXT() and _T() for mapping _UNICODE strings
 * passed to C runtime functions are defined in mingw/tchar.h
 */
#define TEXT(q) __TEXT(q)    
#define _TEXT(q) __TEXT(q) // sebi
using PSHORT = SHORT*;
using PLONG = LONG*;

#ifdef STRICT
typedef void *HANDLE;
#define DECLARE_HANDLE(n) typedef struct n##__{int i;}*n
#else

using HANDLE = PVOID;
#define DECLARE_HANDLE(n) typedef HANDLE n
#endif

using PHANDLE = HANDLE*;
using LPHANDLE = HANDLE*;
using LCID = DWORD;
using PLCID = PDWORD;
using LANGID = WORD;

#define _HAVE_INT64
#define _INTEGRAL_MAX_BITS 64
#undef __int64
#define __int64 long long

using _int64 = __int64; //sebi

using LONGLONG = __int64;
using DWORDLONG = unsigned __int64;

using PLONGLONG = LONGLONG*;
using PDWORDLONG = DWORDLONG*;
using ULONGLONG = DWORDLONG;
using PULONGLONG = DWORDLONG*;
using USN = LONGLONG;

#define Int32x32To64(a,b) ((LONGLONG)(a)*(LONGLONG)(b))
#define UInt32x32To64(a,b) ((DWORDLONG)(a)*(DWORDLONG)(b))
#define Int64ShllMod32(a,b) ((DWORDLONG)(a)<<(b))
#define Int64ShraMod32(a,b) ((LONGLONG)(a)>>(b))
#define Int64ShrlMod32(a,b) ((DWORDLONG)(a)>>(b))

#define ANSI_NULL '\0'
#define UNICODE_NULL L'\0'
using BOOLEAN = BYTE;
using PBOOLEAN = BYTE*;
#endif
using FCHAR = BYTE;
using FSHORT = WORD;
using FLONG = DWORD;

#define NTAPI __stdcall
#include "platform_basetsd.h"

#define FILE_ATTRIBUTE_READONLY			0x00000001
#define FILE_ATTRIBUTE_HIDDEN			0x00000002
#define FILE_ATTRIBUTE_SYSTEM			0x00000004
#define FILE_ATTRIBUTE_DIRECTORY		0x00000010
#define FILE_ATTRIBUTE_ARCHIVE			0x00000020
#define FILE_ATTRIBUTE_DEVICE			0x00000040
#define FILE_ATTRIBUTE_NORMAL			0x00000080
#define FILE_ATTRIBUTE_TEMPORARY		0x00000100
#define FILE_ATTRIBUTE_SPARSE_FILE		0x00000200
#define FILE_ATTRIBUTE_REPARSE_POINT		0x00000400
#define FILE_ATTRIBUTE_COMPRESSED		0x00000800
#define FILE_ATTRIBUTE_OFFLINE			0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED	0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED		0x00004000
#define FILE_ATTRIBUTE_VIRTUAL			0x00010000
#define FILE_ATTRIBUTE_VALID_FLAGS		0x00017fb7
#define FILE_ATTRIBUTE_VALID_SET_FLAGS		0x000031a7

#define PAGE_NOACCESS	0x0001
#define PAGE_READONLY	0x0002
#define PAGE_READWRITE	0x0004
#define PAGE_WRITECOPY	0x0008
#define PAGE_EXECUTE	0x0010
#define PAGE_EXECUTE_READ	0x0020
#define PAGE_EXECUTE_READWRITE	0x0040
#define PAGE_EXECUTE_WRITECOPY	0x0080
#define PAGE_GUARD		0x0100
#define PAGE_NOCACHE		0x0200
#define PAGE_WRITECOMBINE 0x0400
#define MEM_COMMIT           0x1000
#define MEM_RESERVE          0x2000
#define MEM_DECOMMIT         0x4000
#define MEM_RELEASE          0x8000
#define MEM_FREE            0x10000
#define MEM_PRIVATE         0x20000
#define MEM_MAPPED          0x40000
#define MEM_RESET           0x80000
#define MEM_TOP_DOWN       0x100000
#define MEM_WRITE_WATCH	   0x200000 /* 98/Me */
#define MEM_PHYSICAL	   0x400000
#define MEM_4MB_PAGES    0x80000000

struct _SECURITY_ATTRIBUTES {
	DWORD nLength;
	LPVOID lpSecurityDescriptor;
	BOOL bInheritHandle;
};
using SECURITY_ATTRIBUTES = _SECURITY_ATTRIBUTES;
using PSECURITY_ATTRIBUTES = _SECURITY_ATTRIBUTES*;
using LPSECURITY_ATTRIBUTES = _SECURITY_ATTRIBUTES*;


#ifdef __cplusplus
}
#endif


#endif // LINUX_BUILD

#endif
