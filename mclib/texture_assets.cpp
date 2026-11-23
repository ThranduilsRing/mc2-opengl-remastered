#include "mc2_czstring.h"
#include "../code/tick_commit.h"
#include "../RenderCore/TextureService.h"
#include "../RenderCore/StockPbr.h"
#include "loose_asset_priority.h"
#include <zlib.h>
#include <unordered_map>
#include "sim_overlap.h"
// VFS texture sources and CPU consumer snapshots, owned by the session service.
#include "texture_sources_internal.h"
#include "texture_ref.h"
#include "tex_resolve_table.h"
#include "file.h"
#include "tgainfo.h"
#include "fastfile.h"
#include "../GameOS/gameos/texture_decode.h"
#include "../GameOS/gameos/gos_render.h"
#include "../RenderCore/CursorImage.h"
#include "../GameOS/gameos/gos_profiler.h"
#include "../GameOS/gameos/ui_texture.h"
#include "Gates/gate.h"
#include "ui_chrome_sheets.h"
#include "mc2fmt.h"
#include <format>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include "../GameOS/gameos/load_scope.h"

namespace mc2::textures {
// A root FST that is not the shipped stock archive (standalone mods replace root FSTs without a mod.json owner).
static bool userPackedArchive(std::string_view original) {
    const auto archive = LookupArchivePath(original);
    if (archive.empty()) return false;
    static std::unordered_map<std::string, bool> userArchives;
    auto [it, inserted] = userArchives.try_emplace(archive, false);
    auto& [archiveKey, isUserArchive] = *it;
    if (inserted) {
        std::ifstream input(archive, std::ios::binary); // path-audit: loose-only root FST provenance
        uLong crc = crc32(0, nullptr, 0);
        char buffer[65536];
        while (input.read(buffer, sizeof(buffer)) || input.gcount())
            crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer), static_cast<uInt>(input.gcount()));
        std::error_code error;
        const auto bytes = std::filesystem::file_size(archive, error);
        isUserArchive = error || !input.eof() || !RenderCore::stockPbrArchive(bytes, static_cast<uint32_t>(crc));
    }
    return isUserArchive;
}
bool stockMeshAssets(std::initializer_list<std::string_view> paths) {
    for (const auto path : paths) {
        const std::string name(path);
        if (LookupModOwner(name.c_str()) || userPackedArchive(name)) return false;
    }
    return true;
}
std::string stockPbrOverride(std::string_view legacy, bool vehicle, StockPbrTextures& maps, bool stockMesh) {
    const std::string original(legacy);
    if (!stockMesh) {
        maps = {};
        return original;
    }
    const auto stem = RenderCore::stockPbrStem(legacy);
    const auto base = RenderCore::stockPbrBase(legacy, vehicle);
    const auto albedo = base + "_albedo.png";
    File probe;
    const bool exists = probe.open(albedo.c_str()) == NO_ERR;
    const auto dot = original.find_last_of('.');
    const auto cooked = original.substr(0, dot) + ".ktx2";
    const bool legacyMod = LookupModOwner(original.c_str()) || LookupModOwner(cooked.c_str());
    const int legacyPriority = std::min(LookupModPriority(original.c_str()), LookupModPriority(cooked.c_str()));
    if (!RenderCore::preferStockPbr(exists, legacyMod, legacyPriority, LookupModPriority(albedo.c_str()))) {
        maps = {};
        return original;
    }
    // Standalone mods replace root FSTs or loose files without a mod.json owner.
    // Apply the same stock provenance rule as the bundled building replacements.
    const bool userArchive = !legacyMod && userPackedArchive(original);
    if (userArchive || (!legacyMod && userLooseBuildingAsset(original, "", [&] {
            return ReadArchiveBytes(original.c_str(), 64u * 1024u * 1024u); }))) {
        maps = {};
        return original;
    }
    if (maps.albedo != albedo) {
        maps = {};
        maps.albedo = albedo;
        auto detail = [&](std::string_view suffix, mc2::TextureRef& ref) {
            const auto path = base + std::string(suffix);
            File file;
            if (file.open(path.c_str()) == NO_ERR)
                ref.reset(load(path.c_str(), gos_Texture_Solid, gosHint_MipmapFilter0 | gosHint_DontShrink));
        };
        detail("_normal.png", maps.normal);
        detail("_orm.png", maps.orm);
        // Distinct stock atlases bound this session (each further actor reuses its atlas).
        static std::unordered_set<std::string> mechAtlases, vehicleAtlases;
        if (const auto [atlasIt, firstBind] = (vehicle ? vehicleAtlases : mechAtlases).insert(stem); firstBind)
            mc2::fmt::print_out("[ACTOR_PBR] atlas={} kind={} normal={} orm={} mechs={} vehicles={} loaded\n", stem,
                vehicle ? "vehicle" : "mech", static_cast<bool>(maps.normal), static_cast<bool>(maps.orm),
                mechAtlases.size(), vehicleAtlases.size());
    }
    return albedo;
}
namespace {
constexpr size_t kMaxSourceBytes = static_cast<size_t>(512) * 1024 * 1024;
constexpr DWORD kInvalidAsset = 0xffffffffu;
using Bytes = std::vector<uint8_t>;
std::string sidecarPath(std::string_view name) {
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    return std::string(name.substr(0, dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : name.size())) + ".ktx2";
}
std::shared_ptr<const Bytes> readBytes(File& file) {
    ZoneScopedN("TextureSource.Read"); MC2_LOAD_SCOPE("TextureSource.Read");
    const auto count = file.fileSize();
    if (count <= 0 || static_cast<size_t>(count) > kMaxSourceBytes) return {};
    auto bytes = std::make_shared<Bytes>(static_cast<size_t>(count));
    file.seek(0);
    return file.read(bytes->data(), count) == count ? bytes : nullptr;
}
// UI-CHROME-SPLIT-1: the legacy UI sheets are re-packed at load from the per-widget PNGs under data/art/ui.
// A sheet a mod ships (or any sheet the manifest does not list) keeps loading as authored; the mod case is logged once.
std::shared_ptr<const Bytes> uiChromeSheetBytes(mc2::czstring path, std::string_view modOwner) {
    const std::string key = ui_chrome::sheetKeyFromPath(path);
    if (key.empty()) return {};
    static std::once_flag once;
    static ui_chrome::Manifest manifest;
    std::call_once(once, [] {
        File mf;
        if (mf.open("data/art/ui/manifest.json") != NO_ERR) return;
        const auto bytes = readBytes(mf);
        if (!bytes) return;
        std::string err;
        if (!ui_chrome::parseManifest(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), manifest, &err))
            mc2::fmt::print_out("[UI_CHROME v1] manifest rejected, sheets load as authored: {}\n", err.c_str());
    });
    const auto found = manifest.find(key);
    if (!found) return {};
    const ui_chrome::Sheet* sheet = &found->get();
    static std::mutex logMutex;
    static std::unordered_set<std::string> logged;
    auto firstTime = [&](std::string_view tag) {
        std::lock_guard<std::mutex> g(logMutex);
        const auto [pos, inserted] = logged.emplace(tag);
        return inserted;
    };
    if (!modOwner.empty()) {
        if (firstTime("mod:" + key))
            mc2::fmt::print_out("[UI_CHROME v1] sheet {} comes from mod {}: loading it as authored (FIT rects)\n", key.c_str(), std::string(modOwner).c_str());
        return {};
    }
    ui_chrome::Pixels packed;
    std::string err;
    auto loader = [](const ui_chrome::Sheet& s, const ui_chrome::Widget& w, ui_chrome::Pixels& px) {
        File wf;
        const std::string wp = std::format("data/art/ui/{}/{}.png", s.key, w.name);
        if (wf.open(wp.c_str()) != NO_ERR) return false;
        const auto wb = readBytes(wf);
        if (!wb) return false;
        auto dec = mc2tex::decodeMemory(wb->data(), wb->size(), wp.c_str(), gos_Texture_Alpha, 0);
        if (!dec) return false;
        px.w = dec->width; px.h = dec->height;
        px.channels = dec->layout == mc2tex::PixelLayout::RGB8 ? 3 : 4;
        px.data = std::move(dec->pixels);
        return true;
    };
    if (!ui_chrome::composeSheet(*sheet, loader, packed, &err)) {
        if (firstTime("fail:" + key))
            mc2::fmt::print_out("[UI_CHROME v1] sheet {} not re-packed ({}): loading it as authored\n", key.c_str(), err.c_str());
        return {};
    }
    auto png = ui_chrome::encodePngStored(packed);
    if (png.empty()) return {};
    if (firstTime("ok:" + key))
        mc2::fmt::print_out("[UI_CHROME v1] sheet {} re-packed from {} widgets\n", key.c_str(), static_cast<unsigned>(sheet->widgets.size()));
    return std::make_shared<const Bytes>(std::move(png));
}
static bool tryReadTgaLogicalSize(File& textureFile, mc2::czstring name, DWORD& uvScale, DWORD& logicalWidth, DWORD& logicalHeight)
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
	if (textureFile.read(static_cast<MemoryPtr>(sniff), sizeof(sniff)) == sizeof(sniff) &&
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
		uvScale = 1;
		logicalWidth = pngWidth;
		logicalHeight = pngHeight;
		return true;
	}
	textureFile.seek(0);

	TGAFileHeader header;
	if (textureFile.read(reinterpret_cast<MemoryPtr>(&header), sizeof(header)) != sizeof(header))
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

