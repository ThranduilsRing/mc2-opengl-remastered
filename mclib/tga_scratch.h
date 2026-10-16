// WHAT: the scratch-buffer helpers of the terrain texture loaders: reading a whole file into owned bytes, and checking that an
// uncompressed TGA holds the pixels its header claims.
// WHY: terrain textures are mod content. A truncated file must be refused with a log line before a decode loop walks past its
// buffer; the checks live here, free of the engine, so a malformed-file test can drive them.
#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

class File;
struct TGAFileHeader;

namespace mc2::tga
{
// No terrain texture comes near this; a larger size is a corrupt directory entry, not an image.
inline constexpr std::size_t kMaxFileBytes = std::size_t(1) << 30;

// True when `fileBytes` holds a header of `headerBytes` and width x height pixels at `pixelDepth` bits (24 is packed to three
// bytes per pixel; anything else is read as four).
inline bool bodyFits (std::size_t fileBytes, std::size_t headerBytes, long width, long height, int pixelDepth)
{
	if (width <= 0 || height <= 0 || fileBytes < headerBytes)
		return false;
	const std::size_t bytesPerPixel = pixelDepth == 24 ? 3 : 4;
	return fileBytes - headerBytes >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytesPerPixel;
}

// Copies the leading header out of `bytes`; false when the file is shorter than the header.
template <class HeaderT>
bool readHeader (const std::vector<unsigned char>& bytes, HeaderT& out)
{
	if (bytes.size() < sizeof(HeaderT))
		return false;
	std::memcpy(&out, bytes.data(), sizeof(HeaderT));
	return true;
}

// Replaces `out` with the whole contents of an open file. False (and `out` empty) for an empty or oversized file or a short read.
template <class FileT>
bool readAllBytes (FileT& file, std::vector<unsigned char>& out)
{
	out.clear();
	const std::size_t size = file.fileSize();
	if (size == 0 || size > kMaxFileBytes)
		return false;
	out.assign(size, 0);
	if (static_cast<std::size_t>(file.read(out.data(), static_cast<long>(size))) != size)
	{
		out.clear();
		return false;
	}
	return true;
}

// Reads a whole TGA file and its header. Terrain textures are mod content, so an empty, unreadable or header-less file is
// refused with a log line instead of being decoded. `what` names the texture in the log line.
bool loadTga (File& file, std::vector<unsigned char>& bytes, TGAFileHeader& info, const char* what);

// True when the file holds the pixels its header claims; a truncated file is refused with a log line, never walked past its end.
bool tgaPixelsPresent (const std::vector<unsigned char>& bytes, const TGAFileHeader& info, const char* what);
}
