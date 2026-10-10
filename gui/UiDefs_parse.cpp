// UiDefs: defs-file text parsing, path resolution, drawing primitives.
// Declared in UiDefs_internal.h.
#include "UiDefs_internal.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

namespace uidefs_detail {


static std::string trim(const std::string& in)
{
    const char* ws = " \t\r\n";
    const std::size_t first = in.find_first_not_of(ws);
    if (first == std::string::npos)
        return std::string();
    const std::size_t last = in.find_last_not_of(ws);
    return in.substr(first, last - first + 1);
}

std::string toLower(std::string v)
{
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

bool uiDefsTraceEnabled()
{
    return mc2::gate::get<"MC2_UI_DEFS_TRACE">();
}

void uiDefsTrace(const char* fmt, ...)
{
    if (!uiDefsTraceEnabled())
        return;
    std::fprintf(stderr, "[UI_DEFS] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

// UI-DEFS-LOCKSTEP-HARDEN-1: ALWAYS-ON log line for defs/ui page-load and
// render failures. BLACK-MENU-BISECT-1: a version-skewed defs/ui package
// produced a black screen with ZERO log output even under MC2_LOG=1 -- every
// failure path must emit exactly one clear [UI-DEFS] line, unconditionally.
void uiDefsLog(const char* fmt, ...)
{
    std::printf("[UI-DEFS] ");
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

static std::string stripLineComment(const std::string& line)
{
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c == '/' && line[i + 1] == '/')
            return line.substr(0, i);
    }
    return line;
}

static std::string unquote(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        std::string out;
        out.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                const char n = value[++i];
                switch (n) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    default: out.push_back(n); break;
                }
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }
    return value;
}

bool parseInt(const std::string& s, int& out)
{
    try {
        std::size_t pos = 0;
        long v = std::stol(trim(s), &pos, 0);
        if (pos == 0)
            return false;
        out = static_cast<int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parseUInt(const std::string& s, unsigned int& out)
{
    try {
        std::size_t pos = 0;
        unsigned long v = std::stoul(trim(s), &pos, 0);
        if (pos == 0)
            return false;
        out = static_cast<unsigned int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

unsigned int parseColor(const std::unordered_map<std::string, std::string>& f,
                              const char* name,
                              unsigned int fallback)
{
    const auto it = f.find(name);
    if (it == f.end())
        return fallback;
    unsigned int out = fallback;
    parseUInt(it->second, out);
    return out;
}

std::string field(const std::unordered_map<std::string, std::string>& f,
                         const char* name,
                         const char* fallback)
{
    const auto it = f.find(name);
    if (it == f.end())
        return fallback;
    return it->second;
}

Rect parseRect(const std::string& value)
{
    Rect r;
    std::string part;
    std::vector<int> values;
    for (char c : value) {
        if (c == ',') {
            int v = 0;
            parseInt(part, v);
            values.push_back(v);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty() || value.size()) {
        int v = 0;
        parseInt(part, v);
        values.push_back(v);
    }
    if (values.size() >= 4) {
        r.x = values[0];
        r.y = values[1];
        r.w = values[2];
        r.h = values[3];
    }
    return r;
}

bool pointInRect(const Rect& r, int x, int y)
{
    return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h;
}

std::vector<Block> parseTypedFit(const char* path)
{
    std::vector<Block> blocks;
    std::ifstream in(path);
    if (!in)
        return blocks;

    bool inBlock = false;
    Block current;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(stripLineComment(line));
        if (line.empty())
            continue;

        if (!inBlock) {
            const std::size_t brace = line.find('{');
            if (brace == std::string::npos)
                continue;
            current = Block();
            current.type = trim(line.substr(0, brace));
            inBlock = !current.type.empty();
            continue;
        }

        if (line.find('}') != std::string::npos) {
            blocks.push_back(current);
            current = Block();
            inBlock = false;
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string name = trim(line.substr(0, eq));
        if (!name.empty())
            current.fields[name] = unquote(line.substr(eq + 1));
    }
    return blocks;
}


static std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    char buffer[MAX_PATH] = {0};
    const DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        return std::filesystem::path(buffer).parent_path();
#else
    char buffer[4096] = {0};
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path defsRoot()
{
    const std::string explicitRoot = mc2::gate::str<"MC2_DEFS_ROOT">();   // "" when unset or empty
    if (!explicitRoot.empty())
        return std::filesystem::path(explicitRoot);

    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path exe = executableDirectory();
    const std::filesystem::path candidates[] = {
        cwd / "data" / "defs",
        exe / "data" / "defs",
        cwd.parent_path() / "data" / "defs",
        exe.parent_path() / "data" / "defs",
    };

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }

    return cwd / "data" / "defs";
}

bool fileExistsPath(const std::filesystem::path& p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// UI-DEFS-LOCKSTEP-HARDEN-1: defs/ui package version gate. The package ships
// a tiny manifest (data/defs/ui/PACKAGE.fit, "Version = N"); the exe carries
// kExpectedUiPackageVersion (gui/UiDefsLockstep.h). Missing or mismatched =>
// every page load is treated as a failure => LogisticsScreen renders the
// legacy screens (visible, degraded) instead of a silent black quad.
// Checked once per process; result cached. MC2_UI_DEFS_IGNORE_PACKAGE_VERSION=1
// bypasses the gate (modder iteration escape hatch).
bool uiPackageVersionCheckPassed()
{
    static int s_verdict = -1;   // -1 unknown, 0 fail, 1 pass
    if (s_verdict >= 0)
        return s_verdict == 1;

    if (mc2::gate::get<"MC2_UI_DEFS_IGNORE_PACKAGE_VERSION">()) {
        uiDefsLog("package version check BYPASSED (MC2_UI_DEFS_IGNORE_PACKAGE_VERSION set)");
        s_verdict = 1;
        return true;
    }

    const std::filesystem::path manifest = defsRoot() / "ui" / "PACKAGE.fit";
    std::ifstream in(manifest);
    if (!in.is_open()) {
        uiDefsLog("package manifest MISSING: %s (exe expects version %d) -- "
                  "treating data/defs/ui as version-skewed; front-end falls back to legacy UI. "
                  "Deploy matching data/defs with this exe (scripts/deploy_payload.py).",
                  manifest.string().c_str(), UiDefs::kExpectedUiPackageVersion);
        s_verdict = 0;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const int found = UiDefs::parseUiPackageVersionText(text.c_str());
    if (!UiDefs::uiPackageVersionOk(found, UiDefs::kExpectedUiPackageVersion)) {
        uiDefsLog("package version %d, expected %d (%s) -- version-skewed defs/ui package; "
                  "front-end falls back to legacy UI. Deploy matching data/defs with this exe.",
                  found, UiDefs::kExpectedUiPackageVersion, manifest.string().c_str());
        s_verdict = 0;
        return false;
    }
    uiDefsLog("package version %d OK (%s)", found, manifest.string().c_str());
    s_verdict = 1;
    return true;
}

static std::filesystem::path resolveCaseInsensitive(const std::filesystem::path& raw)
{
    if (fileExistsPath(raw))
        return raw;

    std::filesystem::path cur;
    for (const auto& part : raw) {
        if (cur.empty()) {
            cur = part;
            if (!fileExistsPath(cur) && part == raw.root_path())
                continue;
            if (fileExistsPath(cur))
                continue;
        }

        std::filesystem::path candidate = cur / part;
        if (fileExistsPath(candidate)) {
            cur = candidate;
            continue;
        }

        if (!fileExistsPath(cur))
            return raw;

        const std::string wanted = toLower(part.string());
        bool found = false;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
            if (toLower(entry.path().filename().string()) == wanted) {
                cur = entry.path();
                found = true;
                break;
            }
        }
        if (!found)
            return raw;
    }
    return cur;
}

std::filesystem::path resolveAssetPath(const std::string& value)
{
    if (value.empty())
        return std::filesystem::path();

    const std::filesystem::path raw(value);
    if (raw.is_absolute())
        return resolveCaseInsensitive(raw);

    std::vector<std::filesystem::path> roots;
    auto addRoot = [&roots](std::filesystem::path p) {
        std::error_code ec;
        p = std::filesystem::weakly_canonical(p, ec);
        if (ec)
            p = p.lexically_normal();
        if (std::find(roots.begin(), roots.end(), p) == roots.end())
            roots.push_back(p);
    };

    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path exe = executableDirectory();

    for (int i = 0; i < 8; ++i) {
        addRoot(cwd);
        addRoot(exe);
        if (!cwd.has_parent_path() && !exe.has_parent_path())
            break;
        cwd = cwd.parent_path();
        exe = exe.parent_path();
    }

    for (const std::filesystem::path& root : roots) {
        const std::filesystem::path candidate = resolveCaseInsensitive(root / raw);
        if (fileExistsPath(candidate))
            return candidate;
    }

    return resolveCaseInsensitive(raw);
}

void traceMissingTextureOnce(const std::string& texturePath, const std::filesystem::path& resolved)
{
    if (!uiDefsTraceEnabled())
        return;
    static std::unordered_map<std::string, bool> s_seen;
    if (s_seen[texturePath])
        return;
    s_seen[texturePath] = true;
    uiDefsTrace("missing UI texture: requested=%s resolved=%s",
                texturePath.c_str(), resolved.string().c_str());
}

bool readUiTextureSize(const std::filesystem::path& path, int& width, int& height)
{
    width = 0;
    height = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    unsigned char header[32] = {0};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    const std::streamsize got = in.gcount();
    if (got >= 24 &&
        header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') {
        width = (static_cast<int>(header[16]) << 24) |
                (static_cast<int>(header[17]) << 16) |
                (static_cast<int>(header[18]) << 8) |
                static_cast<int>(header[19]);
        height = (static_cast<int>(header[20]) << 24) |
                 (static_cast<int>(header[21]) << 16) |
                 (static_cast<int>(header[22]) << 8) |
                 static_cast<int>(header[23]);
        return width > 0 && height > 0;
    }

    if (got >= 18) {
        const std::string ext = toLower(path.extension().string());
        if (ext == ".tga") {
            width = static_cast<int>(header[12]) | (static_cast<int>(header[13]) << 8);
            height = static_cast<int>(header[14]) | (static_cast<int>(header[15]) << 8);
            return width > 0 && height > 0;
        }
    }

    return false;
}

std::string baseNameNoExt(const char* path)
{
    if (!path)
        return std::string();
    std::filesystem::path p(path);
    return toLower(p.stem().string());
}

int alignmentFromString(const std::string& value)
{
    const std::string v = toLower(value);
    if (v == "right")
        return 1;
    if (v == "center" || v == "centre")
        return 2;
    return 0;
}

int parseLegacyButtonIndex(const std::unordered_map<std::string, std::string>& f)
{
    std::string section = field(f, "legacySection");
    if (section.empty())
        section = field(f, "sourceControlType");
    const std::string prefix = "Button";
    if (section.rfind(prefix, 0) != 0)
        return -1;

    int index = -1;
    if (parseInt(section.substr(prefix.size()), index))
        return index;
    return -1;
}

static void drawQuad(const Rect& r, unsigned int color, unsigned int textureHandle,
                     float u1 = 0.0f, float v1 = 0.0f, float u2 = 1.0f, float v2 = 1.0f)
{
    gos_VERTEX verts[4];
    verts[0].x = static_cast<float>(r.x);       verts[0].y = static_cast<float>(r.y);
    verts[1].x = static_cast<float>(r.x);       verts[1].y = static_cast<float>(r.y + r.h);
    verts[2].x = static_cast<float>(r.x + r.w); verts[2].y = static_cast<float>(r.y + r.h);
    verts[3].x = static_cast<float>(r.x + r.w); verts[3].y = static_cast<float>(r.y);

    verts[0].u = u1; verts[0].v = v1;
    verts[1].u = u1; verts[1].v = v2;
    verts[2].u = u2; verts[2].v = v2;
    verts[3].u = u2; verts[3].v = v1;

    for (int i = 0; i < 4; ++i) {
        verts[i].argb = color;
        verts[i].frgb = 0;
        verts[i].z = 0.0f;
        verts[i].rhw = 0.5f;
    }

    gos_SetRenderState(gos_State_Texture, textureHandle);
    gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
    gos_SetRenderState(gos_State_Filter, gos_FilterNone);
    gos_SetRenderState(gos_State_AlphaTest, true);
    gos_SetRenderState(gos_State_TextureAddress, gos_TextureClamp);
    gos_SetRenderState(gos_State_TextureMapBlend, gos_BlendModulateAlpha);
    gos_DrawQuads(verts, 4);
}

static void drawLineRect(const Rect& r, unsigned int color)
{
    gos_VERTEX verts[5];
    const float x1 = static_cast<float>(r.x);
    const float y1 = static_cast<float>(r.y);
    const float x2 = static_cast<float>(r.x + r.w);
    const float y2 = static_cast<float>(r.y + r.h);
    verts[0].x = x1; verts[0].y = y1;
    verts[1].x = x2; verts[1].y = y1;
    verts[2].x = x2; verts[2].y = y2;
    verts[3].x = x1; verts[3].y = y2;
    verts[4].x = x1; verts[4].y = y1;
    for (int i = 0; i < 5; ++i) {
        verts[i].u = verts[i].v = 0.0f;
        verts[i].argb = color;
        verts[i].frgb = 0;
        verts[i].z = 0.0f;
        verts[i].rhw = 0.5f;
    }
    gos_SetRenderState(gos_State_Texture, 0);
    gos_DrawLines(verts, 5);
}


} // namespace uidefs_detail
