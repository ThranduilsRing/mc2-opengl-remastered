#include "../code/tick_commit.h"
#include "../RenderCore/TextureService.h"
#include "sim_overlap.h"
// VFS texture sources and CPU consumer snapshots, owned by the session service.
#include "texture_sources_internal.h"
#include "texture_ref.h"
#include "tex_resolve_table.h"
#include "file.h"
#include "tgainfo.h"
#include "fastfile.h"
#include "../GameOS/gameos/texture_decode.h"
#include "../GameOS/gameos/gos_profiler.h"
#ifdef MC2_VULKAN_ISLAND
#include "../GameOS/gameos/ui_texture.h"
#endif
#include "Gates/gate.h"
#include "mc2fmt.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace mc2::textures {
namespace {
constexpr size_t kMaxSourceBytes = size_t(512) * 1024 * 1024;
constexpr DWORD kInvalidAsset = 0xffffffffu;
using Bytes = std::vector<uint8_t>;
std::string sidecarPath(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    return std::string(name.substr(0, dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : name.size())) + ".ktx2";
}
std::shared_ptr<const Bytes> readBytes(File& file) {
    const auto count = file.fileSize();
    if (count <= 0 || static_cast<size_t>(count) > kMaxSourceBytes) return {};
    auto bytes = std::make_shared<Bytes>(static_cast<size_t>(count));
    file.seek(0);
    return file.read(bytes->data(), count) == count ? bytes : nullptr;
}
static bool tryReadTgaLogicalSize(File& textureFile, const char* name, DWORD& uvScale, DWORD& logicalWidth, DWORD& logicalHeight)
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

    // A loose stock atlas is not necessarily upscaled. Use the archive's authored
    // extent when available; this only reads metadata and never changes VFS precedence.
    if (textureFile.isLoadedFromDisk() && name && (strstr(name,"/art/") || strstr(name,"\\art\\"))) {
        const auto bytes = ReadArchiveBytes(name, kMaxSourceBytes);
        if (bytes.size() >= sizeof(TGAFileHeader)) {
            TGAFileHeader authored{}; memcpy(&authored,bytes.data(),sizeof(authored));
            if (authored.width>0 && authored.height>0) {
                const DWORD sx=header.width/authored.width, sy=header.height/authored.height;
                uvScale=(sx && sx==sy && header.width%authored.width==0 && header.height%authored.height==0) ? sx : 1;
            }
        }
    }
	const DWORD scale = uvScale ? uvScale : 1;
	logicalWidth = static_cast<DWORD>(header.width) / scale;
	logicalHeight = static_cast<DWORD>(header.height) / scale;
	return logicalWidth && logicalHeight;
}

static bool txmLoadCooked(const char* source, const char* sidecar, bool prefer, RenderCore::KtxImage& out, std::string* resolvedProvider = nullptr)
{
    File cooked;
    if (cooked.open(sidecar) != NO_ERR) return false;
    File original;
    const bool exists = original.open(source) == NO_ERR;
    const int sourceRank = LookupModPriority(source) + (exists && !original.isLoadedFromDisk() ? 1 : 0);
    const int cookedRank = LookupModPriority(sidecar) + (!cooked.isLoadedFromDisk() ? 1 : 0);
    if (!RenderCore::preferCookedSource(exists, sourceRank, cookedRank, prefer)) return false;
    // One mod provides both, both loose: a source edited in place after the sidecar was cooked makes the sidecar stale.
    // Base-game files are exempt: their mtimes come from the install copy, not from editing.
    const char* sourceMod = LookupModOwner(source);
    const char* cookedMod = LookupModOwner(sidecar);
    if (exists && sourceMod && cookedMod && std::string_view(sourceMod) == cookedMod &&
        sourceRank == cookedRank && original.isLoadedFromDisk() && cooked.isLoadedFromDisk() &&
        original.getFilename() && cooked.getFilename()) {
        std::error_code ecSource, ecCooked;
        const auto sourceTime = std::filesystem::last_write_time(original.getFilename(), ecSource);
        const auto cookedTime = std::filesystem::last_write_time(cooked.getFilename(), ecCooked);
        if (!ecSource && !ecCooked && sourceTime > cookedTime) return false;
    }
    const long size = cooked.fileSize();
    if (size <= 0 || size > 256 * 1024 * 1024) return false;
    std::vector<uint8_t> bytes(size);
    if (cooked.read(bytes.data(), size) != size) return false;
    const char* mod = LookupModOwner(sidecar);
    const std::string provider = mod ? std::string("mod:") + mod : cooked.isLoadedFromDisk() ? "base-loose" : "fst";
    if (resolvedProvider) *resolvedProvider = provider;
    RenderCore::TextureKey key;
    key.source = RenderCore::makeTextureSourceId(provider, sidecar, RenderCore::textureContentHash(bytes.data(), bytes.size()));
    auto& service = RenderCore::textureService();
    if (auto cached = service.cooked(key)) { out = *cached; return true; }
    auto parsed = RenderCore::ktxParse(bytes);
    if (!parsed.image) return false;
    out = std::move(*parsed.image);
    service.cacheCooked(key, std::make_shared<const RenderCore::KtxImage>(out));
    return true;
}

