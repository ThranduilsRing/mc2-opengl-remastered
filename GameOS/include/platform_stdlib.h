#ifndef _STDLIB_WIN_H
#define _STDLIB_WIN_H

#ifdef PLATFORM_WINDOWS
	#include <stdlib.h>
#else

#include<stdio.h>
#include<stdlib.h> // itoa
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

void _splitpath (const char*, char*, char*, char*, char*);
void _itoa(int value, char* str, int radix);
inline int _putenv_s(const char* key, const char* value)
{
    return (*value ? setenv(key, value, 1) : unsetenv(key)) == 0 ? 0 : errno;
}

#define _alloca alloca

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_WINDOWS

#endif /* Not _STDLIB_WIN_H */
