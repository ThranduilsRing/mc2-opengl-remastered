//---------------------------------------------------------------------------
// 
// MechCommander 2
//
// TGA file Specifics
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

#include<string.h>
#include<gameos.hpp>
#include "tga_scratch.h"
#include "content_error.h"
//---------------------------------------------------------------------------
using RGB = struct _RGB
{
	unsigned char r;
	unsigned char g;
	unsigned char b;
};

//---------------------------------------------------------------------------
void flipTopToBottom (MemoryPtr buffer, BYTE depth, long width, long height)
{
	//-----------------------------------------------------------
	// ScanLine by Scanline
	MemoryPtr tmpBuffer = static_cast<MemoryPtr>(malloc(width * height * (depth>>3)));
	MemoryPtr tmpPointer = tmpBuffer;

	//----------------------------------
	// Point to last scanline.
	long pixelWidth = width * (depth >> 3);
	MemoryPtr workBuffer = buffer + ((height - 1) * pixelWidth);
	
	for (long i=0;i<height;i++)
	{
		memcpy(tmpBuffer,workBuffer,pixelWidth);

		workBuffer -= pixelWidth;
		tmpBuffer += pixelWidth;
	}

	memcpy(buffer,tmpPointer,width * height * (depth>>3));

	free(tmpPointer);
}
	
//---------------------------------------------------------------------------
void tgaCopy (MemoryPtr dest, MemoryPtr src, long size)
{
	long numCopied = 0;
	while (numCopied != size)
	{
		*dest = *src;
		dest++;
		src++;

		*dest = *src;
		dest++;
		src++;

		*dest = *src;
		dest++;
		src++;

		*dest = 0xff;
		dest++;
		numCopied++;
	}
}

//---------------------------------------------------------------------------
void loadTGATexture (FilePtr tgaFile, MemoryPtr ourRAM, long width, long height)
{
	std::vector<unsigned char> bytes;
	TGAFileHeader info{};
	if (!mc2::tga::loadTga(*tgaFile, bytes, info, "loadTGATexture")) return;
	const TGAFileHeader* header = &info;
	if (width <= 0 || height <= 0 || header->width != width || header->height != height ||
	    (header->pixel_depth != 24 && header->pixel_depth != 32)) {
		mc2::content::error(mc2::content::Kind::Texture, tgaFile->getFilename(), "invalid TGA dimensions or depth");
		return;
	}
	MemoryPtr tgaBuffer = bytes.data();

	switch (header->image_type)
	{
		case UNC_PAL:
		{
			//------------------------------------------------
			// If palette, use entries to create 24Bit image.
		}
		break;

		case UNC_TRUE:
		{
			//------------------------------------------------
			// This is just a bitmap.  Copy it into ourRAM.
			MemoryPtr image = tgaBuffer + sizeof(TGAFileHeader);

			if (!mc2::tga::tgaPixelsPresent(bytes, info, "loadTGATexture")) return;
			if (header->pixel_depth == 32)
				memcpy(ourRAM,image,width * height * 4);
			else
				tgaCopy(ourRAM,image,width * height);

			//------------------------------------------------------------------------
			// Must check image_descriptor to see if we need to un upside down image.
			bool left = (header->image_descriptor & 16) != 0;
			bool top = (header->image_descriptor & 32) != 0;

			if (!top && !left)
			{
				//--------------------------------
				// Image is Upside down.
				flipTopToBottom(ourRAM,32,width,height);
			}
			else if (!top && left)
			{
				//flipTopToBottom(ourRAM,header->pixel_depth,width,height);
				//flipRightToLeft(ourRAM,header->pixel_depth,width,height);
			}
			else if (top && left)
			{
				//flipRightToLeft(ourRAM,header->pixel_depth,width,height);
			}
		}
		break;

		case RLE_PAL:
		{
			//------------------------------------------------
			// If palette, use entries to create 24Bit image.
		}
		break;

		case RLE_TRUE:
		{
			MemoryPtr image = tgaBuffer + sizeof(TGAFileHeader);
			const std::size_t pixels = static_cast<std::size_t>(width) * height;
			if (!mc2::tga::decodeRle(ourRAM, pixels * 4, image, bytes.size() - sizeof(TGAFileHeader),
			                         pixels, header->pixel_depth)) {
				memset(ourRAM, 0, pixels * 4);
				mc2::content::error(mc2::content::Kind::Texture, tgaFile->getFilename(), "invalid TGA RLE packet");
				return;
			}

			//------------------------------------------------------------------------
			// Must check image_descriptor to see if we need to un upside down image.
			bool left = (header->image_descriptor & 16) != 0;
			bool top = (header->image_descriptor & 32) != 0;

			if (!top && !left)
			{
				//--------------------------------
				// Image is Upside down.
				flipTopToBottom(ourRAM,32,width,height);
			}
			else if (!top && left)
			{
				//flipTopToBottom(ourRAM,header->pixel_depth,width,height);
				//flipRightToLeft(ourRAM,header->pixel_depth,width,height);
			}
			else if (top && left)
			{
				//flipRightToLeft(ourRAM,header->pixel_depth,width,height);
			}
		}
		break;
	}


}	

void loadTGAMask (FilePtr tgaFile, MemoryPtr ourRAM, long width, long height)
{
	MemoryPtr tgaBuffer = static_cast<MemoryPtr>(malloc(tgaFile->fileSize()));

	tgaFile->read(tgaBuffer,tgaFile->fileSize());

	//---------------------------------------
	// Parse out TGAHeader.
	TGAFileHeader *header = reinterpret_cast<TGAFileHeader *>(tgaBuffer);

	gosASSERT(header->width == width);
	gosASSERT(header->height == height);
	gosASSERT(header->image_type != UNC_TRUE);
	gosASSERT(header->image_type != RLE_TRUE);
	gosASSERT(header->image_type != RLE_PAL);

	switch (header->image_type)
	{
		case UNC_PAL:
		case UNC_GRAY:
		{
			//------------------------------------------------
			// This is just a bitmap.  Copy it into ourRAM.
			MemoryPtr image = tgaBuffer + sizeof(TGAFileHeader);
			if (header->color_map)
				image += header->cm_length * (header->cm_entry_size>>3);

			memcpy(ourRAM,image,width * height);

			//------------------------------------------------------------------------
			// Must check image_descriptor to see if we need to un upside down image.
			bool left = (header->image_descriptor & 16) != 0;
			bool top = (header->image_descriptor & 32) != 0;

			if (!top && !left)
			{
				//--------------------------------
				// Image is Upside down.
				flipTopToBottom(ourRAM,header->pixel_depth,width,height);
			}
			else if (!top && left)
			{
				flipTopToBottom(ourRAM,header->pixel_depth,width,height);
				//flipRightToLeft(ourRAM,header->pixel_depth,width,height);
			}
			else if (top && left)
			{
				//flipRightToLeft(ourRAM,header->pixel_depth,width,height);
			}
		}
		break;

		case RLE_PAL:
		{
		}
		break;
	}

	free(tgaBuffer);
}	

//---------------------------------------------------------------------------
