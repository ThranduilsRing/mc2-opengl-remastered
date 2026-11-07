//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"mlrheaders.hpp"
#include "../texture_assets.h"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImagePool::GOSImagePool()
{
	Verify(gos_GetCurrentHeap() == Heap);
	texturePath = "";
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImagePool::~GOSImagePool()
{
	// Was HashIterator::DeletePlugs: delete every image (no Unregister_Object).
	for (auto &[name, image] : imageHash)
	{
		delete image;
	}
	imageHash.clear();
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void GOSImagePool::UnLoadImages (void)
{
	// Same as RemoveImage on every image: erase the entry first (advancing
	// through erase's return), then destroy the image.
	for (auto it = imageHash.begin(); it != imageHash.end(); )
	{
		const auto &[name, pooled] = *it;
		GOSImage *image = pooled;
		it = imageHash.erase(it);
		Unregister_Object(image);
		delete image;
	}
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImage*
	GOSImagePool::GetImage(const char* image_name)
{
	Check_Object(this);

	MString imageName = image_name;
	Verify(imageName.GetLength() > 0);

	//
	//---------------------------
	// Get the image for the name
	//---------------------------
	//
	GOSImage *&image = imageHash[std::string(image_name)];

	if (image == nullptr)
	{
		gos_PushCurrentHeap(Heap);
		image = new GOSImage(image_name);
		Register_Object(image);
		gos_PopCurrentHeap();
	}
	Check_Object(image);

	return image;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
GOSImage*
	GOSImagePool::GetImage(const char *image_name, gos_TextureFormat format, int size, gos_TextureHints hints)
{
	Check_Object(this);

	MString imageName = image_name;
	Verify(imageName.GetLength() > 0);

	//
	//---------------------------
	// Get the image for the name
	//---------------------------
	//
	GOSImage *&image = imageHash[std::string(image_name)];

	if (image == nullptr)
	{
		gos_PushCurrentHeap(Heap);
		image = new GOSImage(image_name, hints);
		Register_Object(image);
		gos_PopCurrentHeap();
	}
#ifdef _ARMOR
	else
	{
	}
#endif

	Check_Object(image);

	return image;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
void
	GOSImagePool::RemoveImage(GOSImage *image)
{
	Check_Object(this);
	// Erase BEFORE destroying (the name lives in the image). Only the entry
	// that points at this image goes; ~Plug used to unlink exactly that one.
	if (image != nullptr)
	{
		auto it = imageHash.find(std::string(static_cast<const char*>(image->imageName)));
		if (it != imageHash.end())
		{
			const auto &[name, pooled] = *it;
			if (pooled == image)
				imageHash.erase(it);
		}
	}
	Unregister_Object(image);
	delete image;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
TGAFilePool::TGAFilePool(const char* path)
{
	texturePath = path;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
bool
	TGAFilePool::LoadImage(GOSImage *image, int hint)
{
	if( (image->flags & GOSImage::Loaded) != 0)
		return true;

	MString file_name = texturePath;
	file_name += image->imageName;
	file_name += ".tga";

	// sebi: ORIG BUG FIX: I really suspect that also fFileName should be changed to image->imageName!
	// but who cares about correct code in this game... not original developers for sure
	char *fFileName = image->imageName;
	if (((fFileName[0] != 'F') && (fFileName[0] != 'f')) ||
		((fFileName[1] != 'X') && (fFileName[1] != 'x')))
		hint |= gosHint_DisableMipmap;

	DWORD nodeIndex = mc2::textures::load(file_name,gos_Texture_Detect, hint);

	image->SetHandle(nodeIndex);
	
	image->flags |= GOSImage::Loaded;

	return ((image->flags & GOSImage::Loaded) != 0);
}
