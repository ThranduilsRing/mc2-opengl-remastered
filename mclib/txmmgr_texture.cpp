// MC_TextureManager: texture load/save/copy and MC_TextureNode (KTX route-2 and compressed upload).
// Declared in the class headers; shared names in txmmgr_internal.h.
#include "txmmgr_internal.h"
#include "mc2fmt.h"

DWORD actualTextureSize = 0;
DWORD compressedTextureSize = 0;
static int64_t gTxmRealizedTotal = 0;

static const DWORD MC_TEXCACHE_FILE_LZ = 0xF0000000;
static const DWORD MC_TEXCACHE_FILE_RAW = 0xE0000000;
static const DWORD MC_TEXCACHE_MEM_RAW = 0xD0000000;
static const DWORD MC_TEXCACHE_SIZE_MASK = 0x0FFFFFFF;
static const long MC_TEXCACHE_RAW_THRESHOLD = 256 * 1024;

static bool tryReadTgaLogicalSize(File& textureFile, DWORD uvScale, DWORD& logicalWidth, DWORD& logicalHeight)
{
	logicalWidth = 0;
	logicalHeight = 0;

	if (textureFile.fileSize() < sizeof(TGAFileHeader))
		return false;

	// [UI-PHASE1] carried from mc2r_ui_phase1 (base->theirs hunk @txmmgr.cpp+).
	// PNG check first: the data/defs UI art is .png, and interpreting the
	// PNG signature bytes as a TGA header yields garbage width/height.
	// PNG IHDR stores big-endian width/height at offsets 16 and 20.
	unsigned char sniff[24] = {0};
	if (textureFile.read((MemoryPtr)sniff, sizeof(sniff)) == sizeof(sniff) &&
		sniff[0] == 0x89 && sniff[1] == 'P' && sniff[2] == 'N' && sniff[3] == 'G')
	{
		textureFile.seek(0);
		const DWORD pngWidth = (static_cast<DWORD>(sniff[16]) << 24) |
							   (static_cast<DWORD>(sniff[17]) << 16) |
							   (static_cast<DWORD>(sniff[18]) << 8) |
							   static_cast<DWORD>(sniff[19]);
		const DWORD pngHeight = (static_cast<DWORD>(sniff[20]) << 24) |
								(static_cast<DWORD>(sniff[21]) << 16) |
								(static_cast<DWORD>(sniff[22]) << 8) |
								static_cast<DWORD>(sniff[23]);
		if (!pngWidth || !pngHeight)
			return false;
		// PNG UI art is authored 1:1; do not apply the TGA uvScale shrink.
		logicalWidth = pngWidth;
		logicalHeight = pngHeight;
		return true;
	}
	textureFile.seek(0);

	TGAFileHeader header;
	if (textureFile.read((MemoryPtr)&header, sizeof(header)) != sizeof(header))
	{
		textureFile.seek(0);
		return false;
	}

	textureFile.seek(0);

	if (header.width <= 0 || header.height <= 0)
		return false;

	const DWORD scale = uvScale ? uvScale : 1;
	logicalWidth = static_cast<DWORD>(header.width) / scale;
	logicalHeight = static_cast<DWORD>(header.height) / scale;
	return logicalWidth && logicalHeight;
}

DWORD MC_TextureManager::textureFromMemory (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth)
{
	ZoneScopedN("MC_TextureManager::textureFromMemory");
	long i=0;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemory findSlot");
		for (i=0;i<MC_MAXTEXTURES;i++)
		{
			if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
			{
				break;
			}
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));
		
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = nullptr;
	txmLeakNoteAlloc();  // [TXMMGR_TEXTURE_AUDIT v1] from_memory path

	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].logicalWidth = width;
	masterTextureNodes[i].logicalHeight = width;

	masterTextureNodes[i].width = width;
	long txmSize = width * width * bitDepth;
	
	if (!lzBuffer1)
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory lzBuffers");
		lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer1 != nullptr);
		
		lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer2 != nullptr);
	}
	
	actualTextureSize += txmSize;
	DWORD txmCompressSize;
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory LZCompress");
		txmCompressSize = static_cast<DWORD>(LZCompress(lzBuffer2,(MemoryPtr)data,txmSize));
	}
	compressedTextureSize += txmCompressSize;
	
	if (!masterTextureNodes[i].textureData )
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory cacheAlloc");
		masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmCompressSize);
	}
	
	//No More RAM.  Do not display this texture anymore.
	if (masterTextureNodes[i].textureData == nullptr)
		masterTextureNodes[i].gosTextureHandle = 0;
	else
	{
		ZoneScopedN("MC_TextureManager::textureFromMemory cacheCopy");
		memcpy(masterTextureNodes[i].textureData,lzBuffer2,txmCompressSize);
		masterTextureNodes[i].lzCompSize = txmCompressSize;
	}
	
	return(i);
}

