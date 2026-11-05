#include "mc2_czstring.h"
#include "stream.h"
#include <stdio.h>
#include <assert.h>
#ifdef PLATFORM_WINDOWS
#include <direct.h>
#else
#include <wchar.h>
#endif
#include <ctime>

#include <sys/types.h>
#include <sys/stat.h>
#include <ctime>
#include <string.h> // strlen needed for linux and windows


#include "string_utils.h"

#ifdef _WIN32
#pragma warning(disable: 4996)
#endif 

class file_stream: public stream
{
	FILE* _file;
	char* _path;
public:

	file_stream(void);
	~file_stream(void) override;
	int open(const char *, const char *) override;
	int close() override;
	size_t write(const void* pbuf, size_t size) override;
	size_t write(const void* pbuf, size_t size, size_t count) override;
	size_t write_byte(char b) override;
	size_t write_word(short sh) override;
	size_t write_dword(unsigned int i) override;
	size_t write_float(float f) override;
	size_t write_cstr(mc2::czstring str) override;

	size_t read(void* pbuf, size_t size) const override;
	size_t read(void* pbuf, size_t size, size_t count) const override;
	char read_byte() override;
	short read_word() override;
	unsigned int read_dword() const override;
	float read_float() const override;
	size_t read_cstr(char* ) const override;

	int seek(int bytes, SEEK_TYPE type) override;
    int tell() override;
	const char* get_path() const override { return _path; }

    //virtual int get_fstat(struct _stat* pstat);
};

stream* stream::makeFileStream()
{
	return new file_stream();
}

void stream::releaseStream(stream* pstream)
{
	delete pstream;
}

file_stream::file_stream(void):_file(nullptr), _path(nullptr)
{
}

file_stream::~file_stream(void)
{
	if(_file)
		fclose(_file);
	StringFree(_path);
}


int file_stream::open(mc2::czstring fpath, mc2::czstring mode)
{
	if(_file)
		fclose(_file);
	if((_file = fopen(fpath, mode))==nullptr)
		return -1;

	//save path for different purposes
	StringFree(_path);
	_path = StringDup(fpath);
	
	return 0;
}

size_t file_stream::write(const void* pbuf, size_t size)
{
	assert(_file && pbuf);
	return fwrite(pbuf, 1, size, _file);
}
size_t file_stream::write(const void* pbuf, size_t size, size_t count)
{
	assert(_file && pbuf);
	return fwrite(pbuf, size, count, _file)*size;
}

size_t  file_stream::write_byte(char b)
{
	assert(_file);
	return fwrite(&b, 1, 1, _file);
}
size_t  file_stream::write_word(short sh)
{
	assert(_file);
	return fwrite(&sh, 2, 1, _file)*2;
}
size_t file_stream::write_dword(unsigned int i)
{
	assert(_file);
	return fwrite(&i, 4, 1, _file)*4;
}

size_t file_stream::write_float(float f)
{
	assert(_file);
	return fwrite(&f, 4, 1, _file)*4;
}

size_t file_stream::write_cstr(mc2::czstring str)
{
	assert(_file);
	if(!str) 
		return fwrite("",1,1,_file);
	return fwrite(str, 1, strlen(str)+1, _file);
}

size_t file_stream::read(void* pbuf, size_t size) const
{
	assert(_file && pbuf);
	return fread(pbuf, 1, size, _file);
}
size_t file_stream::read(void* pbuf, size_t size, size_t count) const
{
	assert(_file && pbuf);
	return fread(pbuf, size, count, _file);
}

char file_stream::read_byte()
{
	assert(_file);
	char c;
	fread(&c, 1, 1, _file);
	return c;
}
short file_stream::read_word()
{
	assert(_file);
	short sh;
	fread(&sh, 2, 1, _file);
	return sh;
}
unsigned int file_stream::read_dword() const
{
	assert(_file);
	unsigned int dw;
	fread(&dw, 4, 1, _file);
	return dw;
}

float file_stream::read_float() const
{
	assert(_file);
	float f;
	fread(&f, 4, 1, _file);
	return f;
}

size_t file_stream::read_cstr(char* c) const
{
	assert(_file);
	size_t bytes_read = 0;
	do
	{
		bytes_read += fread(c, 1, 1, _file);
	} while(*c++!='\0');

	return bytes_read;
}

int file_stream::seek(int bytes, SEEK_TYPE type)
{
	switch(type)
	{
        case S_SET:
            return fseek(_file, bytes, SEEK_SET);
		case S_CUR:
			return fseek(_file, bytes, SEEK_CUR);
        case S_END:
            return fseek(_file, bytes, SEEK_END);
        
		default: 
            assert(0 && "Wrong SEEK_TYPE provided");
			return -1;
	}
}

int file_stream::tell()
{
    return ftell(_file);
}

int file_stream::close()
{
	if(_file)
	{
		int rv = fclose(_file);
		if(rv==0)
			_file=nullptr;
		//maybe i should free _path here also?
		return rv;
	}
	return -1;
}

//int file_stream::get_fstat(struct _stat* pstat)
//{
//    if(_file && pstat)
//        return fstat(_file, pstat);
//    return -1;
//}