struct UiSource {
    std::shared_ptr<const Bytes> bytes;
    std::string name;
    gos_TextureFormat format;
    DWORD hints, width, height;
    bool file;
};
UiSource snapshot(const TextureSource& source) {
    return {source.bytes, source.name, static_cast<gos_TextureFormat>(source.format),
            source.hints, source.width, source.height, source.file};
}
std::shared_ptr<const RenderCore::KtxImage> decodeUi(UiSource source) {
    if (!source.bytes) return {};
    auto image = std::make_shared<RenderCore::KtxImage>();
    if (source.file) {
        auto decoded = mc2tex::decodeMemory(source.bytes->data(), source.bytes->size(),
                                            source.name.c_str(), source.format, source.hints);
        if (!decoded) return {};
        image->width = decoded->width; image->height = decoded->height;
        if (decoded->layout == mc2tex::PixelLayout::RGB8) {
            image->pixels.resize(size_t(image->width) * image->height * 4);
            for (size_t i=0, j=0; i+2 < decoded->pixels.size() && j+3 < image->pixels.size(); i+=3,j+=4) {
                std::copy_n(decoded->pixels.data()+i,3,image->pixels.data()+j); image->pixels[j+3]=255;
            }
        } else image->pixels = std::move(decoded->pixels);
    } else {
        image->width = source.width; image->height = source.height;
        if (source.bytes->size() != size_t(image->width) * image->height * 4) return {};
        image->pixels = *source.bytes;
        for (size_t i=0; i < image->pixels.size(); i+=4) {
            std::swap(image->pixels[i],image->pixels[i+2]);
            if (source.format == gos_Texture_Solid) image->pixels[i+3]=255;
        }
    }
    image->vkFormat=37; image->mipCount=1; image->mipByteOffsets={0};
    return image;
}

} // namespace

bool ready() { return sourcesIfReady() != nullptr; }
std::optional<RenderCore::KtxImage> loadCookedSidecar(std::string_view source) {
    if (source.empty()) return std::nullopt;
    const std::string name(source);
    RenderCore::KtxImage out;
    if (!txmLoadCooked(name.c_str(), sidecarPath(name).c_str(), true, out)) return std::nullopt;
    return out;
}

TextureSources& sourceStore() { startSources(); return *sourcesIfReady(); }
void startSources() {
    auto& service=RenderCore::textureService();
    if (!service.sourceOwner()) service.ownSources(std::make_unique<TextureSources>([](DWORD texture) {
        if (gos_GetTextureTexels(texture).status==GosTexels::Status::Ready) gos_DestroyTexture(texture);
    }));
}