DWORD MC_TextureManager::textureInstanceExists (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance, DWORD nFlush)
{
	long i=0;

	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName)
		{
			if (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0)
			{
				if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
				{
					masterTextureNodes[i].numUsers++;
					return(i);							//Return the texture Node Id Now.
				}
				else
				{
					// Copy the texture from old Handle to a new one.
					// Return the NEW handle.
					// There should be no code here!!!
				}
			}
		}
	}
	return 0;
}

DWORD MC_TextureManager::loadTexture (const char *textureFullPathName, gos_TextureFormat key, DWORD hints, DWORD uniqueInstance, DWORD nFlush)
{
	ZoneScopedN("MC_TextureManager::loadTexture");
	long i=0;

	// Residency ground-truth trace (MC2_TEXMGR_LOAD_TRACE=1, default-off,
	// zero-cost when unset): logs every loadTexture call's uniqueInstance +
	// logical name. uniqueInstance != 0 == "modifiable / CPU-locked" (paint;
	// txmmgr.h) -> the texture is CPU_RGBA_REQUIRED and its .tga must NOT be
	// slimmed. Consumed by tools/residency_slim.py to derive the keep/drop set
	// from ground truth (mech *rgb AND plain-named vehicle bodies) instead of
	// fragile filename heuristics.
	{
		static const bool s_loadTrace = mc2::gate::get<"MC2_TEXMGR_LOAD_TRACE">();
		if (s_loadTrace)
		{
			mc2::fmt::print_err("[TEXLOAD] uniq={:d} name={}\n",
				mc2::fmt::as_u32((unsigned long)uniqueInstance), mc2::fmt::safe(textureFullPathName ? textureFullPathName : "<null>"));
			fflush(stderr);
		}
	}

	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].nodeName && (S_stricmp(masterTextureNodes[i].nodeName,textureFullPathName) == 0))
		{
			if (uniqueInstance == masterTextureNodes[i].uniqueInstance)
			{
				if (mc2::gate::get<"MC2_LOG_MECH_ICON">() && strstr(textureFullPathName, "mechicon"))
					mc2::fmt::print_out("[MECHICON] loadTexture CACHE-HIT id={:d}: {}\n", mc2::fmt::as_i32(i), mc2::fmt::safe(textureFullPathName));
				masterTextureNodes[i].numUsers++;
				return(i);							//Return the texture Node Id Now.
			}
			else
			{
				// Copy the texture from old Handle to a new one.
				// Return the NEW handle.
				// There should be no code here!!!
			}
		}
	}

	for (i=0;i<MC_MAXTEXTURES;i++)
	{
		if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
		{
			break;
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));
		
	if (key == gos_Texture_Alpha && Environment.Renderer == 3)
	{
		key = gos_Texture_Keyed;
	}

 	if (mc2::gate::get<"MC2_LOG_MECH_ICON">() && strstr(textureFullPathName, "mechicon"))
		mc2::fmt::print_out("[MECHICON] loadTexture FRESH id={:d}: {}\n", mc2::fmt::as_i32(i), mc2::fmt::safe(textureFullPathName));
	// New Method.  Just store memory footprint of texture.
	// DO NOT create GOS handle until we need it.
 	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	txmLeakNoteAlloc();  // [TXMMGR_TEXTURE_AUDIT v1] named loadTexture path
	masterTextureNodes[i].nodeName = (char *)textureStringHeap->Malloc(strlen(textureFullPathName) + 1);
	gosASSERT(masterTextureNodes[i].nodeName != nullptr);

	strcpy(masterTextureNodes[i].nodeName,textureFullPathName);
	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].uniqueInstance = uniqueInstance;
	masterTextureNodes[i].neverFLUSH = nFlush;
	masterTextureNodes[i].logicalWidth = 0;
	masterTextureNodes[i].logicalHeight = 0;

	// Store a cache-format marker and fileSize in width so that cache knows to create new texture from memory.
	// This way, we never need to know anything about the texture AND we can store PMGs
	// in memory instead of TGAs which use WAY less RAM!
	File textureFile;
	long textureFileOpenResult = textureFile.open(textureFullPathName);

	// ROUTE-2 (MC2_TEXMGR_KTX_PRIMARY, default mode 1): if the .tga cannot be
	// resolved (loose + base-strip + fastfile all miss -- e.g. the redundant
	// /128 .tga was deleted), source dims + RGBA8 from the BC7 .ktx2 sidecar
	// via the CPU decoder and build a MEM_RAW node (cf. textureFromMemoryRaw).
	// Proves the decoder integration; lets a .ktx2 stand alone without a .tga.
	// MC2_TEXMGR_KTX_PRIMARY mode: 0=off, 1=fallback (DEFAULT -- decode the BC7
	// .ktx2 when the .tga is absent; the .tga is the fallback for textures with
	// no .ktx2), 2=force (prefer the .ktx2 even when the .tga exists -- A/B test).
	// Default 1 is safe: for a full deploy (every .tga present) it never fires
	// (open succeeds -> unchanged path, GPU BC7 sidecar VRAM win preserved); for
	// a slim deploy (redundant /128 .tgas removed from loose + tgl.fst via
	// tools/fst_repack_drop.py) the .ktx2 is decoded on demand. Set =0 to force
	// the legacy "tga or bust" behavior.
	static const int s_ktxPrimaryMode = mc2::gate::get<"MC2_TEXMGR_KTX_PRIMARY">();
	if (s_ktxPrimaryMode == 2 || (s_ktxPrimaryMode == 1 && textureFileOpenResult != NO_ERR))
	{
		{
			char sidecar[1024];
			strncpy(sidecar, textureFullPathName, sizeof(sidecar) - 1);
			sidecar[sizeof(sidecar) - 1] = 0;
			char* dot = strrchr(sidecar, '.');
			char* slash = strrchr(sidecar, '/');
			if (dot && (!slash || dot > slash)) *dot = 0;
			if (strlen(sidecar) + 6 < sizeof(sidecar)) strcat(sidecar, ".ktx2");
			RenderCore::KtxImage img;
			std::vector<uint8_t> rgba;
			int kw = 0, kh = 0;
			if (RenderCore::ktxLoadRgba8(sidecar, img) && img.isCompressed &&
				(img.vkFormat == 145 || img.vkFormat == 146) && img.width == img.height &&
				RenderCore::ktxDecodeBc7ToRgba8(img, 0, rgba, &kw, &kh) && kw == kh && kw > 0)
			{
				const DWORD txmSize = (DWORD)(kw * kh * 4);
				masterTextureNodes[i].uvScale = 4;
				masterTextureNodes[i].logicalWidth = (DWORD)kw;
				masterTextureNodes[i].logicalHeight = (DWORD)kh;
				masterTextureNodes[i].hints = hints | gosHint_Try32bpp;
				masterTextureNodes[i].width = MC_TEXCACHE_MEM_RAW + txmSize;
				masterTextureNodes[i].lzCompSize = txmSize;
				actualTextureSize += txmSize;
				compressedTextureSize += txmSize;
				masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmSize);
				if (masterTextureNodes[i].textureData)
				{
					// ktxDecodeBc7ToRgba8 emits RGBA8; the gos MEM_RAW upload expects
					// BGRA8 (matching the .tga decode path), so swap R<->B per pixel.
					// Without this, red content renders blue (blue-tinted scene).
					const uint8_t* src = rgba.data();
					uint8_t* dst = (uint8_t*)masterTextureNodes[i].textureData;
					for (DWORD px = 0; px + 4 <= txmSize; px += 4)
					{
						dst[px + 0] = src[px + 2]; // B
						dst[px + 1] = src[px + 1]; // G
						dst[px + 2] = src[px + 0]; // R
						dst[px + 3] = src[px + 3]; // A
					}
				}
				else
					masterTextureNodes[i].gosTextureHandle = 0;
				mc2::fmt::print_out("[TEXMGR_KTX_PRIMARY] {} -> RGBA8 {:d}x{:d} (ktx2 CPU decode)\n", mc2::fmt::safe(sidecar), mc2::fmt::as_i32(kw), mc2::fmt::as_i32(kh)); fflush(stdout);
				return(i);
			}
		}
	}
	gosASSERT(textureFileOpenResult == NO_ERR);

	// ENCYCLO-3D-2 diagnostic: for TGL preview textures, record where the
	// bytes actually came from at load time (loose disk vs fastfile), the
	// open result, and a few header bytes — separates "loaded garbage/zeros"
	// from "loaded real pixels but never uploaded".
	if ( mc2::gate::get<"MC2_LOG_PREVIEW">() && strstr(textureFullPathName, "tgl") )
	{
		if ( FILE* f = fopen("preview_debug.log","a") )
		{
			unsigned char hdr[18] = {0};
			long fsz = (textureFileOpenResult == NO_ERR) ? textureFile.fileSize() : -1;
			if ( textureFileOpenResult == NO_ERR && fsz >= (long)sizeof(hdr) )
			{
				textureFile.read(hdr, sizeof(hdr));
				textureFile.seek(0);
			}
			mc2::fmt::print(f, "[PREVIEW-LOAD] open={:d} size={:d} disk={:d} name={} tgaType={:d} dims={:d}x{:d} bpp={:d}\n", mc2::fmt::as_i32(textureFileOpenResult), mc2::fmt::as_i32(fsz), mc2::fmt::as_i32(((textureFileOpenResult == NO_ERR) ? textureFile.isLoadedFromDisk() : -1)), mc2::fmt::safe(textureFullPathName), mc2::fmt::as_u32(hdr[2]), mc2::fmt::as_u32(hdr[12] | (hdr[13]<<8)), mc2::fmt::as_u32(hdr[14] | (hdr[15]<<8)), mc2::fmt::as_u32(hdr[16]));
			fclose(f);
		}
	}

	if (textureFile.isLoadedFromDisk())
	{
		// Disk TGAs are 4x-upscaled gameplay textures (logical = physical/4). But some
		// UI atlases are NATIVE-resolution (NOT upscaled), and their FIT UV coords are
		// authored in physical-pixel space -> logical must equal physical (uvScale=1):
		//  - mcui_gn_mechicons.tga (mech-bay roster): blanket uvScale=4 made initIcon's
		//    logical V divisor physical/4 so vehicle indices 118-142 sampled off-atlas.
		//  - cursors*.tga (mouse-cursor sheets): a 128x128 sheet -> logical 32, while the
		//    cursor FIT UNormal (e.g. 48) is physical -> U=48/32=1.5 off-texture ->
		//    INVISIBLE cursor (notably MC2X at the 800-logical cursorsa.fit tier).
		//  - mcl_mc_*.tga (mech-lab component icons: weapons/armor/heatsinks/engines):
		//    native-res UI icons whose FIT/list UVs are physical-pixel. Stock ones
		//    load from the FST (never hit this disk path, so stay uvScale=1), but a
		//    MOD ships them as loose disk TGAs -> blanket uvScale=4 made logical =
		//    physical/4 -> the icon sampled a 1/4 sub-rect and rendered tiny in the
		//    corner. They are NOT 4x-upscaled gameplay art, so force uvScale=1.
		const bool isNativeResUiAtlas = textureFullPathName &&
			(strstr(textureFullPathName, "mcui_gn_mechicons") != nullptr ||
			 strstr(textureFullPathName, "cursors") != nullptr ||
			 strstr(textureFullPathName, "mcl_mc_") != nullptr);
		masterTextureNodes[i].uvScale = isNativeResUiAtlas ? 1 : 4;
	}

	tryReadTgaLogicalSize(textureFile, masterTextureNodes[i].uvScale,
		masterTextureNodes[i].logicalWidth, masterTextureNodes[i].logicalHeight);

	long txmSize = textureFile.fileSize();
	
	if (!lzBuffer1)
	{
		ZoneScopedN("MC_TextureManager::loadTexture lzBuffers");
		lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer1 != nullptr);
		
		lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
		gosASSERT(lzBuffer2 != nullptr);
	}

	//Try reading the RAW data out of the fastFile.
	// If it succeeds, we just saved a complete compress, decompress and two memcpys!!
	long result;
	{
		ZoneScopedN("MC_TextureManager::loadTexture readRAW");
		result = textureFile.readRAW(masterTextureNodes[i].textureData,textureCacheHeap);
	}
	if (!result)
	{
		gosASSERT(txmSize <= MAX_LZ_BUFFER_SIZE);
		{
			ZoneScopedN("MC_TextureManager::loadTexture fileRead");
			textureFile.read(lzBuffer1,txmSize);
		}

		textureFile.close();

		actualTextureSize += txmSize;
		const bool storeRawFileData = (txmSize >= MC_TEXCACHE_RAW_THRESHOLD);
		const DWORD cacheBytes = storeRawFileData ? txmSize : [&]() -> DWORD {
			DWORD txmCompressSize;
			{
				ZoneScopedN("MC_TextureManager::loadTexture LZCompress");
				txmCompressSize = static_cast<DWORD>(LZCompress(lzBuffer2,lzBuffer1,txmSize));
			}
			compressedTextureSize += txmCompressSize;
			return txmCompressSize;
		}();

		{
			ZoneScopedN("MC_TextureManager::loadTexture cacheAlloc");
			masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(cacheBytes);
		}
		if (masterTextureNodes[i].textureData == nullptr)
			masterTextureNodes[i].gosTextureHandle = 0;
		else
		{
			ZoneScopedN("MC_TextureManager::loadTexture cacheCopy");
			memcpy(masterTextureNodes[i].textureData, storeRawFileData ? lzBuffer1 : lzBuffer2, cacheBytes);
			}

		masterTextureNodes[i].lzCompSize = cacheBytes;
		masterTextureNodes[i].width = (storeRawFileData ? MC_TEXCACHE_FILE_RAW : MC_TEXCACHE_FILE_LZ) + txmSize;
	}
	else
	{
		masterTextureNodes[i].lzCompSize = result;
		masterTextureNodes[i].width = MC_TEXCACHE_FILE_LZ + txmSize;
	}

	return(i);
}

