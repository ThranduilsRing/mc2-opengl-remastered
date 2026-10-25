#ifndef PLATFORM_WINDOWS

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <utime.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdint.h>
#include <libgen.h> // dirname
#include <stdlib.h> // free
#include <stdio.h> // free
#include<wchar.h> // wcscpy

#include"platform_windows.h"
#include"platform_str.h"
#include "mc2fmt.h"
#include <filesystem>
#include <cstring>
#include <sys/syscall.h>

static thread_local int gGetLastError = 0;

BOOL QueryPerformanceCounter(LARGE_INTEGER* value)
{
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return FALSE;
    value->QuadPart = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* value) { value->QuadPart = 1000000000; return TRUE; }
DWORD GetCurrentThreadId() { return static_cast<DWORD>(syscall(SYS_gettid)); }
DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
ULONGLONG GetTickCount64()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ULONGLONG(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
void Sleep(DWORD ms) { timespec ts{ms / 1000, long(ms % 1000) * 1000000}; while (nanosleep(&ts, &ts) && errno == EINTR) {} }

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE)
{
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    const int fd = mc2_open(path, flags, 0666);
    if (fd < 0) { gGetLastError = errno; return INVALID_HANDLE_VALUE; }
    return reinterpret_cast<HANDLE>(intptr_t(fd) + 1);
}
BOOL ReadFile(HANDLE file, LPVOID data, DWORD size, DWORD* count, LPVOID)
{
    ssize_t result;
    do { result = read(int(intptr_t(file) - 1), data, size); } while (result < 0 && errno == EINTR);
    if (count) *count = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}
BOOL WriteFile(HANDLE file, LPCVOID data, DWORD size, DWORD* count, LPVOID)
{
    const char* bytes = static_cast<const char*>(data);
    DWORD written = 0;
    while (written < size) {
        const auto n = write(int(intptr_t(file) - 1), bytes + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (count) *count = written; return FALSE; }
        written += static_cast<DWORD>(n);
    }
    if (count) *count = written;
    return TRUE;
}
BOOL CloseHandle(HANDLE file) { return close(int(intptr_t(file) - 1)) == 0; }
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    std::error_code error;
    const std::filesystem::path destination(to);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && std::filesystem::exists(destination, error)) return FALSE;
    std::filesystem::rename(std::filesystem::path(from), destination, error);
    if (error) return FALSE;
    if (flags & MOVEFILE_WRITE_THROUGH) {
        const int fd = open(destination.c_str(), O_RDONLY);
        if (fd < 0) return FALSE;
        const bool flushed = fsync(fd) == 0;
        close(fd);
        if (!flushed) return FALSE;
    }
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR path)
{
    struct stat st{};
    if (mc2_stat(path, &st)) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS, LPVOID output)
{
    struct stat st{};
    if (mc2_stat(path, &st)) return FALSE;
    auto& out = *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(output);
    out = {};
    out.dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    auto time = [](timespec ts) { const uint64_t t = (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000 + ts.tv_nsec / 100; return FILETIME{DWORD(t), DWORD(t >> 32)}; };
    out.ftCreationTime = time(st.st_ctim);
    out.ftLastAccessTime = time(st.st_atim);
    out.ftLastWriteTime = time(st.st_mtim);
    out.nFileSizeLow = DWORD(st.st_size);
    out.nFileSizeHigh = DWORD(uint64_t(st.st_size) >> 32);
    return TRUE;
}
DWORD GetFullPathNameA(LPCSTR path, DWORD capacity, LPSTR out, LPSTR* filePart)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal().string();
    if (error) return 0;
    if (capacity <= absolute.size()) return DWORD(absolute.size() + 1);
    std::memcpy(out, absolute.c_str(), absolute.size() + 1);
    if (filePart) { auto* slash = std::strrchr(out, '/'); *filePart = slash ? slash + 1 : out; }
    return DWORD(absolute.size());
}
DWORD GetModuleFileNameA(HANDLE, LPSTR out, DWORD capacity)
{
    if (!capacity) return 0;
    const auto count = readlink("/proc/self/exe", out, capacity - 1);
    if (count < 0) return 0;
    out[count] = 0;
    return DWORD(count);
}
DWORD GetModuleFileNameW(HANDLE module, LPWSTR out, DWORD capacity)
{
    char path[4096];
    if (!capacity || !GetModuleFileNameA(module, path, sizeof(path))) return 0;
    const auto count = mbstowcs(out, path, capacity - 1);
    if (count == size_t(-1)) return 0;
    out[count] = 0;
    return DWORD(count);
}

DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tzi)
{

    const wchar_t* tzn = L"EST";
    wcscpy(tzi -> StandardName, tzn);
    tzi -> StandardBias = 0;
    GetSystemTime(&tzi -> StandardDate);

    struct timeval tv;
    struct timezone tz;
    gettimeofday(&tv, &tz);
    tzi -> Bias = tz.tz_minuteswest; // sebi !NB check this

    return TIME_ZONE_ID_STANDARD;

}

