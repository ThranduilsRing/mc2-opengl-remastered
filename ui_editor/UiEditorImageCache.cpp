/***************************************************************
 * FILENAME: UiEditorImageCache.cpp
 * DESCRIPTION: Lightweight image texture cache for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Uses internal GameOS Image decode for PNG/JPG/BMP/TGA-style FIT image refs.
 * - Added executable-relative and repo-relative path probes for MC2R data/art/gui assets.
 * - Removed SDL2_image dependency from UI Editor image previews.
 * - Added extensionless legacy texture probing and GUI basename fallbacks.
 ***************************************************************/

#include "UiEditorImageCache.h"

#include <SDL.h>
#include "../tools/common/tool_vk_gpu.h"

#include "utils/Image.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
	struct CachedImage
	{
		bool attempted;
		bool loaded;
		bool unavailable;
		int width;
		int height;
		uint32_t nativeTexture;
		std::string resolvedPath;

		CachedImage()
			: attempted(false)
			, loaded(false)
			, unavailable(false)
			, width(0)
			, height(0)
			, nativeTexture(0)
		{
		}
	};

	std::map<std::string, CachedImage> g_cache;
    std::vector<uint32_t> g_retiredTextures;
	std::string g_status = "Image cache ready.";

	std::string NormalizeSlashes(std::string value)
	{
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] == '\\')
				value[i] = '/';
		}

		return value;
	}

	std::string ToLowerCopy(std::string value)
	{
		for (std::size_t i = 0; i < value.size(); ++i)
			value[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));

		return value;
	}

	bool FileExists(const std::string& path)
	{
		SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
		if (rw == nullptr)
			return false;

		SDL_RWclose(rw);
		return true;
	}

	void AddCandidate(std::vector<std::string>& candidates, const std::string& candidate)
	{
		if (candidate.empty())
			return;

		const std::string normalized = NormalizeSlashes(candidate);
		if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end())
			candidates.push_back(normalized);
	}

	std::string ReplaceExtension(const std::string& path, const char* extension)
	{
		const std::string::size_type slash = path.find_last_of("/\\");
		const std::string::size_type dot = path.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
			return path + extension;

		return path.substr(0, dot) + extension;
	}

	bool HasKnownImageExtension(const std::string& path)
	{
		const std::string lower = ToLowerCopy(path);
		return (lower.size() >= 4 && (lower.substr(lower.size() - 4) == ".png"
			|| lower.substr(lower.size() - 4) == ".tga"
			|| lower.substr(lower.size() - 4) == ".bmp"
			|| lower.substr(lower.size() - 4) == ".jpg"))
			|| (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".jpeg");
	}

	void AddExtensionlessImageVariants(std::vector<std::string>& candidates, const std::string& path)
	{
		if (HasKnownImageExtension(path))
			return;

		AddCandidate(candidates, path + ".png");
		AddCandidate(candidates, path + ".tga");
		AddCandidate(candidates, path + ".bmp");
		AddCandidate(candidates, path + ".jpg");
	}

	void AddPathVariants(std::vector<std::string>& candidates, const std::string& path)
	{
		AddCandidate(candidates, path);
		AddExtensionlessImageVariants(candidates, path);

		const std::string lower = ToLowerCopy(path);
		if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".tga")
			AddCandidate(candidates, ReplaceExtension(path, ".png"));

		if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".bmp")
			AddCandidate(candidates, ReplaceExtension(path, ".png"));

		if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".jpg")
			AddCandidate(candidates, ReplaceExtension(path, ".png"));

		if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".jpeg")
			AddCandidate(candidates, ReplaceExtension(path, ".png"));
	}

	std::vector<std::string> BuildPathCandidates(const std::string& rawPath)
	{
		std::vector<std::string> candidates;
		if (rawPath.empty())
			return candidates;

		const std::string path = NormalizeSlashes(rawPath);
		const std::string::size_type slash = path.find_last_of('/');
		const std::string basename = (slash == std::string::npos) ? path : path.substr(slash + 1);

		AddPathVariants(candidates, path);
		if (!basename.empty())
		{
			AddPathVariants(candidates, std::string("data/art/gui/") + basename);
			AddPathVariants(candidates, std::string("data/art/") + basename);
		}
		AddPathVariants(candidates, std::string("../") + path);
		AddPathVariants(candidates, std::string("../../") + path);
		AddPathVariants(candidates, std::string("../../../") + path);

		char* basePath = SDL_GetBasePath();
		if (basePath != nullptr)
		{
			const std::string base = NormalizeSlashes(basePath);
			AddPathVariants(candidates, base + path);
			AddPathVariants(candidates, base + "../" + path);
			AddPathVariants(candidates, base + "../../" + path);
			AddPathVariants(candidates, base + "../../../" + path);
			SDL_free(basePath);
		}

		return candidates;
	}

	bool ResolveExistingPath(const std::string& rawPath, std::string& outPath)
	{
		const std::vector<std::string> candidates = BuildPathCandidates(rawPath);
		for (std::size_t i = 0; i < candidates.size(); ++i)
		{
			if (FileExists(candidates[i]))
			{
				outPath = candidates[i];
				return true;
			}
		}

		return false;
	}

	bool LoadTextureFromPath(const std::string& rawPath, CachedImage& image)
	{
		image.attempted = true;

		std::string resolvedPath;
		if (!ResolveExistingPath(rawPath, resolvedPath))
		{
			image.unavailable = true;
			g_status = std::string("Image not found: ") + rawPath;
			return false;
		}

		Image decodedImage;
		if (!decodedImage.loadFromFile(resolvedPath.c_str()) || decodedImage.getWidth() <= 0 || decodedImage.getHeight() <= 0)
		{
			image.unavailable = true;
			g_status = std::string("GameOS Image decode failed: ") + resolvedPath;
			return false;
		}

		const int width = decodedImage.getWidth();
		const int height = decodedImage.getHeight();
		const FORMAT format = decodedImage.getFormat();
		const unsigned char* source = decodedImage.getPixels();
		std::vector<unsigned char> rgbaPixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 255);

		if (format == FORMAT_RGBA8)
		{
			for (std::size_t i = 0; i < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++i)
			{
				const std::size_t p = i * 4;
				rgbaPixels[p + 0] = source[p + 0];
				rgbaPixels[p + 1] = source[p + 1];
				rgbaPixels[p + 2] = source[p + 2];
				rgbaPixels[p + 3] = source[p + 3];
			}
		}
		else if (format == FORMAT_RGB8)
		{
			for (std::size_t i = 0; i < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++i)
			{
				const std::size_t src = i * 3;
				const std::size_t dst = i * 4;
				rgbaPixels[dst + 0] = source[src + 0];
				rgbaPixels[dst + 1] = source[src + 1];
				rgbaPixels[dst + 2] = source[src + 2];
				rgbaPixels[dst + 3] = 255;
			}
		}
		else if (format == FORMAT_I8 || format == FORMAT_A8)
		{
			for (std::size_t i = 0; i < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++i)
			{
				const std::size_t dst = i * 4;
				const unsigned char value = source[i];
				rgbaPixels[dst + 0] = value;
				rgbaPixels[dst + 1] = value;
				rgbaPixels[dst + 2] = value;
				rgbaPixels[dst + 3] = 255;
			}
		}
		else
		{
			image.unavailable = true;
			g_status = std::string("Unsupported GameOS Image format: ") + resolvedPath;
			return false;
		}

        const uint32_t nativeTexture = toolvk::createTexture(width, height, toolvk::TexFormat::Rgba8Unorm,
            rgbaPixels.data(), {}, false, false);

		if (nativeTexture == 0)
		{
			image.unavailable = true;
			g_status = std::string("Image upload failed: ") + resolvedPath;
			return false;
		}

		image.nativeTexture = nativeTexture;
		image.width = width;
		image.height = height;
		image.loaded = true;
		image.unavailable = false;
		image.resolvedPath = resolvedPath;
		g_status = std::string("Loaded image: ") + resolvedPath;
		return true;
	}

}