// DEV-SHELL texture_refresh (see txmmgr.h). Mirrors the loadTexture disk
// read/compress path byte-for-byte so the refreshed cache entry is exactly
// what a fresh loadTexture of the same file would have produced; the node's
// key/hints/uvScale/logical dims are untouched (same-format, same-size
// replacement). GL work happens at the next get_gosTextureHandle() cache-in,
// so this is safe anywhere on the main thread.
long MC_TextureManager::refreshTexturesByName (const char *substring, long &skippedNoFile)
{
	skippedNoFile = 0;
	if (!substring || !*substring)
		return 0;

	// case-insensitive substring match (nodeNames are stored lowercase, but
	// don't rely on it)
	auto containsNoCase = [](const char *hay, const char *needle) -> bool {
		const size_t nlen = strlen(needle);
		for (const char *p = hay; *p; ++p)
		{
			size_t k = 0;
			while (k < nlen && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
				++k;
			if (k == nlen) return true;
		}
		return false;
	};

	long refreshed = 0;
	for (long i = 0; i < MC_MAXTEXTURES; i++)
	{
		MC_TextureNode &node = masterTextureNodes[i];
		if (!node.nodeName || node.gosTextureHandle == 0xffffffff)
			continue;
		if (!containsNoCase(node.nodeName, substring))
			continue;

		File textureFile;
		if (textureFile.open(node.nodeName) != NO_ERR || !textureFile.isLoadedFromDisk())
		{
			// FST-only (or unopenable): no loose file to refresh from.
			skippedNoFile++;
			continue;
		}

		const long txmSize = textureFile.fileSize();
		if (txmSize <= 0 || txmSize > MAX_LZ_BUFFER_SIZE)
		{
			skippedNoFile++;
			textureFile.close();
			continue;
		}

		if (!lzBuffer1)
		{
			lzBuffer1 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
			gosASSERT(lzBuffer1 != nullptr);
			lzBuffer2 = (MemoryPtr)textureCacheHeap->Malloc(MAX_LZ_BUFFER_SIZE);
			gosASSERT(lzBuffer2 != nullptr);
		}

		textureFile.read(lzBuffer1, txmSize);
		textureFile.close();

		const bool storeRawFileData = (txmSize >= MC_TEXCACHE_RAW_THRESHOLD);
		const DWORD cacheBytes = storeRawFileData
			? (DWORD)txmSize
			: static_cast<DWORD>(LZCompress(lzBuffer2, lzBuffer1, txmSize));

		DWORD *fresh = (DWORD *)textureCacheHeap->Malloc(cacheBytes);
		if (!fresh)
		{
			skippedNoFile++;
			continue;	// leave the old copy live rather than corrupt the node
		}
		memcpy(fresh, storeRawFileData ? lzBuffer1 : lzBuffer2, cacheBytes);

		if (node.textureData)
			textureCacheHeap->Free(node.textureData);
		node.textureData = fresh;
		node.lzCompSize = cacheBytes;
		node.width = (storeRawFileData ? MC_TEXCACHE_FILE_RAW : MC_TEXCACHE_FILE_LZ) + txmSize;

		// Force re-upload: destroy the resident gos texture (same cache-out
		// dance as flushCache) so the next get_gosTextureHandle() re-creates
		// it from the fresh bytes.
		if (node.gosTextureHandle != CACHED_OUT_HANDLE && node.gosTextureHandle != 0)
		{
			gos_DestroyTexture(node.gosTextureHandle);
			if (currentUsedTextures > 0)
				currentUsedTextures--;
		}
		node.gosTextureHandle = CACHED_OUT_HANDLE;
		refreshed++;
	}
	return refreshed;
}

DWORD MC_TextureManager::textureFromMemoryRaw (DWORD *data, gos_TextureFormat key, DWORD hints, DWORD width, DWORD bitDepth)
{
	ZoneScopedN("MC_TextureManager::textureFromMemoryRaw");
	long i=0;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw findSlot");
		for (i=0;i<MC_MAXTEXTURES;i++)
		{
			if (masterTextureNodes[i].gosTextureHandle == 0xffffffff)
			{
				break;
			}
		}
	}

	if (i == MC_MAXTEXTURES)
		STOP(("TOO Many textures in game.  We have exceeded 4096 game handles"));

	masterTextureNodes[i].gosTextureHandle = CACHED_OUT_HANDLE;
	masterTextureNodes[i].nodeName = nullptr;
	txmLeakNoteAlloc();  // [TXMMGR_TEXTURE_AUDIT v1] from_memory raw path
	masterTextureNodes[i].numUsers = 1;
	masterTextureNodes[i].key = key;
	masterTextureNodes[i].hints = hints;
	masterTextureNodes[i].logicalWidth = width;
	masterTextureNodes[i].logicalHeight = width;

	const DWORD txmSize = width * width * bitDepth;
	masterTextureNodes[i].width = MC_TEXCACHE_MEM_RAW + txmSize;
	masterTextureNodes[i].lzCompSize = txmSize;
	actualTextureSize += txmSize;
	compressedTextureSize += txmSize;

	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw cacheAlloc");
		masterTextureNodes[i].textureData = (DWORD *)textureCacheHeap->Malloc(txmSize);
	}

	if (masterTextureNodes[i].textureData == nullptr)
		masterTextureNodes[i].gosTextureHandle = 0;
	else
	{
		ZoneScopedN("MC_TextureManager::textureFromMemoryRaw cacheCopy");
		memcpy(masterTextureNodes[i].textureData, data, txmSize);
	}

	return(i);
}

