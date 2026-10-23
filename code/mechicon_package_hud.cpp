#include "mechicon_package_hud.h"
#include "mechicon.h"
#include "mover.h"
#include "package_chassis.h"
#include "mc2fmt.h"
#include "asset_scale.h"
#include "../mclib/tga_scratch.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Strip {
	std::vector<unsigned long> pixels;   // top-to-bottom rows, BGRA
	long width = 0, height = 0;
	AssetScale::AssetKey key;
	bool valid = false;
};

std::map<std::string, Strip>& strips()
{
	static std::map<std::string, Strip> cache;
	return cache;
}

const Strip* load(std::string_view base, float iconX, float iconY)
{
	char path[512];
	mc2::fmt::to_buf(path, "{}{}_{:d}x{:d}.tga", mc2::fmt::safe(artPath), base,
		static_cast<int>(iconX), static_cast<int>(iconY));
	S_strlwr(path);
	auto& cache = strips();
	const bool fresh = !cache.contains(path);
	Strip& strip = cache[path];
	if (fresh)
	{
		File file;
		std::vector<unsigned char> bytes;
		TGAFileHeader hdr{};
		if (NO_ERR == file.open(path) && mc2::tga::readAllBytes(file, bytes) && mc2::tga::readHeader(bytes, hdr))
		{
			if (hdr.pixel_depth != 32 || hdr.width < iconX || hdr.height < 2 * iconY ||
				!mc2::tga::bodyFits(bytes.size(), sizeof(hdr), hdr.width, hdr.height, hdr.pixel_depth))
			{
				mc2::fmt::print_out("[mechicon] package HUD strip '{}' is not 32-bit {:.0f}x{:.0f}x2 -- stock icon used\n",
					mc2::fmt::safe(path), iconX, iconY);
			}
			else
			{
				strip.width = hdr.width;
				strip.height = hdr.height;
				strip.pixels.resize(static_cast<size_t>(hdr.width) * hdr.height);
				for (size_t i = 0; i < strip.pixels.size(); ++i)
				{
					std::uint32_t pixel = 0;
					std::memcpy(&pixel, bytes.data() + sizeof(hdr) + i * sizeof(pixel), sizeof(pixel));
					strip.pixels[i] = pixel;
				}
				// TGA rows are stored bottom-to-top.
				for (long y = 0; y < hdr.height / 2; ++y)
					std::swap_ranges(strip.pixels.begin() + y * hdr.width, strip.pixels.begin() + (y + 1) * hdr.width,
						strip.pixels.begin() + (hdr.height - 1 - y) * hdr.width);
				strip.key = AssetScale::key(path);
				strip.valid = true;
			}
		}
	}
	return strip.valid ? &strip : nullptr;
}
}

bool mechicon_package_hud::blit(Mover* unit, float iconX, float iconY, bool back, unsigned long* dest, int stride, int width, int height)
{
	const mc2pkg::PackageChassis* pc = unit ? packageChassisByTypeNum(unit->getTypeHandle()) : nullptr;
	if (!pc || pc->hudIcon.empty()) return false;
	const Strip* strip = load(pc->hudIcon, iconX, iconY);
	if (!strip) return false;
	const auto actualW = static_cast<std::uint32_t>(strip->width), actualH = static_cast<std::uint32_t>(strip->height);
	const AssetScale::Vec2 f = AssetScale::blitFactorFor(strip->key, actualW, actualH, "mechicon.blit");
	const float offsetY = back ? static_cast<float>(height) : 0.f;
	for (int j = 0; j < height; ++j)
	{
		const int srcY = static_cast<int>(offsetY * f.y) + static_cast<int>(j * f.y);
		for (int i = 0; i < width; ++i)
		{
			const int srcX = static_cast<int>(i * f.x);
			const bool inside = srcY >= 0 && static_cast<std::uint32_t>(srcY) < actualH && srcX >= 0 && static_cast<std::uint32_t>(srcX) < actualW;
			dest[static_cast<size_t>(j) * stride + i] = inside ? strip->pixels[static_cast<size_t>(srcY) * actualW + srcX] : 0ul;
		}
	}
	return true;
}
