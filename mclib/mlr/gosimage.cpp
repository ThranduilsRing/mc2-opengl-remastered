//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include "mc2fmt.h"

//#############################################################################
//############################    GOSImage    ###############################
//#############################################################################

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImage::GOSImage( const char* iName ) : Plug (DefaultData)
{
	imageName = iName;

	flags = 0;

	instance = 0;

	mcTextureNodeIndex = 0xffffffff;

	ptr.pTexture = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImage::GOSImage( DWORD iHandle ) : Plug (DefaultData)
{
	char str[20];

	mc2::fmt::to_buf(str, "image{:03d}", mc2::fmt::as_i32(iHandle));

	imageName = str;

	flags = Loaded;

	instance = 0;

	mcTextureNodeIndex = iHandle;

	ptr.pTexture = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImage::GOSImage(const char *name, gos_TextureHints hints) : Plug (DefaultData)
{
	imageName = name;

	flags = Loaded;

	instance = 0;

	ipHints = hints;

	mcTextureNodeIndex = mcTextureManager ? mcTextureManager->loadTexture(name,gos_Texture_Detect,ipHints) : 0xffffffff;

	ptr.pTexture = nullptr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImage::~GOSImage()
{
	imageName = "";

	if((flags & Locked) != 0)
	{
//		gos_UnLockTexture(imageHandle);
	}

	if((flags & Loaded) != 0)
	{
		if (mcTextureManager)
		{
			mcTextureManager->releaseNode(mcTextureNodeIndex);
			mcTextureNodeIndex = 0xffffffff;
		}
	}
	flags = 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int
	GOSImage::GetWidth()
{
	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
int
	GOSImage::GetHeight()
{
	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	GOSImage::LockImage()
{
	if(!(flags & Locked))
	{
		flags |= Locked;
		DWORD imageHandle = mcTextureManager->get_gosTextureHandle(mcTextureNodeIndex);

		if (imageHandle != 0xffffffff)
			gos_LockTexture(imageHandle, 0, false, &ptr);
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	GOSImage::UnlockImage()
{
	if(flags & Locked)
	{
		flags &= ~Locked;
		Start_Timer(Unlock_Texture_Time);
		DWORD imageHandle = mcTextureManager->get_gosTextureHandle(mcTextureNodeIndex);

		if (imageHandle != 0xffffffff)
			gos_UnLockTexture(imageHandle);

		Stop_Timer(Unlock_Texture_Time);

		ptr.pTexture = nullptr;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
unsigned char*
	GOSImage::GetImagePtr()
{
	return reinterpret_cast<unsigned char *>(ptr.pTexture);
}