long MC_TextureManager::saveTexture (DWORD textureIndex, const char *textureFullPathName)
{
	if ((MC_MAXTEXTURES <= textureIndex) || (nullptr == masterTextureNodes[textureIndex].textureData))
	{
		return (~NO_ERR);
	}
	File textureFile;
	long textureFileOpenResult = textureFile.create(textureFullPathName);
	if (NO_ERR != textureFileOpenResult)
	{
		textureFile.close();
		return textureFileOpenResult;
	}

	{
		if (masterTextureNodes[textureIndex].width == 0)
		{
			textureFile.close();
			return (~NO_ERR);		//These faces have no texture!!
		}

		{
			const DWORD cacheFormat = masterTextureNodes[textureIndex].width & 0xF0000000;
			const DWORD originalSize = masterTextureNodes[textureIndex].width & MC_TEXCACHE_SIZE_MASK;
			if (cacheFormat == MC_TEXCACHE_FILE_RAW || cacheFormat == MC_TEXCACHE_MEM_RAW)
			{
				textureFile.write((MemoryPtr)masterTextureNodes[textureIndex].textureData, originalSize);
			}
			else
			{
				// Badboys are now LZ Compressed in texture cache.
				long origSize = static_cast<long>(LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)masterTextureNodes[textureIndex].textureData,masterTextureNodes[textureIndex].lzCompSize,MAX_LZ_BUFFER_SIZE));
				if (origSize != originalSize)
					STOP(("Decompressed to different size from original!  Txm:%s  Width:%d  DecompSize:%d",masterTextureNodes[textureIndex].nodeName,originalSize,origSize));

				if (origSize >= MAX_LZ_BUFFER_SIZE)
					STOP(("Texture TOO large: %s",masterTextureNodes[textureIndex].nodeName));

				textureFile.write(MC_TextureManager::lzBuffer2, origSize);
			}
		}
		textureFile.close();
	}

	return NO_ERR;
}

