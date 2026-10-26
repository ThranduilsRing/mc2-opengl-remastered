#ifndef _BASETSD_H
#define _BASETSD_H
#include <stdint.h>

#ifndef __int64
#define __int64 long long
#endif

#ifdef __cplusplus
extern "C" {
#endif
using INT8 = signed char;
using INT16 = signed short;
using LONG32 = int;
using PLONG32 = int*;
using INT32 = int;
using PINT32 = int*;
using UINT8 = unsigned char;
using UINT16 = unsigned short;
using ULONG32 = unsigned int;
using PULONG32 = unsigned int*;
using DWORD32 = unsigned int;
using PDWORD32 = unsigned int*;
using UINT32 = unsigned int;
using PUINT32 = unsigned int*;

using INT_PTR = intptr_t;
using PINT_PTR = INT_PTR*;
using UINT_PTR = uintptr_t;
using PUINT_PTR = UINT_PTR*;
using LONG_PTR = intptr_t;
using PLONG_PTR = LONG_PTR*;
using ULONG_PTR = uintptr_t;
using PULONG_PTR = ULONG_PTR*;
using UHALF_PTR = unsigned short;
using PUHALF_PTR = unsigned short*;
using HALF_PTR = short;
using PHALF_PTR = short*;
using HANDLE_PTR = uintptr_t;

using SIZE_T = ULONG_PTR;
using PSIZE_T = ULONG_PTR*;
using SSIZE_T = LONG_PTR;
using PSSIZE_T = LONG_PTR*;
using DWORD_PTR = ULONG_PTR;
using PDWORD_PTR = ULONG_PTR*;
using LONG64 = __int64;
using PLONG64 = __int64*;
using INT64 = __int64;
using PINT64 = __int64*;
using ULONG64 = unsigned __int64;
using PULONG64 = unsigned __int64*;
using DWORD64 = unsigned __int64;
using PDWORD64 = unsigned __int64*;
using UINT64 = unsigned __int64;
using PUINT64 = unsigned __int64*;
#ifdef __cplusplus
}
#endif

#endif /* _BASETSD_H */
