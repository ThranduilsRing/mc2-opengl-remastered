#ifndef _WINBASE_H
#define _WINBASE_H

#ifdef __cplusplus
extern "C" {
#endif

#define WINBASEAPI    

union LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; }; int64_t QuadPart; };
union ULARGE_INTEGER { struct { DWORD LowPart; DWORD HighPart; }; uint64_t QuadPart; };
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
BOOL QueryPerformanceCounter(LARGE_INTEGER*);
BOOL QueryPerformanceFrequency(LARGE_INTEGER*);
DWORD GetCurrentThreadId();
DWORD GetCurrentProcessId();
void Sleep(DWORD);
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
BOOL MoveFileExW(LPCWSTR, LPCWSTR, DWORD);
inline LONG InterlockedIncrement(volatile LONG* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchange(volatile LONG* value, LONG next) { return __atomic_exchange_n(value, next, __ATOMIC_SEQ_CST); }
inline LONG InterlockedCompareExchange(volatile LONG* value, LONG next, LONG expected) {
    __atomic_compare_exchange_n(value, &expected, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
HANDLE CreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL ReadFile(HANDLE, LPVOID, DWORD, DWORD*, LPVOID);
BOOL WriteFile(HANDLE, LPCVOID, DWORD, DWORD*, LPVOID);
BOOL CloseHandle(HANDLE);
DWORD GetFileAttributesA(LPCSTR);
DWORD GetFullPathNameA(LPCSTR, DWORD, LPSTR, LPSTR*);
DWORD GetModuleFileNameA(HANDLE, LPSTR, DWORD);
DWORD GetModuleFileNameW(HANDLE, LPWSTR, DWORD);

#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2

#define INVALID_HANDLE_VALUE (HANDLE)-1

struct _SYSTEMTIME {
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
};
using SYSTEMTIME = _SYSTEMTIME;
using LPSYSTEMTIME = _SYSTEMTIME*;

struct _TIME_ZONE_INFORMATION {
	LONG Bias;
	WCHAR StandardName[32];
	SYSTEMTIME StandardDate;
	LONG StandardBias;
	WCHAR DaylightName[32];
	SYSTEMTIME DaylightDate;
	LONG DaylightBias;
};
using TIME_ZONE_INFORMATION = _TIME_ZONE_INFORMATION;
using LPTIME_ZONE_INFORMATION = _TIME_ZONE_INFORMATION*;

struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
};
using FILETIME = _FILETIME;
using PFILETIME = _FILETIME*;
using LPFILETIME = _FILETIME*;

struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };
BOOL GetFileAttributesExA(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;

#ifdef _WIN32_WCE
    DWORD dwOID;
#else
	DWORD dwReserved0;
	DWORD dwReserved1;
#endif

	CHAR cFileName[MAX_PATH];

#ifndef _WIN32_WCE
	CHAR cAlternateFileName[14];
#endif

};
using WIN32_FIND_DATAA = _WIN32_FIND_DATAA;
using PWIN32_FIND_DATAA = _WIN32_FIND_DATAA*;
using LPWIN32_FIND_DATAA = _WIN32_FIND_DATAA*;

#define WIN32_FIND_DATA WIN32_FIND_DATAA

DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION);
VOID WINAPI GetSystemTime(LPSYSTEMTIME);

#define RtlMoveMemory memmove
#define RtlCopyMemory memcpy
#define RtlFillMemory(d,l,f) memset((d), (f), (l))
#define RtlZeroMemory(d,l) RtlFillMemory((d),(l),0)
#define MoveMemory RtlMoveMemory
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory

BOOL CreateDirectory(LPCSTR,LPSECURITY_ATTRIBUTES);
BOOL SetFileAttributes(LPCSTR,DWORD);
BOOL DeleteFile(LPCSTR);
BOOL CopyFile(LPCTSTR, LPCTSTR, BOOL);

int lstrlen(LPCSTR);
int lstrlenA(LPCSTR);

#define lstrcmpi lstrcmpiA

int lstrcmpiA(LPCSTR,LPCSTR);
int lstrcmpiW(LPCWSTR,LPCWSTR);


WINBASEAPI HANDLE WINAPI FindFirstFileA(LPCSTR,LPWIN32_FIND_DATAA);
//WINBASEAPI HANDLE WINAPI FindFirstFileW(LPCWSTR,LPWIN32_FIND_DATAW);
#define FindFirstFile FindFirstFileA

WINBASEAPI BOOL WINAPI FindNextFileA(HANDLE,LPWIN32_FIND_DATAA);
//WINBASEAPI BOOL WINAPI FindNextFileW(HANDLE,LPWIN32_FIND_DATAW);
#define FindNextFile FindNextFileA

BOOL WINAPI FindClose(HANDLE);

WINBASEAPI PVOID WINAPI VirtualAlloc(PVOID,DWORD,DWORD,DWORD);
WINBASEAPI BOOL WINAPI VirtualFree(PVOID,DWORD,DWORD);

WINBASEAPI DWORD WINAPI GetLastError(void);

#ifdef __cplusplus
}
#endif

#endif /* _WINBASE_H */