static bool txmLoadCooked(mc2::czstring source, mc2::czstring sidecar, bool prefer, RenderCore::KtxImage& out, std::string* resolvedProvider = nullptr)
{
    ZoneScopedN("TextureSource.Cooked.Total");
    File cooked;
    File original;
    bool haveCooked, exists;
    { ZoneScopedN("TextureSource.Cooked.Open");
      haveCooked = cooked.open(sidecar) == NO_ERR;
      exists = haveCooked && original.open(source) == NO_ERR; }
    if (!haveCooked) return false;
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
        // Project file API (whole-second mtimes: an edit inside the cook's own second is not seen as newer).
        const time_t sourceTime = original.getFileMTime();
        const time_t cookedTime = cooked.getFileMTime();
        if (sourceTime > cookedTime) return false;
    }
    const long size = cooked.fileSize();
    if (size <= 0 || size > 256 * 1024 * 1024) return false;
    std::vector<uint8_t> bytes(size);
    { ZoneScopedN("TextureSource.Cooked.Read"); MC2_LOAD_SCOPE("TextureSource.Cooked.Read");
      if (cooked.read(bytes.data(), size) != size) return false; }
    const char* mod = LookupModOwner(sidecar);
    const std::string provider = mod ? std::string("mod:") + mod : cooked.isLoadedFromDisk() ? "base-loose" : "fst";
    if (resolvedProvider) *resolvedProvider = provider;
    RenderCore::TextureKey key;
    { ZoneScopedN("TextureSource.Cooked.Hash"); MC2_LOAD_SCOPE("TextureSource.Cooked.Hash");
      key.source = RenderCore::makeTextureSourceId(provider, sidecar, RenderCore::textureContentHash(bytes.data(), bytes.size())); }
    auto& service = RenderCore::textureService();
    if (auto cached = service.cooked(key)) { out = *cached; return true; }
    auto parsed = [&] { ZoneScopedN("TextureSource.Cooked.Parse"); MC2_LOAD_SCOPE("TextureSource.Cooked.Parse"); return RenderCore::ktxParse(bytes); }();
    if (!parsed.image) return false;
    out = std::move(*parsed.image);
    { ZoneScopedN("TextureSource.Cooked.Publish"); MC2_LOAD_SCOPE("TextureSource.Cooked.Publish");
      service.cacheCooked(key, std::make_shared<const RenderCore::KtxImage>(out)); }
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
            image->pixels.resize(static_cast<size_t>(image->width) * image->height * 4);
            for (size_t i=0, j=0; i+2 < decoded->pixels.size() && j+3 < image->pixels.size(); i+=3,j+=4) {
                std::copy_n(decoded->pixels.data()+i,3,image->pixels.data()+j); image->pixels[j+3]=255;
            }
        } else image->pixels = std::move(decoded->pixels);
    } else {
        image->width = source.width; image->height = source.height;
        if (source.bytes->size() != static_cast<size_t>(image->width) * image->height * 4) return {};
        image->pixels = *source.bytes;
        for (size_t i=0; i < image->pixels.size(); i+=4) {
            std::swap(image->pixels[i],image->pixels[i+2]);
            if (source.format == gos_Texture_Solid) image->pixels[i+3]=255;
        }
    }
    if (source.format == gos_Texture_Keyed) mc2::keyCursorPixels(image->pixels);
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