DWORD load(const char* path, gos_TextureFormat format, DWORD hints, DWORD variant, DWORD lifetime) {
    ZoneScopedN("TextureSource.Load");
    if (!RenderCore::activeTextureService || !path || !*path) return kInvalidAsset;
    if (mc2::gate::get<"MC2_TEXMGR_LOAD_TRACE">()) {
        mc2::fmt::print_out("[TEXLOAD] uniq={:d} name={}\n",mc2::fmt::as_u32(variant),path);
        std::fflush(stdout);
    }
    auto& store=sourceStore();
    if (auto source=store.find(path,variant)) return store.retain(source) ? source->asset : kInvalidAsset;
    auto source=std::make_shared<TextureSource>();
    source->name=path; source->variant=variant; source->hints=hints; source->lifetime=lifetime;
    if (format==gos_Texture_Alpha && Environment.Renderer==3) format=gos_Texture_Keyed;
    source->format=format;
    File file;
    const auto open=file.open(path);
    std::string provider;
    if (1 != 0) {
        const auto sidecar=sidecarPath(source->name);
        RenderCore::KtxImage cooked; Bytes rgba; int w=0,h=0;
        if (txmLoadCooked(path,sidecar.c_str(),1==2,cooked,&provider) &&
            cooked.isCompressed && cooked.width==cooked.height &&
            RenderCore::ktxDecodeBlocksToRgba8(cooked,0,rgba,&w,&h) && w==h && w>0) {
            for (size_t i=0;i+3<rgba.size();i+=4) std::swap(rgba[i],rgba[i+2]);
            source->bytes=std::make_shared<const Bytes>(std::move(rgba));
            source->width=source->height=source->logicalWidth=source->logicalHeight=static_cast<DWORD>(w);
            source->uvScale=4; source->hints |= gosHint_Try32bpp;
            source->cookedBc7=cooked.vkFormat==145 || cooked.vkFormat==146;
        }
    }
    if (!source->bytes && open!=NO_ERR) {
        source->bytes=std::make_shared<const Bytes>(Bytes{255,0,255,255});
        source->width=source->height=source->logicalWidth=source->logicalHeight=1;
        provider="missing";
        mc2::fmt::print_out("[TEXMGR_MISSING] texture not found, bound a 1x1 placeholder: {}\n",path);
    } else if (!source->bytes) {
        source->file=true;
        if (file.isLoadedFromDisk()) {
            const bool native=strstr(path,"mcui_gn_mechicons") || strstr(path,"cursors") || strstr(path,"mcl_mc_");
            source->uvScale=native ? 1 : 4;
        }
        tryReadTgaLogicalSize(file,path,source->uvScale,source->logicalWidth,source->logicalHeight);
        source->width=source->logicalWidth*source->uvScale; source->height=source->logicalHeight*source->uvScale;
        source->bytes=readBytes(file);
        if (!source->bytes) return kInvalidAsset;
        const auto* mod=LookupModOwner(path);
        provider=mod ? std::string("mod:")+mod : file.isLoadedFromDisk() ? "base-loose" : "fst";
    }
    source->key.source=RenderCore::makeTextureSourceId(provider,path,RenderCore::textureContentHash(source->bytes->data(),source->bytes->size()));
    source->bytes=store.internBytes(source->key.source,source->bytes);
    const DWORD asset=store.publish(std::move(source));
    if (mc2::tick::collecting() || mc2::overlap::onSimWorker()) store.requestCreate(asset);
    return asset;
}
void commitTickTextures() {
    mc2::overlap::assertMainThread("textures::commitTickTextures");
    if (auto* store=sourcesIfReady()) {
        for (auto asset : store->takeCreates()) sourceId(asset);
        store->collectRetired();
    }
}
DWORD sourceId(DWORD asset) {
    if (mc2::tick::collecting() || mc2::overlap::onSimWorker()) {
        if (auto* store=sourcesIfReady()) store->requestCreate(asset);
        return 0;
    }
    mc2::overlap::assertMainThread("textures::sourceId");
    if (asset==kInvalidAsset || asset==0) return asset;
    auto source=liveSource(asset);
    if (!source) return 0;
    if (source->cpuTexture) return source->cpuTexture;
    ZoneScopedN("TextureSource.CreateCpuTexture");
    const auto format=static_cast<gos_TextureFormat>(source->format);
    if ((source->file || source->cookedBc7) && source->variant==0 && !source->name.empty()) {
        RenderCore::KtxImage cooked;
        if (txmLoadCooked(source->name.c_str(),sidecarPath(source->name).c_str(),true,cooked) &&
            cooked.isCompressed && (cooked.vkFormat==145 || cooked.vkFormat==146)) {
            const size_t bytes=cooked.mipCount>1 && cooked.mipByteOffsets.size()>1
                ? cooked.mipByteOffsets[1] : cooked.pixels.size();
            source->cpuTexture=gos_NewCompressedTexture2D(cooked.vkFormat,cooked.width,cooked.height,
                                                          cooked.pixels.data(),bytes,source->name.c_str(),
                                                          source->cookedBc7 ? format : gos_Texture_Solid);
        }
    }
    if (!source->cpuTexture) {
        if (source->file) {
            source->cpuTexture=gos_NewTextureFromMemory(format,source->name.c_str(),
                const_cast<uint8_t*>(source->bytes->data()),static_cast<DWORD>(source->bytes->size()),source->hints);
        } else {
            source->cpuTexture=gos_NewEmptyTexture(format,source->name.empty() ? nullptr : source->name.c_str(),source->width,source->hints);
            TEXTUREPTR lock{};
            gos_LockTexture(source->cpuTexture,0,0,&lock);
            if (lock.pTexture) {
                const size_t bytes=size_t(lock.Width)*lock.Height*sizeof(DWORD);
                memcpy(lock.pTexture,source->bytes->data(),(std::min)(bytes,source->bytes->size()));
                gos_UnLockTexture(source->cpuTexture);
            }
        }
    }
    sourceStore().noteCpuTexture(source);
    return source->cpuTexture;
}
namespace {
DWORD pixels(DWORD* data, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel) {
    if (!RenderCore::activeTextureService || !data || !width ||
        width>RenderCore::kKtxMaxEdge || bytesPerPixel!=4 || size_t(width)*width*4>kMaxSourceBytes) return kInvalidAsset;
    auto source=std::make_shared<TextureSource>();
    const auto* begin=reinterpret_cast<const uint8_t*>(data);
    source->bytes=std::make_shared<const Bytes>(begin,begin+size_t(width)*width*4);
    source->width=source->height=source->logicalWidth=source->logicalHeight=width;
    source->format=format; source->hints=hints;
    auto& store=sourceStore();
    const auto asset=store.publish(std::move(source));
    if (mc2::tick::collecting() || mc2::overlap::onSimWorker()) store.requestCreate(asset);
    return asset;
}
}
DWORD fromPixels(DWORD* data, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel) {
    return pixels(data,format,hints,width,bytesPerPixel);
}
DWORD fromRawPixels(DWORD* data, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel) {
    return pixels(data,format,hints,width,bytesPerPixel);
}
long save(DWORD asset, const char* path) {
    auto source=liveSource(asset);
    if (!source || !source->bytes || source->bytes->empty()) return ~NO_ERR;
    File file; const auto error=file.create(path);
    if (error!=NO_ERR) return error;
    const auto size=static_cast<long>(source->bytes->size());
    return file.write(const_cast<uint8_t*>(source->bytes->data()),size)==size ? NO_ERR : ~NO_ERR;
}
long refresh(const char* match, long& missing) {
    missing=0; if (!match || !*match || !sourcesIfReady()) return 0;
    std::string needle=match;
    for (char& c:needle) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    long count=0;
    for (const auto& weak:sourceStore().assets()) if (auto source=weak.lock()) {
        if (!source->users || source->name.empty() || !RenderCore::textureService().isLive(source->handle)) continue;
        std::string haystack=source->name;
        for (char& c:haystack) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (haystack.find(needle)==std::string::npos) continue;
        File file;
        if (file.open(source->name.c_str())!=NO_ERR || !file.isLoadedFromDisk()) { ++missing; continue; }
        auto bytes=readBytes(file);
        if (!bytes) { ++missing; continue; }
        const auto identity=RenderCore::makeTextureSourceId("refresh",source->name,
            RenderCore::textureContentHash(bytes->data(),bytes->size()));
        source->bytes=sourceStore().internBytes(identity,std::move(bytes)); source->file=true;
        queueCpuTexture(*source);
        RenderCore::textureService().rebind(source->handle,source->asset);
#ifdef MC2_VULKAN_ISLAND
        mc2vk::invalidateUiImage(source->handle);
#endif
        ++count;
    }
    if (count) invalidateTexResolveTable();
    sourceStore().collectRetired();
    return count;
}
std::shared_ptr<const RenderCore::KtxImage> image(DWORD asset) {
    auto source=liveSource(asset); return source ? decodeUi(snapshot(*source)) : nullptr;
}
uint64_t uiId(DWORD asset) {
    auto source=liveSource(asset);
    if (!source) return 0;
#ifdef MC2_VULKAN_ISLAND
    if (!mc2vk::uiTextureServiceReady()) return 0;
    if (!mc2vk::hasUiImage(source->handle)) {
        auto copy=snapshot(*source);
        mc2vk::requestUiImage(source->handle,[copy=std::move(copy)]() mutable { return decodeUi(std::move(copy)); });
    }
    return RenderCore::uiTextureId(source->handle);
#else
    return 0;
#endif
}
} // namespace mc2::textures