UiEditorImageTexture::UiEditorImageTexture()
	: loaded(false)
	, unavailable(false)
	, width(0)
	, height(0)
	, resolvedPath("")
	, textureId(0)
{
}

bool UiEditorImageCache_Initialize()
{
	g_status = toolvk::gpuReady() ? "GameOS Image decoder ready." : "Native image device unavailable.";
	return toolvk::gpuReady();
}

void UiEditorImageCache_Shutdown()
{
	UiEditorImageCache_Clear();
    UiEditorImageCache_BeginFrame();
}

void UiEditorImageCache_BeginFrame() {
    for (const uint32_t id : g_retiredTextures) toolvk::destroyTexture(id);
    g_retiredTextures.clear();
}

void UiEditorImageCache_Clear()
{
	for (std::map<std::string, CachedImage>::iterator it = g_cache.begin(); it != g_cache.end(); ++it)
	{
		if (it->second.nativeTexture != 0)
			g_retiredTextures.push_back(it->second.nativeTexture);
	}

	g_cache.clear();
}

const UiEditorImageTexture* UiEditorImageCache_Get(const char* path)
{
	static UiEditorImageTexture result;

	result = UiEditorImageTexture();

	if (path == nullptr || path[0] == '\0')
		return &result;

	const std::string key = NormalizeSlashes(path);
	CachedImage& image = g_cache[key];

	if (!image.attempted)
		LoadTextureFromPath(key, image);

	result.loaded = image.loaded;
	result.unavailable = image.unavailable;
	result.width = image.width;
	result.height = image.height;
	result.resolvedPath = image.resolvedPath.c_str();
	result.textureId = (ImTextureID)(intptr_t)toolvk::imguiTex(image.nativeTexture);
	return &result;
}

const char* UiEditorImageCache_GetStatus()
{
	return g_status.c_str();
}
