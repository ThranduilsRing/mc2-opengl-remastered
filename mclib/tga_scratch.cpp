#include "tga_scratch.h"

#include "file.h"
#include "tgainfo.h"
#include "../GameOS/gameos/utils/mc2log.h"

namespace mc2::tga
{
bool loadTga (File& file, std::vector<unsigned char>& bytes, TGAFileHeader& info, const char* what)
{
	if (readAllBytes(file, bytes) && readHeader(bytes, info))
		return true;
	MC2_LOG(Io, Error, "terrain: {} is empty, unreadable or shorter than a TGA header; it is not loaded", what);
	return false;
}

bool tgaPixelsPresent (const std::vector<unsigned char>& bytes, const TGAFileHeader& info, const char* what)
{
	if (bodyFits(bytes.size(), sizeof(TGAFileHeader), info.width, info.height, info.pixel_depth))
		return true;
	MC2_LOG(Io, Error, "terrain: {} is truncated ({} bytes for {}x{} at {} bits); it is not loaded", what, bytes.size(),
		static_cast<int>(info.width), static_cast<int>(info.height), static_cast<int>(info.pixel_depth));
	return false;
}
}