VOID WINAPI GetSystemTime(LPSYSTEMTIME t)
{
    time_t tim = time(nullptr);
    struct tm* gm_time_data = gmtime(&tim);

    t -> wYear = 1900 + gm_time_data -> tm_year;
    t -> wMonth = 1 + gm_time_data -> tm_mon;
    t -> wDayOfWeek = gm_time_data -> tm_wday;
    t -> wDay = gm_time_data -> tm_mday;
    t -> wHour = gm_time_data -> tm_hour;
    t -> wMinute = gm_time_data -> tm_min;
    t -> wSecond = gm_time_data -> tm_sec;
    t -> wMilliseconds = 0;
}

BOOL CreateDirectory(LPCSTR dir, LPSECURITY_ATTRIBUTES attr)
{
    int rv = mkdir(dir, S_IRWXU);
    if (rv) {
        gGetLastError = errno;
        switch (gGetLastError)
        {
        case EEXIST:
            gGetLastError = ERROR_ALREADY_EXISTS;
            break;
        case ENOENT:
            gGetLastError = ERROR_PATH_NOT_FOUND;
            break;
        }
    }
    return rv==0;
}

BOOL SetFileAttributes(LPCSTR str, DWORD attr_bits)
{
    return TRUE; // sebi: does this means anything for Linux ?
}

BOOL DeleteFile(LPCSTR file)
{
    return 0 == unlink(file);
}

BOOL CopyFile(  LPCTSTR lpExistingFileName,
                LPCTSTR lpNewFileName,
                BOOL    bFailIfExists)
{
    int src = open(lpExistingFileName, O_RDONLY);
    if(src == -1) {
        return FALSE;
    }

    /* rw-rw-rw- */
    int permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;      

    int dst_flags = O_CREAT | O_WRONLY | O_TRUNC;
    if(bFailIfExists)
        dst_flags |= O_EXCL;

    int dst = open(lpNewFileName, dst_flags, permissions);
    if(dst == -1) {
        return FALSE;
    }

    const int BUF_SIZE = 1024;
    char buf[BUF_SIZE];
    int num_read = 0;

    bool error = false;
    while ((num_read = read(src, buf, BUF_SIZE)) > 0) {
        if(write(dst, buf, num_read) != num_read) {
            error = true;
            break;
        }
    }


    if (num_read != -1 && !error)
    {
        struct stat file_info = {0};
        fstat(src, &file_info);

        struct utimbuf ut;
        ut.actime = file_info.st_atim.tv_sec;
        ut.modtime = file_info.st_mtim.tv_sec;
        utime(lpExistingFileName, &ut);
    }

    close(src);
    close(dst);

    if (num_read == -1 || error)
        return FALSE;

    return TRUE;
}

int lstrlen(LPCSTR str)
{
    return strlen(str);
}

int lstrlenA(LPCSTR str)
{
    return lstrlen(str);
}

int lstrcmpiA(LPCSTR str1, LPCSTR str2)
{
    return strcasecmp(str1, str2);
}
int lstrcmpiW(LPCWSTR str1, LPCWSTR str2)
{
    return wcscasecmp(str1, str2);
}

