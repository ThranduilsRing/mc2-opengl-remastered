#ifndef PLATFORM_WINDOWS_H
#define PLATFORM_WINDOWS_H

#define NOMINMAX

#ifdef PLATFORM_WINDOWS
	#include<windows.h>
#else
#include "platform_intrinsics.h"
#include "platform_str.h"
#include "platform_stdlib.h"
#include "platform_io.h"
	#include"platform_windef.h"
	#include"platform_winbase.h"
	#include"platform_winuser.h"
	#include"platform_winnls.h"
	#include"platform_mmsystem.h"
#endif // PLATFORM_WINDOWS_H

#endif /* _WINDOWS_ */
