#include "icon_registry.h"
#include "asystem.h"
#include "file.h"
#include "inifile.h"
#include "fastfile.h"
#include "asset_scale.h"
#include "../mclib/texture_assets.h"
#include "../RenderCore/KtxLoader.h"
#include "../GameOS/gameos/texture_decode.h"
#include "mc2fmt.h"
#include "Gates/gate.h"
#include <filesystem>
#include <set>
#include <bit>

namespace unit_icons {
namespace {
Registry registry;
DWORD texture = 0;
std::set<std::string> warned;
std::map<std::string, AssetScale::Vec2> sourceScale;

struct Pixels { int w = 0, h = 0; DWORD logicalW = 0, logicalH = 0; std::vector<uint32_t> data; };
Pixels load(std::string path, bool hud = false) {
    Pixels out;
    if (!fileExists(path.c_str())) return out;
    if (hud) {
        // Damage colours are exact palette values; a cooked BC7 sidecar must not quantize them.
        File file;
        if (file.open(path.c_str()) != NO_ERR || file.getLength() <= 0 || file.getLength() > 64 * 1024 * 1024) return out;
        std::vector<uint8_t> bytes(file.getLength());
        if (file.read(bytes.data(), long(bytes.size())) != bytes.size()) return out;
        auto image = mc2tex::decodeMemory(bytes.data(), bytes.size(), path.c_str(), gos_Texture_Alpha, 0);
        if (!image) return out;
        out.w = image->width; out.h = image->height;
        out.data.resize(size_t(out.w) * out.h);
        const int stride = image->layout == mc2tex::PixelLayout::RGB8 ? 3 : 4;
        for (size_t i = 0; i < out.data.size(); ++i) {
            const auto* p = image->pixels.data() + i * stride;
            out.data[i] = (uint32_t(stride == 4 ? p[3] : 255) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
        return out;
    }
    const DWORD handle = mc2::textures::load(path.c_str(), gos_Texture_Keyed, 0);
    auto image = mc2::textures::image(handle);
    mc2::textures::logicalSize(handle, out.logicalW, out.logicalH);
    mc2::textures::release(handle);
    if (!image || image->isCompressed || image->pixels.size() < size_t(image->width) * image->height * 4) return out;
    out.w = image->width; out.h = image->height;
    out.data.resize(size_t(out.w) * out.h);
    for (size_t i = 0; i < out.data.size(); ++i) {
        const auto* p = image->pixels.data() + i * 4;
        out.data[i] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
    return out;
}

void addSheet(std::string path, int cw, int ch, int stripRows = 0) {
    if (!stripRows && sourceScale.contains(path)) return;
    auto src = load(path, stripRows != 0);
    if (src.data.empty()) return;
    if (stripRows) {
        // Height declares the authored scale; width may add or remove icon columns.
        const float scale = float(src.h) / (ch * stripRows);
        const AssetScale::Vec2 f{scale, scale};
        int cols = int(src.w / (cw * f.x) + .01f);
        int rows = stripRows;
        while (cols > 0 && int((cols - 1) * cw * f.x) + int((cw - 1) * f.x) >= src.w) --cols;
        while (rows > 0 && int((rows - 1) * ch * f.y) + int((ch - 1) * f.y) >= src.h) --rows;
        const int w = cols * cw, h = rows * ch;
        std::vector<uint32_t> sampled(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int sx = int((x / cw) * cw * f.x) + int((x % cw) * f.x);
                const int sy = int((y / ch) * ch * f.y) + int((y % ch) * f.y);
                sampled[size_t(y) * w + x] = src.data[size_t(sy) * src.w + sx];
            }
        registry.sheet(path, sampled, w, h, cw, ch, true);
    } else {
        const float sx = src.logicalW ? float(src.w) / src.logicalW : 1.f;
        const float sy = src.logicalH ? float(src.h) / src.logicalH : 1.f;
        sourceScale[path] = {sx, sy};
        registry.sheet(path, src.data, src.w, src.h, int(cw * sx), int(ch * sy));
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
    addSheet(path, int(w), int(h));
}

void collect(const char* key, void* data) {
    auto path = canonical(key);
    if ((path.ends_with(".tga") || path.ends_with(".png")) && path.starts_with("data/art/mechicons/"))
        static_cast<std::set<std::string>*>(data)->insert(std::move(path));
}

void init(std::string first = {}, int cw = 25, int ch = 30) {
    if (texture) return;
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
    addSheet("data/art/mcui_low4.tga", 25, 30, 2);
    addSheet("data/art/mcui_med4.tga", 32, 38, 2);
    addSheet("data/art/mcui_high7.tga", 40, 48, 2);
    addSheet("data/art/mcui_low5.tga", 25, 30, 1);
    addSheet("data/art/mcui_med5.tga", 32, 38, 1);
    addSheet("data/art/mcui_high8.tga", 40, 48, 1);

    std::set<std::string> loose;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it("data/art/mechicons", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) collect(it->path().generic_string().c_str(), &loose);
    EnumerateModFiles("data/art/mechicons/", collect, &loose);
    for (long i = 0; i < numFastFiles; ++i) {
        const auto* info = fastFiles[i]->getFilesInfo();
        for (long j = 0; j < fastFiles[i]->getNumFiles(); ++j) collect(info[j].pfe->name, &loose);
    }
    std::map<std::string, int> namedPriority;
    for (const auto& path : loose) {
        const std::string key = "mech/" + std::filesystem::path(path).stem().string();
        const int priority = LookupModPriority(path.c_str());
        if (namedPriority.contains(key) && namedPriority[key] <= priority) continue;
        auto src = load(path);
        if (!src.data.empty()) {
            registry.add(key, src.w, src.h, src.data);
            namedPriority[key] = priority;
        }
    }
    // Per-instance damage masks occupy ordinary packed rects in the same atlas.
    const std::vector<uint32_t> blank(40 * 48, 0);
    for (int i = 0; i < 240; ++i) registry.add("damage/" + std::to_string(i), 40, 48, blank);
    if (!registry.pack() || registry.edge > 8192) {
        mc2::fmt::print_err("[ICON_REGISTRY] atlas exceeds texture source capacity; using placeholder\n");
        std::erase_if(registry.icons, [](const auto& item) {
            const auto& [key, icon] = item;
            return key != "missing" && !key.starts_with("damage/");
        });
        registry.pack();
    }
    texture = mc2::textures::fromRawPixels(std::bit_cast<DWORD*>(registry.atlas.data()), gos_Texture_Alpha, 0, registry.edge);
    mc2::textures::adoptOwned(texture);
    mc2::fmt::print_out("[ICON_REGISTRY] icons={} atlas={}x{}\n", registry.icons.size(), registry.edge, registry.edge);
    if (mc2::gate::get<"MC2_LOG_MECH_ICON">() || mc2::gate::get<"MC2_LOG_MECHICON">()) {
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
}

void apply(aObject& widget, const aObject& source, long index, std::string chassis) {
    const char* name = mc2::textures::name(source.getTextureHandle());
    const std::string sheet = canonical(name ? name : "missing");
    init(sheet, int(source.width()), int(source.height()));
    const std::string named = "mech/" + canonical(std::filesystem::path(chassis).stem().string());
    const bool individual = registry.find(named) != nullptr;
    const auto& icon = resolve(individual ? named : legacyKey(sheet, index));
    if (widget.getTextureHandle() != texture) widget.setTexture(mc2::textures::retain(texture));
    widget.setFileWidth(float(registry.edge)); widget.setFileHeight(float(registry.edge));
    // aObject adds 0.1 texel; preserve its old logical-pixel bias on upscaled art.
    const auto scale = individual ? AssetScale::Vec2{1.f, 1.f} : sourceScale[sheet];
    widget.setUVs(icon.x + .1f * scale.x - .1f, icon.y + .1f * scale.y - .1f,
                  icon.x + icon.width + .1f * scale.x - .1f, icon.y + icon.height + .1f * scale.y - .1f);
}

void blit(std::string sheet, long index, bool back, uint32_t* dest, int stride, int width, int height, std::string chassis) {
    init();
    const std::string key = legacyKey(canonical(sheet) + (back ? "/back" : ""), index);
    const Icon* found = registry.find(key);
    // MC2X transport indices also name bay pictures; MCO supplies loose chassis pictures.
    if (!found && !back) {
        found = registry.find("mech/" + canonical(std::filesystem::path(chassis).stem().string()));
        if (!found) found = registry.find(legacyKey("data/art/mc2x_mechicons.tga", index));
    }
    const auto& icon = found ? *found : resolve(key);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            dest[size_t(y) * stride + x] = icon.pixels[size_t(y * icon.height / height) * icon.width + x * icon.width / width];
}

const Icon& damageRect(long slot) {
    init();
    return resolve("damage/" + std::to_string(slot));
}

unsigned long damageTexture() { init(); return mc2::textures::sourceId(texture); }

void shutdown() {
    if (texture) mc2::textures::releaseOwned(texture);
    texture = 0; registry = {}; sourceScale.clear(); warned.clear();
}
}