WINBASEAPI PVOID WINAPI VirtualAlloc(PVOID lpAddress, DWORD dwSize, DWORD flAllocationType, DWORD flProtect)
{
    if(flAllocationType == MEM_RESERVE)
    {       
        // to RESERVE memory in Linux, use mmap with a private, anonymous, non-accessible mapping.
        // The following line reserves 1gb of ram starting at 0x10000000.
        void* result = mmap(0, dwSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if(result == MAP_FAILED)
            gGetLastError = errno;

        return result == MAP_FAILED ? 0 : result;
    }
    else if(flAllocationType == MEM_COMMIT)
    {
        void* memptr;
        if(lpAddress == 0) {
            memptr = mmap(0, dwSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if(memptr == MAP_FAILED)
                gGetLastError = errno;

            if(memptr == MAP_FAILED)
               return 0;

            lpAddress = memptr;
        }

        assert(lpAddress!=0);

        // to COMMIT memory in Linux, use mprotect on the range of memory you'd like to commit, and
        // grant the memory READ and/or WRITE access.
        // The following line commits 1mb of the buffer.  It will return -1 on out of memory errors.
        //
        int flags = 0;

        if(flProtect == PAGE_READONLY)
            flags = PROT_READ;
        else if(flProtect == PAGE_READWRITE)
            flags = PROT_READ | PROT_WRITE;

        int result = mprotect(static_cast<void*>(lpAddress), dwSize, flags);
        if(result == -1)
        {
            gGetLastError = errno;
            return nullptr;
        }
        return lpAddress;
    }
    else
    {
        assert(0 && "VirtualAlloc: unsupported allocation type");
    }
}


WINBASEAPI BOOL WINAPI VirtualFree(PVOID lpAddress, DWORD dwSize, DWORD dwFreeType)
{
    //assert(dwFreeType == MEM_RELEASE);
    if(dwFreeType == MEM_RELEASE) {

        int rv = munmap(lpAddress, dwSize);
        if(rv == -1) {
            gGetLastError = errno;
            return FALSE;
        }
        return TRUE;
    } 
    /*
    else if(deFreeType == MEM_DECOMMIT) {

        // http://blog.nervus.org/managing-virtual-address-spaces-with-mmap/
        // instead of unmapping the address, we're just gonna trick 
        // the TLB to mark this as a new mapped area which, due to 
        // demand paging, will not be committed until used.

        mmap(lpAddress, dwSize, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANON, -1, 0);
        msync(lpAddress, dwSize, MS_SYNC|MS_INVALIDATE);
    }*/
    else if(dwFreeType == MEM_DECOMMIT) {

        int result = mprotect(static_cast<void*>(lpAddress), dwSize, PROT_NONE);
        if(result == -1) {
            gGetLastError = errno;
            return FALSE;
        }
        return TRUE;
    }

    assert(0 && "VirtualFree: Unsupported dwFreeType");
    return FALSE;

}

WINBASEAPI DWORD WINAPI GetLastError()
{
    return gGetLastError;
}

static thread_local const char* g_current_wildcard = nullptr;
static int path_filter(const struct dirent* e) {
    return 0 == fnmatch(g_current_wildcard, e->d_name, FNM_PATHNAME);
}
struct FindFileData {
    struct dirent* entries;
    int last_retrieved_entry;
    int num_entries;
    char* dir_name;
    bool initialized;
};

static void FillFindData(const char* dir, const char* name, LPWIN32_FIND_DATAA out) {
    *out = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const auto path = (std::filesystem::path(dir) / name).string();
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes)) return;
    out->dwFileAttributes = attributes.dwFileAttributes;
    out->ftCreationTime = attributes.ftCreationTime;
    out->ftLastAccessTime = attributes.ftLastAccessTime;
    out->ftLastWriteTime = attributes.ftLastWriteTime;
    out->nFileSizeLow = attributes.nFileSizeLow;
    out->nFileSizeHigh = attributes.nFileSizeHigh;
    std::snprintf(out->cFileName, sizeof(out->cFileName), "%s", name);
}

HANDLE WINAPI FindFirstFileA( LPCTSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData)
{
    struct dirent** entries;
    int n = 0;

    // this will NOT work correctly if directory has wildcard

    char* dn = strdup(lpFileName);
    char* bn = strdup(lpFileName);
    char* dir_name = dirname(dn);
    char* base_name = basename(bn);

    g_current_wildcard = base_name;
    n = scandir(dir_name, &entries, path_filter, alphasort);
    g_current_wildcard = nullptr;


    FindFileData* ffd = nullptr;

    if(n > 0) {

        ffd = new FindFileData();
        ffd->entries = new dirent[n];
        for(int i=0;i<n;++i) {
            //memcpy(ffd->entries+i, entries[i], sizeof(struct dirent));
            // sebi: for some reason AddressSanitizer and valgrind complain about memcpy :-/
            const int len = sizeof(ffd->entries[i].d_name);
            strncpy(ffd->entries[i].d_name, entries[i]->d_name, len - 1);
            ffd->entries[i].d_name[len - 1] = '\0';
            free(entries[i]);
        }
        free(entries);
        entries = nullptr;
        ffd->last_retrieved_entry = 0;
        ffd->num_entries = n;
        ffd->initialized = true;
        ffd->dir_name = strdup(dir_name);

        FillFindData(ffd->dir_name, ffd->entries[0].d_name, lpFindFileData);

    }

    free(dn);
    free(bn);

    return ffd == nullptr ? INVALID_HANDLE_VALUE : ffd;
}

BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData)
{
    assert(hFindFile!=0);

    FindFileData* ffd = static_cast<FindFileData*>(hFindFile);
    assert(ffd->initialized);

    assert(ffd->last_retrieved_entry < ffd->num_entries);

    if(ffd->last_retrieved_entry + 1 == ffd->num_entries) {
        gGetLastError = ERROR_NO_MORE_FILES; 
        return FALSE;
    } else {
        const int e_idx = ++ffd->last_retrieved_entry;
        FillFindData(ffd->dir_name, ffd->entries[e_idx].d_name, lpFindFileData);
        return TRUE;
    }
}

BOOL WINAPI FindClose(HANDLE hFindFile)
{
    assert(hFindFile!=0);

    if(hFindFile == INVALID_HANDLE_VALUE)
        return TRUE;

    FindFileData* ffd = static_cast<FindFileData*>(hFindFile);
    assert(ffd->initialized);

    free(ffd->dir_name);
    delete[] ffd->entries;
    delete ffd;

    return TRUE;
}


#endif // PLATFORM_WINDOWS



