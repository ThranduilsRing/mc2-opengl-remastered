#include "mc2_czstring.h"
#include "icon_registry.h"
#include "icon_decode_cache.h"
#include "asystem.h"
#include "file.h"
#include "inifile.h"
#include "fastfile.h"
#include "asset_scale.h"
#include "../mclib/texture_assets.h"
#include "../RenderCore/KtxLoader.h"
#include "../GameOS/gameos/texture_decode.h"
#include "../GameOS/gameos/gos_render.h"
#include "mc2fmt.h"
#include "Gates/gate.h"
#include "vfs.h"
#include "../GameOS/gameos/load_scope.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include <filesystem>
#include <unordered_map>
#include <set>
#include <bit>
#include <array>

namespace unit_icons {
namespace {
Registry registry;
DWORD texture = 0;
std::set<std::string> warned;
// Per-instance damage overlays live on small pages so a damage lock never touches the big source atlas.
constexpr int kDamageSlots = 240, kDamagePerPage = 16, kDamageEdge = 256, kDamageCols = 4;
constexpr int kDamagePages = kDamageSlots / kDamagePerPage;
std::vector<Icon> damageIcons;
DWORD damagePages[kDamagePages]{};
std::map<std::string, AssetScale::Vec2> sourceScale;
std::map<std::string, int> namedPriority;

struct Pixels { int w = 0, h = 0; DWORD logicalW = 0, logicalH = 0; std::vector<uint32_t> data; };
Pixels load(std::string path, bool hud = false) {
    Pixels out;
    if (!fileExists(path.c_str())) return out;
    if (hud) {
        // Damage colours are exact palette values; a cooked BC7 sidecar must not quantize them.
        File file;
        if (file.open(path.c_str()) != NO_ERR || file.getLength() <= 0 || file.getLength() > 64 * 1024 * 1024) return out;
        std::vector<uint8_t> bytes(file.getLength());
        if (file.read(bytes.data(), static_cast<long>(bytes.size())) != bytes.size()) return out;
        auto image = mc2tex::decodeMemory(bytes.data(), bytes.size(), path.c_str(), gos_Texture_Alpha, 0);
        if (!image) return out;
        out.w = image->width; out.h = image->height;
        out.data.resize(static_cast<size_t>(out.w) * out.h);
        const int stride = image->layout == mc2tex::PixelLayout::RGB8 ? 3 : 4;
        for (size_t i = 0; i < out.data.size(); ++i) {
            const auto* p = image->pixels.data() + i * stride;
            out.data[i] = (static_cast<uint32_t>(stride == 4 ? p[3] : 255) << 24) | (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
        }
        return out;
    }
    const DWORD handle = mc2::textures::load(path.c_str(), gos_Texture_Keyed, 0);
    auto image = mc2::textures::image(handle);
    mc2::textures::logicalSize(handle, out.logicalW, out.logicalH);
    mc2::textures::release(handle);
    if (!image || image->isCompressed || image->pixels.size() < static_cast<size_t>(image->width) * image->height * 4) return out;
    out.w = image->width; out.h = image->height;
    out.data.resize(static_cast<size_t>(out.w) * out.h);
    for (size_t i = 0; i < out.data.size(); ++i) {
        const auto* p = image->pixels.data() + i * 4;
        out.data[i] = (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
    }
    return out;
}

void addSheet(std::string path, int cw, int ch, int stripRows = 0) {
    graphics::pump_loading_events();
    if (!stripRows && sourceScale.contains(path)) return;
    auto src = load(path, stripRows != 0);
    if (src.data.empty()) return;
    if (stripRows) {
        // Height declares the authored scale; width may add or remove icon columns.
        const float scale = static_cast<float>(src.h) / (ch * stripRows);
        const AssetScale::Vec2 f{scale, scale};
        int cols = static_cast<int>(src.w / (cw * f.x) + .01f);
        int rows = stripRows;
        while (cols > 0 && static_cast<int>((cols - 1) * cw * f.x) + static_cast<int>((cw - 1) * f.x) >= src.w) --cols;
        while (rows > 0 && static_cast<int>((rows - 1) * ch * f.y) + static_cast<int>((ch - 1) * f.y) >= src.h) --rows;
        const int w = cols * cw, h = rows * ch;
        std::vector<uint32_t> sampled(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int sx = static_cast<int>((x / cw) * cw * f.x) + static_cast<int>((x % cw) * f.x);
                const int sy = static_cast<int>((y / ch) * ch * f.y) + static_cast<int>((y % ch) * f.y);
                sampled[static_cast<size_t>(y) * w + x] = src.data[static_cast<size_t>(sy) * src.w + sx];
            }
        registry.sheet(path, sampled, w, h, cw, ch, true);
    } else {
        const float sx = src.logicalW ? static_cast<float>(src.w) / src.logicalW : 1.f;
        const float sy = src.logicalH ? static_cast<float>(src.h) / src.logicalH : 1.f;
        sourceScale[path] = {sx, sy};
        registry.sheet(path, src.data, src.w, src.h, static_cast<int>(cw * sx), static_cast<int>(ch * sy));
    }
}

void layoutSheet(std::string layout, std::string block) {
    FitIniFile file;
    if (file.open(layout.c_str()) != NO_ERR || file.seekBlock(block.c_str()) != NO_ERR) return;
    long w = 0, h = 0;
    char name[256]{};
    if (file.readIdLong("Width", w) != NO_ERR || file.readIdLong("Height", h) != NO_ERR ||
        file.readIdString("FileName", name, sizeof(name) - 1) != NO_ERR || w <= 0 || h <= 0) return;
    std::string path = "data/art/" + canonical(name);
    if (!path.ends_with(".tga") && !path.ends_with(".png")) path += ".tga";
    addSheet(path, static_cast<int>(w), static_cast<int>(h));
}

void collect(mc2::czstring key, void* data) {
    auto path = canonical(key);
    if ((path.ends_with(".tga") || path.ends_with(".png")) &&
        (path.starts_with("data/art/mechicons/") || path.starts_with("data/art/pilots/")))
        static_cast<std::set<std::string>*>(data)->insert(std::move(path));
}

// LookupModPriority falls back to _access() for a path no mod owns: once per portrait per frame was a disk probe each.
// The rank of a legacy sheet is cached until the VFS index changes (mod reload / rebuild) or the registry shuts down.
int cachedSheetRank(std::string_view sheet) {
    static std::unordered_map<std::string, int> ranks;
    static uint64_t seen = 0;
    if (seen != mc2::vfs::generation()) { ranks.clear(); seen = mc2::vfs::generation(); }
    const std::string key(sheet);
    if (const auto found = ranks.find(key); found != ranks.end()) { const auto& [name, rank] = *found; return rank; }
    const int rank = LookupModPriority(key.c_str());
    ranks.emplace(key, rank);
    return rank;
}
void init(std::string first = {}, int cw = 25, int ch = 30) {
    if (texture) return;
    MC2_LOAD_SCOPE("icons.init");
    std::vector<uint32_t> missing(16 * 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) missing[y * 16 + x] = ((x / 4 + y / 4) % 2) ? 0xffff00ff : 0xff202020;
    registry.add("missing", 16, 16, missing);
    // These existing FITs describe the authored grid, independently of displayed widget size.
    layoutSheet("data/art/mcl_gn_availablemechentry.fit", "Static1");
    layoutSheet("data/art/mc2x_availablemechentry.fit", "Static1");
    layoutSheet("data/art/mcl_gn_deploymentteams.fit", "MechEntryIcon");
    for (const auto* name : {"data/art/mcui_gn_mechicons.tga", "data/art/mc2x_mechicons.tga", "data/art/x_mc2x_mechicons.tga"})
        addSheet(name, first == name ? cw : 25, first == name ? ch : 30);
    if (!first.empty() && !sourceScale.contains(first)) addSheet(first, cw, ch);
    for (int width : {640, 800, 1024})
        for (bool vehicle : {false, true}) {
            const auto hud = hudIconSource(width, width, vehicle);
            addSheet(hud.sheet, hud.width, hud.height, vehicle ? 1 : 2);
        }

    std::set<std::string> loose;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it("data/art/mechicons", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) collect(it->path().generic_string().c_str(), &loose);
    for (std::filesystem::directory_iterator it("data/art/pilots", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) collect(it->path().generic_string().c_str(), &loose);
    EnumerateModFiles("data/art/mechicons/", collect, &loose);
    EnumerateModFiles("data/art/pilots/", collect, &loose);
    for (long i = 0; i < numFastFiles; ++i) {
        const auto* info = fastFiles[i]->getFilesInfo();
        for (long j = 0; j < fastFiles[i]->getNumFiles(); ++j) collect(info[j].pfe->name, &loose);
    }
    uint64_t decodeKey = iconDecodeVersion;
    bool cacheable = true;
    { MC2_LOAD_SCOPE("icons.key");
      for (const auto& path : loose) {
        const auto digest = mc2::vfs::contentDigest(path);
        cacheable = cacheable && digest != 0;
        decodeKey = iconDecodeKey(decodeKey, path, digest, LookupModPriority(path.c_str()));
      }
    }
    // These are exact decoded source pixels, independent of resolution, sheet
    // layout and atlas coordinates. Packing and all texture mutations stay here.
    std::array<char, 1024> userDir{};
    const std::string cacheDir = gos_GetUserDataDirectory(userDir)
        ? (std::filesystem::path(userDir.data()) / "cache" / "icons").generic_string() : std::string{};
    Registry cachedIcons;
    std::map<std::string, int> cachedRanks;
    const bool cacheHit = [&] {
        MC2_LOAD_SCOPE("icons.cacheRead");
        if (!cacheable) return false;
        const auto bytes = mc2cache::readEntry(cacheDir, iconDecodeKind, decodeKey);
        return bytes && decodeIcons(*bytes, decodeKey, cachedIcons, cachedRanks);
    }();
    { MC2_LOAD_SCOPE("icons.decode");
      for (const auto& path : loose) {
        graphics::pump_loading_events();
        const bool pilot = path.starts_with("data/art/pilots/");
        auto relative = path.substr(std::string(pilot ? "data/art/pilots/" : "data/art/mechicons/").size());
        relative = relative.substr(0, relative.find_last_of('.'));
        const std::string key = pilot ? "pilot/" + relative : relative.starts_with("hud/") ? relative : "mech/" + relative;
        const int priority = LookupModPriority(path.c_str());
        if (namedPriority.contains(key) && namedPriority[key] <= priority) continue;
        const bool raw = key.starts_with("hud/") || pilot;
        Pixels src;
        if (raw && cachedRanks.contains(key) && cachedRanks.at(key) == priority) {
            auto& icon = cachedIcons.icons.at(key);
            src.w = icon.width; src.h = icon.height;
            src.data = std::move(icon.pixels);
        } else src = load(path, raw);
        if (!src.data.empty() && key.starts_with("mech/")) {
            // A named icon is one cell: a sheet that reached the named path contributes its first.
            const auto [cw, ch] = firstCell(src.w, src.h, src.data);
            if (cw != src.w || ch != src.h) {
                mc2::fmt::print_out("[ICON_REGISTRY] {} is a {}x{} sheet; using its first {}x{} cell\n", key, src.w, src.h, cw, ch);
                src.data = topLeft(src.w, src.data, cw, ch);
                src.w = cw; src.h = ch;
            }
        }
        if (!src.data.empty() && registry.add(key, src.w, src.h, src.data)) {
            namedPriority[key] = priority;
            // A mod's grey silhouette is drawn like stock's blue HUD art, not tinted by the orange card animation.
            const bool mono = std::ranges::all_of(src.data, [](uint32_t p) { return ((p >> 16) & 255) == ((p >> 8) & 255) && ((p >> 8) & 255) == (p & 255); });
            if (key.starts_with("mech/") && mono && priority < 999998) {
                auto blue = src.data;
                for (auto& p : blue) p = (p & 0xff000000u) | (((p >> 16) & 255) * 110 / 255) << 16 | (((p >> 8) & 255) * 170 / 255) << 8 | ((p & 255) * 225 / 255);
                const std::string blueKey = "monohud/" + key.substr(5);
                if (registry.add(blueKey, src.w, src.h, blue)) namedPriority[blueKey] = priority;
            }
        } else if (raw) cacheable = false;
      }
      if (!cacheHit && cacheable) {
        auto rawRanks = namedPriority;
        std::erase_if(rawRanks, [](const auto& item) {
            const auto& [key, rank] = item;
            return !key.starts_with("hud/") && !key.starts_with("pilot/");
        });
        mc2cache::writeEntry(cacheDir, iconDecodeKind, decodeKey, encodeIcons(registry, rawRanks, decodeKey));
      }
    }
    mc2::fmt::print_out("[ICON_DECODE_CACHE] {} key={:016x} sources={}\n", cacheHit ? "hit" : "miss", decodeKey, loose.size());
    damageIcons.resize(kDamageSlots);
    for (int i = 0; i < kDamageSlots; ++i) {
        const int cell = i % kDamagePerPage;
        damageIcons[i] = {40, 48, 1 + cell % kDamageCols * 42, 1 + cell / kDamageCols * 50, {}, {}};
    }
    std::vector<uint32_t> blankPage(static_cast<size_t>(kDamageEdge) * kDamageEdge, 0);
    for (DWORD& page : damagePages) {
        page = mc2::textures::fromRawPixels(std::bit_cast<DWORD*>(blankPage.data()), gos_Texture_Alpha, 0, kDamageEdge);
        mc2::textures::adoptOwned(page);
    }
    const bool packed = [&] { MC2_LOAD_SCOPE("icons.pack"); return registry.pack(); }();
    if (!packed || registry.edge > 8192) {
        mc2::fmt::print_err("[ICON_REGISTRY] atlas exceeds texture source capacity; using placeholder\n");
        std::erase_if(registry.icons, [](const auto& item) {
            const auto& [key, icon] = item;
            return key != "missing";
        });
        registry.pack();
    }
    texture = mc2::textures::fromRawPixels(std::bit_cast<DWORD*>(registry.atlas.data()), gos_Texture_Alpha, 0, registry.edge, 4, "generated/hud/icon-registry");
    mc2::textures::adoptOwned(texture);
    mc2::fmt::print_out("[ICON_REGISTRY] icons={} atlas={}x{}\n", registry.icons.size(), registry.edge, registry.edge);
    if (false || false) {
        std::filesystem::create_directories("debug_state", ec);
        FILE* dump = fopen("debug_state/icon_registry.tga", "wb");
        if (dump) {
            unsigned char header[18] = {0, 0, 2};
            header[12] = header[14] = registry.edge & 255;
            header[13] = header[15] = registry.edge >> 8;
            header[16] = 32; header[17] = 0x28;
            fwrite(header, 1, sizeof(header), dump);
            fwrite(registry.atlas.data(), sizeof(uint32_t), registry.atlas.size(), dump);
            fclose(dump);
        }
        for (const auto& [key, icon] : registry.icons)
            mc2::fmt::print_out("[ICON_RECT] key={} x={} y={} w={} h={}\n", key, icon.x, icon.y, icon.width, icon.height);
    }
}

const Icon& resolve(std::string key) {
    if (const Icon* icon = registry.find(key)) return *icon;
    const auto [position, inserted] = warned.insert(key);
    if (inserted)
        mc2::fmt::print_err("[ICON_REGISTRY] missing key={} (placeholder)\n", key);
    return *registry.find("missing");
}

// The VFS keys a file by its "data/..." tail; a chassis CSV path may carry the install root in front.
std::string dataKey(std::string_view path) {
    const auto at = path.rfind("data/");
    return std::string(at == std::string_view::npos ? path : path.substr(at));
}
}

// Order: the mod's own art (CSV stem, then the author's MechName) -> its own HUD front art -> a sheet cell the
// same source declares -> the placeholder. One cell, fitted to the template's rect.
bool apply(aObject& widget, const aObject& source, long index, const Subject& subject) {
    const char* name = mc2::textures::name(source.getTextureHandle());
    const std::string sheet = canonical(name ? name : "missing");
    init(sheet, static_cast<int>(source.width()), static_cast<int>(source.height()));
    Query query;
    query.stem = canonical(std::filesystem::path(subject.file).stem().string());
    query.name = canonical(subject.name);
    query.sheet = sheet;
    query.index = index;
    query.dataRank = LookupModPriority(dataKey(subject.file).c_str());
    query.sheetRank = LookupModPriority(sheet.c_str());
    const auto tiered = [&](std::string_view stem, bool vehicle) {
        const auto hud = hudIconSource(Environment.drawableWidth, Environment.screenWidth, vehicle);
        return std::format("hud/{}/front/{}", std::filesystem::path(hud.sheet).stem().string(), stem);
    };
    query.hud = {"hud/front/" + query.stem, tiered(query.stem, false), tiered(query.stem, true), "monohud/" + query.stem, "monohud/" + query.name};
    auto pick = registry.select(namedPriority, query);
    const Icon* icon = pick.icon;
    // A chassis is one chassis whichever source owns its CSV: an MCO Atlas uses stock's Atlas art, an MCO Crab the bt2018 Crab.
    // Match by chassis name across sources, never by VFS rank, before any generic fallback.
    const auto byChassis = [&](std::string_view base) -> const Icon* {
        const std::string chassis(base);
        for (const auto& key : {"hud/front/" + chassis, tiered(base, false), tiered(base, true), "monohud/" + chassis, "monohud/bt2018_" + chassis})
            if (const Icon* found = registry.find(key)) return found;
        return nullptr;
    };
    if (!icon) {
        const std::string base = query.stem.starts_with("bt2018_") ? query.stem.substr(7) : query.stem;
        if ((icon = byChassis(base)) || (icon = byChassis(query.name))) pick.hud = icon;
    }
    if (!icon) {
        if (mc2_diag::tagRequested("ICON_REGISTRY")) {
            const auto [position, inserted] = warned.insert("no-icon:" + query.stem);
            if (inserted)
                mc2::fmt::print_err("[ICON_REGISTRY] no art for chassis {} (class fallback)\n", query.stem);
        }
        // No source has art for this chassis: a stock silhouette of its class (copter, tank or weight class), never the
        // missing-texture checkerboard and never a mech standing in for a vehicle.
        std::string cls = "bushwacker";
        if (subject.vehicle)
            cls = query.name.find("copter") != std::string::npos || query.name.find("vtol") != std::string::npos ? "mediumcopter"
                : query.name.find("trooper") != std::string::npos || query.name.find("armor") != std::string::npos || query.name.find("elemental") != std::string::npos ? "infantry" : "bulldog";
        else
            cls = subject.tons < 40 ? "jenner" : subject.tons < 60 ? "bushwacker" : subject.tons < 80 ? "catapult" : "atlas";
        const Icon* generic = registry.find("hud/front/" + cls);
        if (generic) { icon = generic; pick.hud = generic; }
        else icon = registry.find("missing");
    }
    if (widget.getTextureHandle() != texture) widget.setTexture(mc2::textures::retain(texture));
    widget.setFileWidth(static_cast<float>(registry.edge)); widget.setFileHeight(static_cast<float>(registry.edge));
    // aObject adds 0.1 texel; preserve its old logical-pixel bias on upscaled art.
    const auto scale = pick.hud || pick.own ? AssetScale::Vec2{1.f, 1.f} : sourceScale[sheet];
    widget.setUVs(icon->x + .1f * scale.x - .1f, icon->y + .1f * scale.y - .1f,
                  icon->x + icon->width + .1f * scale.x - .1f, icon->y + icon->height + .1f * scale.y - .1f);
    // The cell is the template's rect.
    fitWidget(widget, static_cast<int>(source.width()), static_cast<int>(source.height()), icon->width, icon->height);
    if (pick.hud) widget.setColor(0xffffffff);
    return pick.hud != nullptr;
}

void blit(std::string sheet, long index, bool back, uint32_t* dest, int stride, int width, int height, std::string chassis) {
    init();
    const std::string key = legacyKey(canonical(sheet) + (back ? "/back" : ""), index);
    const auto stem = canonical(std::filesystem::path(chassis).stem().string());
    // Stock ships one source per mech side (hud/front, hud/back); the draw size is picked below.
    // A mod that still ships tiered hud/<sheet>/... files keeps working; the better VFS rank wins.
    const std::string side = back ? "back" : "front";
    const std::string single = std::format("hud/{}/{}", side, stem);
    const std::string tiered = std::format("hud/{}/{}/{}", canonical(std::filesystem::path(sheet).stem().string()), side, stem);
    const auto rankOf = [](std::string k) {
        return namedPriority.contains(k) && registry.find(k) ? namedPriority.at(k) : 1000001;
    };
    // Authored art for exactly this draw size (hud/<side>/<stem>@WxH) beats resampling the single source.
    const std::string exact = hudSizedKey(side, stem, width, height);
    const auto pick = pickHudSingle(single, rankOf(single), exact, rankOf(exact));
    const int singleRank = pick.rank, tieredRank = rankOf(tiered);
    if (tieredRank < singleRank && !warned.contains("tiered-hud:" + tiered)) {
        warned.insert("tiered-hud:" + tiered);
        mc2::fmt::print_out("[ICON_REGISTRY] tiered mod HUD icon {} overrides the single-source icon\n", tiered);
    }
    const Icon* found = registry.lookupHud(pick.key, tiered, singleRank, tieredRank, key, LookupModPriority(sheet.c_str()));
    // MC2X transport indices also name bay pictures; MCO supplies loose chassis pictures.
    if (!found && !back) {
        const std::string bay = "mech/" + stem;
        const auto bayPriority = namedPriority.find(bay);
        if (bayPriority != namedPriority.end() && namedPriority.at(bay) <= LookupModPriority(sheet.c_str()))
            found = registry.find(bay);
        if (!found) found = registry.find(legacyKey("data/art/mc2x_mechicons.tga", index));
    }
    const auto& icon = found ? *found : resolve(key);
    resampleIcon(icon, dest, stride, width, height);
}

const Icon* pilotIcon(long index, std::string_view legacySheet) {
    init();
    const std::string key = std::format("pilot/{:02}", index);
    const Icon* icon = namedPriority.contains(key) ? registry.find(key) : nullptr;
    if (!icon) return nullptr;
    // Lower VFS rank wins: a mod that replaces the old portrait grid keeps drawing through it.
    if (!legacySheet.empty() && namedPriority.at(key) > cachedSheetRank(legacySheet)) {
        if (!warned.contains("legacy-pilot-grid")) {
            warned.insert("legacy-pilot-grid");
            mc2::fmt::print_out("[ICON_REGISTRY] mod pilot sheet {} overrides single-file portraits (legacy grid)\n", legacySheet);
        }
        return nullptr;
    }
    return icon;
}

bool pilotUV(long index, std::string_view legacySheet, unsigned long& sourceId, float& u0, float& v0, float& u1, float& v1) {
    const Icon* icon = pilotIcon(index, legacySheet);
    if (!icon || !texture || registry.edge <= 0) return false;
    const float edge = static_cast<float>(registry.edge);
    u0 = icon->x / edge; u1 = (icon->x + icon->width) / edge;
    v0 = icon->y / edge; v1 = (icon->y + icon->height) / edge;
    sourceId = mc2::textures::sourceId(texture);
    return true;
}

bool applyPilot(aObject& widget, long index) {
    const char* name = mc2::textures::name(widget.getTextureHandle());
    init();
    const std::string legacy = name ? canonical(name) : std::string{};
    const Icon* icon = pilotIcon(index, legacy);
    if (!icon) return false;
    if (widget.getTextureHandle() != texture) widget.setTexture(mc2::textures::retain(texture));
    widget.setFileWidth(static_cast<float>(registry.edge)); widget.setFileHeight(static_cast<float>(registry.edge));
    widget.setUVs(static_cast<float>(icon->x), static_cast<float>(icon->y),
                  static_cast<float>(icon->x + icon->width), static_cast<float>(icon->y + icon->height));
    return true;
}

const Icon& damageRect(long slot) {
    init();
    if (slot < 0 || slot >= kDamageSlots) return resolve("damage/" + std::to_string(slot));
    return damageIcons[slot];
}

// The page that holds this slot's rect; rect x/y are page-local.
unsigned long damageTexture(long slot) {
    init();
    return mc2::textures::sourceId(damagePages[std::clamp<long>(slot, 0, kDamageSlots - 1) / kDamagePerPage]);
}

void shutdown() {
    if (texture) mc2::textures::releaseOwned(texture);
    for (DWORD& page : damagePages) {
        if (page) mc2::textures::releaseOwned(page);
        page = 0;
    }
    texture = 0; registry = {}; damageIcons.clear(); sourceScale.clear(); namedPriority.clear(); warned.clear();
}
}