DWORD load(mc2::czstring path, gos_TextureFormat format, DWORD hints, DWORD variant, DWORD lifetime) {
    ZoneScopedN("TextureSource.Load"); MC2_LOAD_SCOPE("TextureSource.Load");
    { ZoneScopedN("TextureSource.Load.Pump"); graphics::pump_loading_events(); }
    if (!RenderCore::activeTextureService || !path || !*path) return kInvalidAsset;
    ZoneText(path, std::min<size_t>(std::strlen(path), 192));
    if (mc2::gate::get<"MC2_TEXMGR_LOAD_TRACE">()) {
        mc2::fmt::print_out("[TEXLOAD] uniq={:d} name={}\n",mc2::fmt::as_u32(variant),path);
        std::fflush(stdout);
    }
    auto& store=sourceStore();
    { ZoneScopedN("TextureSource.Load.Find");
      if (auto source=store.find(path,variant)) return store.retain(source) ? source->asset : kInvalidAsset; }
    if (variant!=0) {
        // A per-instance unique copy (destroy swaps, first spawn of a type) of a resident texture reuses its bytes: no file open.
        const auto want=(format==gos_Texture_Alpha && Environment.Renderer==3) ? gos_Texture_Keyed : format;
        const DWORD asset=store.cloneResident(path,variant,want,hints,lifetime);
        if (asset!=kInvalidAsset) {
            if (mc2::tick::collecting() || mc2::overlap::onSimWorker()) store.requestCreate(asset);
            return asset;
        }
    }
    if (mc2::gate::get<"MC2_TEXMGR_LOAD_TRACE">()) mc2::fmt::print_out("[TEXLOAD_COLD] uniq={:d} name={}\n",mc2::fmt::as_u32(variant),path);
    auto source=std::make_shared<TextureSource>();
    source->name=path; source->variant=variant; source->hints=hints; source->lifetime=lifetime;
    if (format==gos_Texture_Alpha && Environment.Renderer==3) format=gos_Texture_Keyed;
    source->format=format;
    File file;
    const auto open=[&] { ZoneScopedN("TextureSource.Open"); MC2_LOAD_SCOPE("TextureSource.Open"); return file.open(path); }();
    std::string provider;
    if (source->name.find("a_compass.tga") == std::string::npos) {
        const auto sidecar=sidecarPath(source->name);
        RenderCore::KtxImage cooked; Bytes rgba; int w=0,h=0;
        if (txmLoadCooked(path,sidecar.c_str(),1==2,cooked,&provider) &&
            cooked.isCompressed && cooked.width==cooked.height &&
            [&] { ZoneScopedN("TextureSource.Cooked.Decode"); MC2_LOAD_SCOPE("TextureSource.Cooked.Decode");
                  return RenderCore::ktxDecodeBlocksToRgba8(cooked,0,rgba,&w,&h); }() && w==h && w>0) {
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
        if (auto packed=uiChromeSheetBytes(path,mod ? mod : "")) source->bytes=std::move(packed);
        if (!mod && (strstr(path, "mcl_mm_") || strstr(path, "MCL_MM_") ||
                     strstr(path, "mcl_splashscreen_frame_") || strstr(path, "MCL_SplashScreen_Frame_") ||
                     strstr(path, "mcl_dialog_") || strstr(path, "MCL_Dialog_"))) {
            auto decoded = mc2tex::decodeMemory(source->bytes->data(), source->bytes->size(), path, gos_Texture_Alpha, 0);
            if (decoded) {
                ui_chrome::Pixels px{decoded->width, decoded->height,
                    decoded->layout == mc2tex::PixelLayout::RGB8 ? 3 : 4, std::move(decoded->pixels)};
                if (ui_chrome::clearMenuMatte(path, px))
                    source->bytes = std::make_shared<const Bytes>(ui_chrome::encodePngStored(px));
            }
        }
        provider=mod ? std::string("mod:")+mod : file.isLoadedFromDisk() ? "base-loose" : "fst";
    }
    { ZoneScopedN("TextureSource.Hash"); MC2_LOAD_SCOPE("TextureSource.Hash");
      source->key.source=RenderCore::makeTextureSourceId(provider,path,RenderCore::textureContentHash(source->bytes->data(),source->bytes->size())); }
    const DWORD asset=[&] { ZoneScopedN("TextureSource.Publish"); MC2_LOAD_SCOPE("TextureSource.Publish");
        source->bytes=store.internBytes(source->key.source,source->bytes);
        return store.publish(std::move(source)); }();
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
void prewarmCpuTextures() {
    mc2::overlap::assertMainThread("textures::prewarmCpuTextures");
    ZoneScopedN("TextureSource.PrewarmCpuTextures"); MC2_LOAD_SCOPE("TextureSource.PrewarmCpuTextures");
    if (auto* store=sourcesIfReady()) {
        // Realize the mission's admitted sources while the loading screen is
        // still up, including sources belonging to actors outside the first view.
        for (auto asset : store->coldCpuTextures()) {
            sourceId(asset);
            graphics::pump_loading_events();
        }
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
    ZoneScopedN("TextureSource.CreateCpuTexture"); MC2_LOAD_SCOPE("TextureSource.CreateCpuTexture");
    const auto format=static_cast<gos_TextureFormat>(source->format);
    if ((source->file || source->cookedBc7) && source->variant==0 && !source->name.empty() &&
        source->name.find("a_compass.tga") == std::string::npos) {
        RenderCore::KtxImage cooked;
        if (txmLoadCooked(source->name.c_str(),sidecarPath(source->name).c_str(),true,cooked) &&
            cooked.isCompressed && (cooked.vkFormat==145 || cooked.vkFormat==146)) {
            // The whole cooked chain goes through (level 0 first, tightly packed); a one-level file stays one level.
            const uint32_t mips=cooked.mipCount>1 ? static_cast<uint32_t>(cooked.mipCount) : 1u;
            source->cpuTexture=gos_NewCompressedTexture2D(cooked.vkFormat,cooked.width,cooked.height,
                                                          std::span<const uint8_t>(cooked.pixels.data(),cooked.pixels.size()),source->name.c_str(),
                                                          source->cookedBc7 ? format : gos_Texture_Solid,mips);
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
                const size_t bytes=static_cast<size_t>(lock.Width)*lock.Height*sizeof(DWORD);
                memcpy(lock.pTexture,source->bytes->data(),(std::min)(bytes,source->bytes->size()));
                gos_UnLockTexture(source->cpuTexture);
            }
        }
    }
    sourceStore().noteCpuTexture(source);
    return source->cpuTexture;
}
namespace {
DWORD pixels(DWORD* data, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel, std::string_view name={}) {
    if (!RenderCore::activeTextureService || !data || !width ||
        width>RenderCore::kKtxMaxEdge || bytesPerPixel!=4 || static_cast<size_t>(width)*width*4>kMaxSourceBytes) return kInvalidAsset;
    auto source=std::make_shared<TextureSource>();
    const auto* begin=reinterpret_cast<const uint8_t*>(data);
    source->bytes=std::make_shared<const Bytes>(begin,begin+static_cast<size_t>(width)*width*4);
    source->width=source->height=source->logicalWidth=source->logicalHeight=width;
    source->format=format; source->hints=hints;
    source->name=name;
    auto& store=sourceStore();
    const auto asset=store.publish(std::move(source));
    if (mc2::tick::collecting() || mc2::overlap::onSimWorker()) store.requestCreate(asset);
    return asset;
}
}
DWORD fromPixels(DWORD* data, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel) {
    return pixels(data,format,hints,width,bytesPerPixel);
}
DWORD fromRawPixels(DWORD* data, gos_TextureFormat format, DWORD hints, DWORD width, DWORD bytesPerPixel, std::string_view name) {
    return pixels(data,format,hints,width,bytesPerPixel,name);
}
long save(DWORD asset, mc2::czstring path) {
    auto source=liveSource(asset);
    if (!source || !source->bytes || source->bytes->empty()) return ~NO_ERR;
    File file; const auto error=file.create(path);
    if (error!=NO_ERR) return error;
    const auto size=static_cast<long>(source->bytes->size());
    return file.write(const_cast<uint8_t*>(source->bytes->data()),size)==size ? NO_ERR : ~NO_ERR;
}
long refresh(mc2::czstring match, long& missing) {
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
        mc2vk::invalidateUiImage(source->handle);
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
    if (!mc2vk::uiTextureServiceReady()) return 0;
    if (!mc2vk::hasUiImage(source->handle)) {
        auto copy=snapshot(*source);
        mc2vk::requestUiImage(source->handle,[copy=std::move(copy)]() mutable { return decodeUi(std::move(copy)); });
    }
    return RenderCore::uiTextureId(source->handle);
}
} // namespace mc2::textures
