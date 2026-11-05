// Shared surface of the terrtxm2.cpp translation units (terrtxm2.cpp, terrtxm2_{bake,load}.cpp).
//
// terrtxm2.cpp keeps init / destroy, the colormap data access, the texture handle resolve and the save functions; the rest of
// TerrainColorMap is split by responsibility (see DOC in each piece).
#ifndef TERRTXM2_INTERNAL_H
#define TERRTXM2_INTERNAL_H

#ifndef TERRTXM2_H
#include"terrtxm2.h"
#endif
#include"tex_resolve_table.h"

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"

#ifndef CAMERA_H
#include"camera.h"
#endif

#include"resizeimage.h"

#include"platform_io.h"
#include<sys/stat.h>
#include<cstdio>
#include<cstdint>		// BURNIN-DISK-JPEG: uint8_t for stb_image pixel buffer
#include "mc2_dialog.h"
#include "Gates/gate.h"
#include "mc2fmt.h"

inline constexpr int COLOR_MAP_TEXTURE_SIZE = 256;
#define COLOR_MAP_RES					float(COLOR_MAP_TEXTURE_SIZE)

void* DecodeJPG( const char* FileName, BYTE* Data, DWORD DataSize, DWORD* TextureWidth, DWORD* TextureHeight, bool TextureLoad, void *pDestSurf );

inline constexpr int MBOK = static_cast<int>(1);

// Globals defined in terrtxm2.cpp (or elsewhere in the engine) that the split pieces share.
extern bool forceShadowBurnIn;
extern "C" unsigned char* BurninJpeg_DecodeRGB(const unsigned char* data, int dataLen, int* outW, int* outH);
extern "C" void BurninJpeg_FreePixels(unsigned char* px);

// The decoded B,G,R,A colour map, or an empty vector when the sidecar is absent or unusable (the caller falls back to the .tga).
inline std::vector<unsigned char> tryLoadBurninJpg(const char* texturePath, const char* fileName, long* outWidth)
{
	if (outWidth) *outWidth = 0;

	char jName[1024];
	sprintf(jName, "%s.burnin", fileName);
	FullPathFileName burnInJpg;
	burnInJpg.init(texturePath, jName, ".jpg");

	// Use File::open so the mod overlay (TryModOpen) is consulted — std::fopen bypasses it.
	File jpgFile;
	if (NO_ERR != jpgFile.open(static_cast<const char*>(burnInJpg)))
		return {}; // sidecar absent -> caller uses .tga

	long jFileSize = jpgFile.fileSize();
	if (jFileSize <= 0)
	{
		jpgFile.close();
		mc2::fmt::print_out("[BURNIN_JPG] fallback name={} reason=empty-file\n", mc2::fmt::safe(fileName));
		return {};
	}

	// A sidecar this large is not a colour map; refuse it rather than size a buffer from the file.
	constexpr long kMaxJpgBytes = 256L * 1024 * 1024;
	if (jFileSize > kMaxJpgBytes)
	{
		jpgFile.close();
		mc2::fmt::print_out("[BURNIN_JPG] fallback name={} reason=file-too-large ({:d} bytes)\n", mc2::fmt::safe(fileName), mc2::fmt::as_long(jFileSize));
		return {};
	}

	std::vector<unsigned char> jpgBytes(static_cast<size_t>(jFileSize));
	if (jpgFile.read(jpgBytes.data(), jFileSize) != jFileSize)
	{
		jpgFile.close();
		mc2::fmt::print_out("[BURNIN_JPG] fallback name={} reason=read-failed\n", mc2::fmt::safe(fileName));
		return {};
	}
	jpgFile.close();

	int w = 0, h = 0;
	unsigned char* rgb = BurninJpeg_DecodeRGB(jpgBytes.data(), static_cast<int>(jFileSize), &w, &h);
	if (!rgb)
	{
		mc2::fmt::print_out("[BURNIN_JPG] fallback name={} reason=jpeg-decode-failed\n", mc2::fmt::safe(fileName));
		return {};
	}
	if (w <= 0 || h <= 0 || w != h)
	{
		BurninJpeg_FreePixels(rgb);
		mc2::fmt::print_out("[BURNIN_JPG] fallback name={} reason=bad-dims ({:d}x{:d})\n", mc2::fmt::safe(fileName), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
		return {};
	}

	long cmapBytes = static_cast<long>(w) * static_cast<long>(h) * static_cast<long>(sizeof(DWORD));
	std::vector<unsigned char> colorMap(static_cast<size_t>(cmapBytes));

	// Pack stb RGB -> ColorMap B,G,R,A (matches the 24-bit .burnin.tga path).
	const long pixels = static_cast<long>(w) * static_cast<long>(h);
	const unsigned char* src = rgb;
	unsigned char* dst = colorMap.data();
	for (long i = 0; i < pixels; ++i)
	{
		dst[0] = src[2]; // Blue
		dst[1] = src[1]; // Green
		dst[2] = src[0]; // Red
		dst[3] = 0xFF;   // Alpha
		dst += 4;
		src += 3;
	}

	BurninJpeg_FreePixels(rgb);

	mc2::fmt::print_out("[BURNIN_JPG] loaded name={} {:d}x{:d} (from {:d} jpg bytes)\n",
		mc2::fmt::safe(fileName), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h), mc2::fmt::as_size(static_cast<size_t>(jFileSize)));

	if (outWidth) *outWidth = static_cast<long>(w);
	return colorMap;
}

inline bool skipColormapTiles() {
    static const bool s = mc2::gate::get<"MC2_TERRAIN_INDIRECT">()
        && true;
    return s;
}

// COLORMAP-BC7-KTX2-1: default ON; kill-switch MC2_COLORMAP_KTX2=0 disables.
// When ON, BuildColormapAtlas will prefer the .burnin.ktx2 sidecar over cpuColorMap.
inline bool useKtx2Colormap() {
    static const bool s = mc2::gate::get<"MC2_COLORMAP_KTX2">();
    return s;
}

//---------------------------------------------------------------------------
inline void saveTGAFile(MemoryPtr ColorMap, const char * fileName, long pixelWidth)
{
	TGAFileHeader colorMapInfo;
	colorMapInfo.image_id_len = 0;
	colorMapInfo.color_map = 0;
	colorMapInfo.image_type = UNC_TRUE;

	colorMapInfo.cm_first_entry= 0;
	colorMapInfo.cm_length = 0;
	colorMapInfo.cm_entry_size = 0;

	colorMapInfo.x_origin = 0;
	colorMapInfo.y_origin = 0;
	colorMapInfo.width = pixelWidth;
	colorMapInfo.height = pixelWidth;
	colorMapInfo.pixel_depth = 32.0;
	colorMapInfo.image_descriptor = 32;	//DO NOT FLIP!
	
	char newName[1024];
	sprintf(newName,"%s.burnin",fileName);
	
	FullPathFileName outputName;
	outputName.init(texturePath,newName,".tga");
	
	_chmod(outputName,_S_IREAD | _S_IWRITE);
	
	File outputFile;
	outputFile.create(outputName);
	
	outputFile.write(reinterpret_cast<MemoryPtr>((&colorMapInfo)),sizeof(TGAFileHeader));
	outputFile.write(ColorMap,pixelWidth*pixelWidth*sizeof(DWORD));
	outputFile.close();
}

#endif // TERRTXM2_INTERNAL_H
