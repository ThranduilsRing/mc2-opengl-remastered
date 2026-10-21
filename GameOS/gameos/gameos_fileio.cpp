#include "gameos.hpp"
#include "toolos.hpp"
#include "fileio.hpp"

#include<string.h>

// gos_FileSize
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
//#include <unistd.h> // close()
#include "platform_io.h"
//

#include <stdio.h> // fopen
#include "platform_io.h"

#include <errno.h>

#include "platform_io.h" // will be removed
#include "mc2fmt.h"

namespace {

// The one place a GOS stream touches the C runtime file API. The stream
// handle goes through it for open/seek/read/write/close, so that plumbing
// exists once instead of once per member function.
FILE* openStream(const char* path, gosEnum_FileWriteStatus status)
{
    const char* mode = nullptr;
    switch(status) {
        case READONLY:
            mode = "rb";
            break;
        case READWRITE:
            mode = "rwb";
            break;
        case APPEND:
            mode = "ab";
            break;
        default:
            STOP(("Unknown file open mode"));
            gosASSERT(0);
    }

    FILE* file = fopen(path, mode);
    if(!file)
    {
        int err = errno; // save errno from being possible overwritten by sprintf
        mc2::fmt::print_err("gosFileStream: failed to open file: {} : {}\n", mc2::fmt::safe(path), mc2::fmt::safe(strerror(err)));
        STOP(("Failed to open file"));
    }
    return file;
}

DWORD seekStream(FILE* file, int where, gosEnum_FileSeekType seek_type)
{
    gosASSERT(file);
    int whence = 0;
    switch(seek_type) {
        case FROMSTART:
            whence = SEEK_SET;
            break;
        case FROMCURRENT:
            whence = SEEK_CUR;
            break;
        case FROMEND:
            whence = SEEK_END;
            break;
        default:
            STOP(("Unknown seek_type"));
            gosASSERT(0);
    }

    return fseek(file, where, whence);
}

DWORD readStream(FILE* file, void* buffer, DWORD length)
{
    gosASSERT(file);
    size_t size = 1;
    size_t nmemb = length;
    size_t num_items = fread(buffer, size, nmemb, file);
    gosASSERT(num_items == nmemb); // .. can trigger if eof
    return num_items * size;
}

DWORD writeStream(FILE* file, const void* buffer, DWORD length)
{
    gosASSERT(file);
    size_t size = 1;
    size_t nmemb = length;
    size_t num_items = fwrite(buffer, size, nmemb, file);
    gosASSERT(num_items == nmemb); // .. can trigger if eof
    return num_items * size;
}

void closeStream(FILE* file)
{
    gosASSERT(file);
    fclose(file);
}

} // namespace

gosFileStream::gosFileStream( const char *FileName, gosEnum_FileWriteStatus fwstatus )
{
    gosASSERT(FileName);
    gosASSERT(strlen(FileName) < MAX_PATH);
    m_writeEnabled = fwstatus;
    strcpy(m_Filename, FileName);

    // ???
    pNext = nullptr;

    m_hFile = openStream(m_Filename, fwstatus);
}

DWORD gosFileStream::Seek( int where, gosEnum_FileSeekType seek_type)
{
    return seekStream(static_cast<FILE*>(m_hFile), where, seek_type);
}

DWORD gosFileStream::Read( void *buffer, DWORD length )
{
    return readStream(static_cast<FILE*>(m_hFile), buffer, length);
}

DWORD gosFileStream::Write( const void *buffer, DWORD length )
{
    return writeStream(static_cast<FILE*>(m_hFile), buffer, length);
}

gosFileStream::~gosFileStream()
{
    closeStream(static_cast<FILE*>(m_hFile));
}

// toolos.hpp
void __stdcall gos_FileSetReadWrite(char const* FileName)
{
    STOP(("gos function not implemented"));
}

DWORD __stdcall gos_FileSize(char const* FileName)
{
	int fd = _open(FileName, O_RDONLY);
    if(fd == -1) {
        STOP(("gos_FileSize failed"));
        return 0;
    }

    struct stat buf;
    fstat(fd, &buf);
    _close(fd);
    
    return buf.st_size;
}
__int64 __stdcall gos_FileTimeStamp(char const* FileName)
{
    return 0;
}
// HGOSFILE = gosFileStream*
void __stdcall gos_OpenFile(gosFileStream** out_fstream, char const* path, gosEnum_FileWriteStatus fwstatus)
{
    *out_fstream = new gosFileStream(path, fwstatus);
}

DWORD __stdcall gos_ReadFile(HGOSFILE hfile, void* buf, DWORD size)
{
    gosFileStream* fstream = hfile;
    return fstream->Read(buf, size);
}

DWORD __stdcall gos_WriteFile(HGOSFILE hfile, void const* buf, DWORD size)
{
    gosFileStream* fstream = hfile;
    return fstream->Write(buf, size);
}

void __stdcall gos_CloseFile(gosFileStream* hfile)
{
    gosFileStream* fstream = hfile;
    delete fstream;
}
////////////////////////////////////////////////////////////////////////////////
bool __stdcall gos_DoesFileExist(char const* FileName)
{
    STOP(("gos function not implemented"));
    return false;
}
bool __stdcall gos_CreateDirectory(char const* FileName)
{
    STOP(("gos function not implemented"));
    return false;
}
char* __stdcall gos_FindDirectories(char const* DirectoryName)
{
    STOP(("gos function not implemented"));
    return nullptr;
}
void __stdcall gos_FindDirectoriesClose()
{
    STOP(("gos function not implemented"));
}
char* __stdcall gos_FindDirectoriesNext()
{
    STOP(("gos function not implemented"));
    return nullptr;
}
char* __stdcall gos_FindFiles(char const* PathFileName)
{
    STOP(("gos function not implemented"));
    return nullptr;
}
void __stdcall gos_FindFilesClose()
{
    STOP(("gos function not implemented"));
}
char* __stdcall gos_FindFilesNext()
{
    STOP(("gos function not implemented"));
    return nullptr;
}

void __stdcall gos_GetCurrentPath(char* Buffer, int buf_len)
{
    gosASSERT(Buffer);
    if(getcwd(Buffer, buf_len) == nullptr) {
        int last_error = errno;
        STOP(("Current path is longer than buffer provided for it, getwd: %s\n", strerror(last_error)));
    }
}

void __stdcall gos_GetFile(char const* FileName, BYTE** MemoryImage, SIZE_T* Size)
{
    STOP(("gos function not implemented"));
}

// sebi
bool __stdcall gos_FileExists(char const* FileName)
{
    if(0 == access(FileName, F_OK))
        return true;
    return false;
}

////////////////////////////////////////////////////////////////////////////////