DWORD MC_TextureManager::copyTexture( DWORD texNodeID )
{
	gosASSERT( texNodeID < MC_MAXTEXTURES );
	if ( masterTextureNodes[texNodeID].gosTextureHandle != -1 )
	{
		masterTextureNodes[texNodeID].numUsers++;
		return texNodeID;
	}
	else
	{
		STOP(( "tried to copy an invalid texture" ));
	}

	return -1;

}
DWORD MC_TextureNode::get_gosTextureHandle (void)	//If texture is not in VidRAM, cache a texture out and cache this one in.
{
	// PERF 2026-05-07: stripped MC_TextureNode::get_gosTextureHandle Tracy
	// scopes/plots from this hot accessor; cache-miss work remains unchanged.
	if (gosTextureHandle == 0xffffffff)
	{
		//Somehow this texture is bad.  Probably we are using a handle which got purged between missions.
		// Just send back, NO TEXTURE and we should be able to debug from there because the tri will have no texture!!
		PAUSE(("txmmgr: Bad texture handle!"));
		return 0x0;
	}
	
	if (gosTextureHandle != CACHED_OUT_HANDLE)
	{
		lastUsed = turn;
		return gosTextureHandle;
	}
	else
	{
		if ((mcTextureManager->currentUsedTextures >= MAX_MC2_GOS_TEXTURES) && !mcTextureManager->flushCache())
		{
			PAUSE(("txmmgr: Out of texture handles!"));
			return 0x0;		//No texture!
		}
	   
		if (width == 0)
		{
			{
				char _cbbuf[256];
				mc2::fmt::to_buf(_cbbuf, "[TXM] zero-width texture node: nodeName={} handle={:d} lzCompSize={:d}", mc2::fmt::safe(nodeName ? nodeName : "<null>"), mc2::fmt::as_u32((unsigned)gosTextureHandle), mc2::fmt::as_u32((unsigned)lzCompSize));
				puts(_cbbuf); fflush(stdout); crashbundle_append(_cbbuf);
			}
			PAUSE(("txmmgr: Textur has zero width!"));
			return 0;		//These faces have no texture!!
		}

		if (!textureData)
		{
			PAUSE(("txmmgr: Cache is out of RAM!"));
			return 0x0;		//No Texture.  Cache is out of RAM!!
		}

		const DWORD cacheFormat = width & 0xF0000000;
		if ((cacheFormat == MC_TEXCACHE_FILE_LZ) || (cacheFormat == MC_TEXCACHE_FILE_RAW) || (cacheFormat == MC_TEXCACHE_MEM_RAW))
		{
			const DWORD originalSize = width & MC_TEXCACHE_SIZE_MASK;
			BYTE* textureBytes = (BYTE*)textureData;
			{
				if (cacheFormat == MC_TEXCACHE_FILE_LZ)
				{
					// Cache this badboy IN.
					// Badboys are now LZ Compressed in texture cache.
					long origSize;
					{
						origSize = static_cast<long>(LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE));
					}
					if (origSize != originalSize)
						STOP(("Decompressed to different size from original!  Txm:%s  Width:%d  DecompSize:%d",nodeName,originalSize,origSize));

					if (origSize >= MAX_LZ_BUFFER_SIZE)
						STOP(("Texture TOO large: %s",nodeName));
					textureBytes = (BYTE*)MC_TextureManager::lzBuffer2;
				}
				else if (cacheFormat == MC_TEXCACHE_MEM_RAW)
				{
					textureBytes = (BYTE*)textureData;
				}
			}

			if (cacheFormat == MC_TEXCACHE_MEM_RAW)
			{
				{
					gosTextureHandle = gos_NewEmptyTexture(key,nodeName,logicalWidth ? logicalWidth : width,hints);
				}
				TEXTUREPTR pTextureData;
				{
					gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
				}
				{
					memcpy(pTextureData.pTexture, textureBytes, originalSize);
				}
				{
					gos_UnLockTexture(gosTextureHandle);
				}
			}
			else
			{
				// TEXMGR-COMPRESSED-UPLOAD-1: gated BC7 .ktx2 sidecar upload.
				// Default-OFF; when the gate is set AND a same-stem .ktx2 sidecar
				// exists on disk AND it is a stored BC7 (vkFormat 145/146) image,
				// upload mip 0 via glCompressedTexImage2D and skip the normal
				// gos_NewTextureFromMemory call. Any failure (no sidecar, not BC7,
				// load fail, no BPTC support) falls through unchanged.
				// Default-ON (opt out with MC2_TEXMGR_COMPRESSED_UPLOAD=0): prefer the
				// BC7 .ktx2 sidecar for GPU upload when present; any failure (no sidecar,
				// not BC7, no BPTC support) falls through to the .tga unchanged.
				static const bool s_texmgrCompressedUpload = mc2::gate::get<"MC2_TEXMGR_COMPRESSED_UPLOAD">();
				bool uploadedCompressed = false;

				// Exclude MODIFIABLE textures (uniqueInstance != 0): the mech/vehicle
				// paint-scheme system gos_LockTexture()s these and rewrites the R/G/B
				// team-color mask in place (mech3d/gvactor setPaintScheme). A BC7
				// texture is GPU-immutable / not CPU-lockable, so compressing it leaves
				// the raw mask on screen (base blue + red/green, no team colors). Static
				// shared textures (uniqueInstance == 0: terrain, trees, buildings) are
				// never locked and keep the BC7 upload. See txmmgr.h ("Texture is
				// modifiable. DO NOT CACHE OUT").
				if (s_texmgrCompressedUpload && nodeName && *nodeName && uniqueInstance == 0)
				{
					if (!GLEW_ARB_texture_compression_bptc)
					{
						static bool s_warnedNoBptc = false;
						if (!s_warnedNoBptc)
						{
							s_warnedNoBptc = true;
							mc2::fmt::print_out("[TEXMGR_BC7] skip: GLEW_ARB_texture_compression_bptc unsupported; compressed upload disabled\n");
						}
					}
					else
					{
						char sidecar[1024];
						strncpy(sidecar, nodeName, sizeof(sidecar) - 1);
						sidecar[sizeof(sidecar) - 1] = '\0';
						char* dot = strrchr(sidecar, '.');
						char* slash = strrchr(sidecar, '/');
						char* bslash = strrchr(sidecar, '\\');
						char* lastSep = slash > bslash ? slash : bslash;
						bool haveRoom = false;
						if (dot && dot > lastSep)
						{
							size_t base = (size_t)(dot - sidecar);
							if (base + 6 < sizeof(sidecar)) { strcpy(sidecar + base, ".ktx2"); haveRoom = true; }
						}
						else
						{
							size_t len = strlen(sidecar);
							if (len + 6 < sizeof(sidecar)) { strcpy(sidecar + len, ".ktx2"); haveRoom = true; }
						}

						if (haveRoom)
						{
							// Disk-only probe (std::fopen, NOT File/FastFile).
							FILE* probe = std::fopen(sidecar, "rb");
							if (!probe)
							{
							}
							else
							{
								std::fclose(probe);
								RenderCore::KtxImage img;
								if (RenderCore::ktxLoadRgba8(sidecar, img) &&
									img.isCompressed &&
									(img.vkFormat == 145 || img.vkFormat == 146))
								{
									GLenum glIF = (img.vkFormat == 146)
										? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
										: GL_COMPRESSED_RGBA_BPTC_UNORM;
									size_t mip0Bytes = (img.mipCount > 1 && img.mipByteOffsets.size() > 1)
										? (size_t)img.mipByteOffsets[1]
										: img.pixels.size();
									DWORD h = gos_NewCompressedTexture2D(
										(uint32_t)glIF, img.width, img.height,
										img.pixels.data(), mip0Bytes, nodeName);
									if (h != 0 /*INVALID_TEXTURE_ID*/ && h != 0xffffffff)
									{
										gosTextureHandle = h;
										uploadedCompressed = true;
										mc2::fmt::print_out("[TEXMGR_BC7] loaded name={} {:d}x{:d} fmt=0x{:x} bytes={:d}\n", mc2::fmt::safe(nodeName), mc2::fmt::as_i32(img.width), mc2::fmt::as_i32(img.height), mc2::fmt::as_u32((unsigned)glIF), mc2::fmt::as_size(mip0Bytes));
									}
									else
									{
										mc2::fmt::print_out("[TEXMGR_BC7] upload failed name={}; fallthrough RGBA8\n", mc2::fmt::safe(nodeName));
									}
								}
								else
								{
									mc2::fmt::print_out("[TEXMGR_BC7] sidecar not BC7/loadable name={}; fallthrough RGBA8\n", mc2::fmt::safe(sidecar));
								}
							}
						}
					}
				}

				if (!uploadedCompressed)
				{
					gosTextureHandle = gos_NewTextureFromMemory(key,nodeName,textureBytes,originalSize,hints);
				}
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;
			lastUsed = turn;

			return gosTextureHandle;
		}
		else
		{
			{
				gosTextureHandle = gos_NewEmptyTexture(key,nodeName,width,hints);
			}
			mcTextureManager->currentUsedTextures++;
			if (mcTextureManager->currentUsedTextures > mcTextureManager->peakUsedTextures)
				mcTextureManager->peakUsedTextures = mcTextureManager->currentUsedTextures;
			++gTxmRealizedTotal;

			TEXTUREPTR pTextureData;
			{
				gos_LockTexture(gosTextureHandle, 0, 0, &pTextureData);
			}
		 
			DWORD txmSize = pTextureData.Height * pTextureData.Height * sizeof(DWORD);
			gosASSERT(textureData);

			{
				LZDecomp(MC_TextureManager::lzBuffer2,(MemoryPtr)textureData,lzCompSize,MAX_LZ_BUFFER_SIZE);
			}
			{
				memcpy(pTextureData.pTexture,MC_TextureManager::lzBuffer2,txmSize);
			}

			{
				gos_UnLockTexture(gosTextureHandle);
			}
			 
			lastUsed = turn;
			return gosTextureHandle;
		}
	}
}
