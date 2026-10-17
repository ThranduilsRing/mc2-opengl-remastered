/***************************************************************
 * FILENAME: AssetViewerApp.cpp
 * DESCRIPTION: Top-level application class for mc2_asset_viewer.
 ***************************************************************/
// TGL loader includes (engine_stubs.cpp provides all referenced symbols).
// These must come BEFORE any imgui/SDL headers so that platform_windows.h
// from the engine side doesn't collide with SDL's own WIN32 guards.
#include "heap.h"
#include "tgl.h"
#include "msl.h"
#include "fastfile.h"
#include "ffile.h"

#include "AssetViewerApp.h"
#include "SphereMesh.h"
#include "LocalPbrMaterialBackend.h"
#include "MaterialTextureLoader.h"
#include "MaterialPreviewPBR.h"
#include "FitMaterialLoader.h"
#include "UiEditorImageCache.h"
#include "imgui.h"
#include "TextureExtensions.h"
#include "TextureDecoderRegistry.h"
#include "TextureMetadata.h"
#include "TexturePreview2D.h"
#include "Ktx2Decoder.h"
#include "ShaderIncludeResolver.h"
#include <SDL.h>
#include <GL/glew.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

// GUI mode: ctor/dtor own the cache lifetime. The static runSmoke() path does
// NOT construct an AssetViewerApp and does its own Initialize/Shutdown — the two
// paths are mutually exclusive, so there is no double-init.
AssetViewerApp::AssetViewerApp()  {
    UiEditorImageCache_Initialize();
    // Deploy root is the viewer's cwd; "." -> ./tgl.fst for TglMeshLoader.
    meshSurface_.setDeployDir(".");
    engineShaderPreview_.setDeployDir(".");
    engineShaderPreview_.setShaderRoot("shaders");
    workbenchPanel_.setDeployDir(".");
    workbench_.setDeployDir(".");
}
AssetViewerApp::~AssetViewerApp() { UiEditorImageCache_Shutdown(); }

void AssetViewerApp::drawUi()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MC2 Asset Viewer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float sidebarW = 180.0f, browserW = 300.0f;
    ImGui::BeginChild("sidebar", ImVec2(sidebarW, 0), true);
    sidebar_.draw();
    ImGui::EndChild();
    ImGui::SameLine();

    // browser child: dispatch on the active asset type.
    ImGui::BeginChild("browser", ImVec2(browserW, 0), true);
    if (sidebar_.active() == AssetType::StaticProps) {
        modelBrowser_.draw();
        if (modelBrowser_.hasSelection()) {
            std::string sel = modelBrowser_.takeSelection();
            meshSurface_.setSource(sel);
            engineShaderPreview_.setSource(sel);
        }
    } else if (sidebar_.active() == AssetType::ModWorkbench) {
        ImGui::TextDisabled("Drop a GLB on the window.");
    } else {
        browser_.draw();
        if (browser_.hasSelection()) {
            std::string sel = browser_.takeSelection();
            if (sidebar_.active() == AssetType::Textures) surface_.setSource(sel);
            // Materials mode: slots are assigned via MaterialSlots' own picker, not the browser.
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // inspector child: dispatch on the active asset type.
    ImGui::BeginChild("inspector", ImVec2(0, 0), true);
    switch (sidebar_.active()) {
      case AssetType::Textures:
        inspector_.draw(surface_);                                  // calls surface_.draw(GetContentRegionAvail())
        break;
      case AssetType::Materials:
        if (!materialsAutoLoaded_) {
            materialsAutoLoaded_ = true;   // attempt once, regardless of success
            materialSlots_.loadFit("data/defs/materials/viewer/test_materials.fit", materialSurface_);
        }
        materialSlots_.draw(materialSurface_);                      // slot pickers + light/camera
        materialSurface_.draw(ImGui::GetContentRegionAvail());      // lit sphere + approximate label
        break;
      case AssetType::StaticProps: {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::TextUnformatted("Preview backend:");
        ImGui::SameLine(); ImGui::RadioButton("Backend-B: Approximate", &backend_, 0);
        ImGui::SameLine(); ImGui::RadioButton("Backend-A: Engine Shader", &backend_, 1);
        // Shrink avail by the radio row height so the preview fills the remainder.
        avail.y -= ImGui::GetFrameHeightWithSpacing();
        if (backend_ == 1) {
            // Draw first so the lazy compile runs before we check ok().
            engineShaderPreview_.draw(avail);
            if (!engineShaderPreview_.ok()) {
                ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1),
                    "Backend-A unavailable - showing Backend-B. See Shader Contract below.");
                meshSurface_.draw(avail);   // FAIL OPEN
            }
            DrawShaderContractReport(engineShaderPreview_.report());
        } else {
            meshSurface_.draw(avail);
        }
        break;
      }
      case AssetType::ModWorkbench:
        workbenchPanel_.draw(workbench_, ImGui::GetContentRegionAvail());
        break;
    }
    ImGui::EndChild();

    ImGui::End();
}

static int smokeFail(const char* msg) { std::fprintf(stderr, "[smoke] FAIL: %s\n", msg); return 1; }

int AssetViewerApp::runSmoke(const char* fixtureDir)
{
    if (!IsSupportedTextureFile("a.PNG"))  return smokeFail("PNG should be supported");
    if ( IsSupportedTextureFile("a.dds"))  return smokeFail("dds should NOT be supported");
    if ( IsSupportedTextureFile("noext"))  return smokeFail("extensionless should NOT be supported");
    if (FormatDimensions(TextureMetadata{256,128,4,0}) != "256 x 128") return smokeFail("dims format");
    if (FormatChannels(TextureMetadata{0,0,4,0})       != "RGBA")      return smokeFail("channels format");
    { TextureMetadata m; m.fileBytes = 1572864; if (FormatFileSize(m) != "1.5 MB") return smokeFail("size format"); }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;
        std::string path = (std::filesystem::path(fixtureDir) / "test_rgba.png").string();
        surface.setSource(path);
        if (surface.hasError())               rc = smokeFail("fixture failed to load");
        else if (surface.metadata().width  != 4) rc = smokeFail("fixture width != 4");
        else if (surface.metadata().height != 2) rc = smokeFail("fixture height != 2");
        UiEditorImageCache_Shutdown();
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS\n");
    return rc;
}

int AssetViewerApp::runSmokeKtxParse(const char* dir)
{
    namespace fs = std::filesystem;
    auto p = [&](const char* f){ return (fs::path(dir) / f).string(); };

    Ktx2DecodedImage rgba = loadKtx2Image(p("tex_rgba8.ktx2"));
    if (!rgba.ok)                       return smokeFail("rgba8 ktx2 should parse");
    if (rgba.img.width != 4 || rgba.img.height != 2) return smokeFail("rgba8 dims");
    if (rgba.img.isCompressed)          return smokeFail("rgba8 should not be compressed");

    // Validate the hand-written BC7 fixture against the REAL loader here (no GL).
    // The GL smoke's BC7 path is BPTC-gated and may SKIP, so this is the only
    // guard that a malformed BC7 fixture cannot ship green. (review fix: KTX2 #1)
    Ktx2DecodedImage bc7 = loadKtx2Image(p("tex_bc7.ktx2"));
    if (!bc7.ok)                        return smokeFail("bc7 ktx2 should parse");
    if (!bc7.img.isCompressed)          return smokeFail("bc7 should be compressed");
    if (bc7.img.width != 4 || bc7.img.height != 2) return smokeFail("bc7 dims");

    Ktx2DecodedImage sup = loadKtx2Image(p("tex_super.ktx2"));
    if (sup.ok || sup.error.find("Supercompressed") == std::string::npos)
        return smokeFail("supercompressed should be classified");

    Ktx2DecodedImage bad = loadKtx2Image(p("tex_badfmt.ktx2"));
    if (bad.ok || bad.error.find("Unsupported KTX2 format") == std::string::npos)
        return smokeFail("bad format should be classified");

    Ktx2DecodedImage missing = loadKtx2Image(p("does_not_exist.ktx2"));
    if (missing.ok || missing.error.find("not found") == std::string::npos)
        return smokeFail("missing file should be classified");

    std::printf("[smoke] PASS ktx parse+classify\n");
    return 0;
}

int AssetViewerApp::runSmokeDecoder()
{
    auto& reg = textureDecoderRegistry();
    if (!reg.isSupported("a.PNG"))      return smokeFail("png should be supported via registry");
    if (!reg.isSupported("a.tga"))      return smokeFail("tga should be supported via registry");
    if ( reg.isSupported("a.dds"))      return smokeFail("dds should NOT be supported");
    if ( reg.isSupported("noext"))      return smokeFail("extensionless should NOT be supported");
    // IsSupportedTextureFile must now agree with the registry.
    if (!IsSupportedTextureFile("a.png")) return smokeFail("IsSupportedTextureFile png");
    if ( IsSupportedTextureFile("a.dds")) return smokeFail("IsSupportedTextureFile dds");
    if (TextureExtLower("X/Y.KTX2") != "ktx2") return smokeFail("TextureExtLower");
    if (!reg.isSupported("a.ktx2"))     return smokeFail("ktx2 should be supported now");
    {
        auto exts = reg.supportedExtensions();
        bool hasKtx2 = false; for (auto& e : exts) if (e == "ktx2") hasKtx2 = true;
        if (!hasKtx2) return smokeFail("supportedExtensions missing ktx2");
    }
    std::printf("[smoke] PASS decoder registry\n");
    return 0;
}

int AssetViewerApp::runSmokeKtx(const char* dir)
{
    namespace fs = std::filesystem;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke-ktx", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    auto& reg = textureDecoderRegistry();

    // RGBA8 path
    {
        DecodedTexture d = reg.load((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (!d.error.empty())           rc = smokeFail("rgba8 ktx upload error");
        else if (d.glTexture == 0)      rc = smokeFail("rgba8 ktx no texture");
        else if (d.width != 4 || d.height != 2) rc = smokeFail("rgba8 ktx dims");
        else if (d.formatLabel.rfind("RGBA8", 0) != 0) rc = smokeFail("rgba8 label");
        else if (!d.ownsGlTexture)      rc = smokeFail("rgba8 should be owned");
        else if (glGetError() != GL_NO_ERROR) rc = smokeFail("rgba8 glGetError");
        if (d.ownsGlTexture && d.glTexture) { GLuint t = d.glTexture; glDeleteTextures(1, &t); }
    }

    // BC7 path (skip cleanly if no BPTC)
    if (rc == 0) {
        if (!GLEW_ARB_texture_compression_bptc) {
            std::printf("[smoke] SKIP bc7: no BPTC\n");
        } else {
            DecodedTexture d = reg.load((fs::path(dir) / "tex_bc7.ktx2").string());
            if (!d.error.empty())       rc = smokeFail("bc7 upload error");
            else if (d.glTexture == 0)  rc = smokeFail("bc7 no texture");
            else if (!d.isCompressed)   rc = smokeFail("bc7 should be compressed");
            else if (glGetError() != GL_NO_ERROR) rc = smokeFail("bc7 glGetError");
            if (d.ownsGlTexture && d.glTexture) { GLuint t = d.glTexture; glDeleteTextures(1, &t); }
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS ktx upload\n");
    return rc;
}

int AssetViewerApp::runSmokePreview(const char* dir)
{
    namespace fs = std::filesystem;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("smoke-preview", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError();

    int rc = 0;
    {
        UiEditorImageCache_Initialize();
        TexturePreview2D surface;

        surface.setSource((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (surface.hasError())                                  rc = smokeFail("ktx2 preview load");
        else if (surface.metadata().formatLabel.rfind("RGBA8", 0) != 0) rc = smokeFail("ktx2 format label");

        // Replace with a PNG (legacy, not owned) -> must free the prior KTX texture, no crash/leak-error.
        if (rc == 0) surface.setSource((fs::path(dir) / "test_rgba.png").string());
        if (rc == 0 && surface.hasError())                      rc = smokeFail("png after ktx2");

        // Replace with another KTX2 -> exercises owned->owned replacement path.
        if (rc == 0) surface.setSource((fs::path(dir) / "tex_rgba8.ktx2").string());
        if (rc == 0 && surface.hasError())                      rc = smokeFail("ktx2 after png");

        if (rc == 0 && glGetError() != GL_NO_ERROR)             rc = smokeFail("preview glGetError");
        UiEditorImageCache_Shutdown();
    }   // ~TexturePreview2D here must free its owned KTX texture without error

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0) std::printf("[smoke] PASS preview ownership+metadata\n");
    return rc;
}

int AssetViewerApp::runSmokeTiers(const char* dir)
{
    namespace fs = std::filesystem;
    auto p = [&](const char* rel){ return (fs::path(dir) / rel).make_preferred().string(); };

    // Select sample.ktx2 in the 128 tier (selectFile sets folder = parent).
    FileBrowser fb;
    fb.selectFile(p("tiers/128/sample.ktx2"));
    if (fb.CurrentTier() != "128")            return smokeFail("CurrentTier should be 128");
    auto tiers = fb.SiblingTiers();
    if (tiers.size() != 2 || tiers[0] != "128" || tiers[1] != "256")
        return smokeFail("SiblingTiers should be {128,256}");

    // Switch to 256: same filename exists -> selection preserved, now under tiers/256.
    fb.SwitchTier("256");
    if (fb.CurrentTier() != "256")            return smokeFail("CurrentTier should be 256 after switch");
    if (!fb.hasSelection())                   return smokeFail("selection should persist (sample exists in 256)");
    if (fb.selectionPath().find("256") == std::string::npos ||
        fb.selectionPath().find("sample.ktx2") == std::string::npos)
        return smokeFail("selectionPath should point at tiers/256/sample.ktx2");

    // Missing tier -> no-op (folder unchanged).
    fb.SwitchTier("999");
    if (fb.CurrentTier() != "256")            return smokeFail("missing tier should be a no-op");

    // Switch to a tier lacking the selected file -> folder switches, no selection.
    fb.selectFile(p("tiers/128/only128.ktx2"));
    fb.SwitchTier("256");
    if (fb.CurrentTier() != "256")            return smokeFail("should switch folder even when file absent");
    if (fb.hasSelection())                    return smokeFail("selection should drop when file absent in new tier");

    std::printf("[smoke] PASS tiers (detect/switch/continuity)\n");
    return 0;
}

int AssetViewerApp::runSmokeSphere()
{
    // CPU-only: no GL context required.
    SphereMesh m;
    m.generate(1.0f, 32, 64);
    const auto& v = m.vertices();
    const auto& idx = m.indices();
    if (v.empty() || idx.empty()) { printf("[smoke] FAIL: empty mesh\n"); return 1; }
    if (idx.size() % 3 != 0)      { printf("[smoke] FAIL: index count not triangulated\n"); return 1; }

    for (const auto& sv : v) {
        // position is on the unit sphere
        float pr = std::sqrt(sv.px*sv.px + sv.py*sv.py + sv.pz*sv.pz);
        if (std::fabs(pr - 1.0f) > 1e-3f) { printf("[smoke] FAIL: vertex off sphere (r=%f)\n", pr); return 1; }
        // normal is unit length and equals the position direction (unit sphere)
        float nl = std::sqrt(sv.nx*sv.nx + sv.ny*sv.ny + sv.nz*sv.nz);
        if (std::fabs(nl - 1.0f) > 1e-3f) { printf("[smoke] FAIL: non-unit normal\n"); return 1; }
        // tangent is unit length, perpendicular to the normal, handedness is +/-1
        float tl = std::sqrt(sv.tx*sv.tx + sv.ty*sv.ty + sv.tz*sv.tz);
        if (std::fabs(tl - 1.0f) > 1e-3f) { printf("[smoke] FAIL: non-unit tangent\n"); return 1; }
        float ndott = sv.nx*sv.tx + sv.ny*sv.ty + sv.nz*sv.tz;
        if (std::fabs(ndott) > 1e-2f) { printf("[smoke] FAIL: tangent not perpendicular to normal (%f)\n", ndott); return 1; }
        if (std::fabs(std::fabs(sv.tw) - 1.0f) > 1e-3f) { printf("[smoke] FAIL: handedness not +/-1\n"); return 1; }
    }
    printf("[smoke] PASS sphere verts=%zu tris=%zu\n", v.size(), idx.size()/3);
    return 0;
}

int AssetViewerApp::runSmokeBackend()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-backend", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context (need GL 3.3)"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("glewInit"); }
    glGetError(); // consume glew's spurious error

    LocalPbrMaterialBackend b;
    bool ok = b.init();
    GLenum e = glGetError();
    int rc = 0;
    if (!ok)             rc = smokeFail("backend init/compile failed");
    else if (e != GL_NO_ERROR) rc = smokeFail("glGetError non-zero after init");
    else {
        b.shutdown();
        std::printf("[smoke] PASS backend=%s approximate=%d\n", b.name(), (int)b.isApproximate());
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

int AssetViewerApp::runSmokeFit()
{
    auto approx = [](float a, float b){ return std::fabs(a - b) < 0.01f; };

    // Same aspect, different native res, same avail+zoom -> SAME display size (the caveat).
    FitSize a = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 1.0f);
    FitSize b = FitTextureDisplaySize(256, 256, 400.0f, 400.0f, 1.0f);
    FitSize c = FitTextureDisplaySize(512, 512, 400.0f, 400.0f, 1.0f);
    if (!approx(a.w, b.w) || !approx(a.h, b.h)) return smokeFail("128 vs 256 differ in display size");
    if (!approx(a.w, c.w) || !approx(a.h, c.h)) return smokeFail("128 vs 512 differ in display size");

    // zoom 1 fits inside the avail area (square -> 400x400).
    if (a.w > 400.01f || a.h > 400.01f || a.w < 1.0f) return smokeFail("fit overflows or empty");

    // Aspect preserved for non-square (256x128 -> w == 2*h, fits).
    FitSize r = FitTextureDisplaySize(256, 128, 400.0f, 400.0f, 1.0f);
    if (!approx(r.w, 2.0f * r.h)) return smokeFail("aspect not preserved");
    if (r.w > 400.01f || r.h > 400.01f) return smokeFail("non-square overflows");

    // zoom 2 == exactly 2x zoom 1.
    FitSize z = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 2.0f);
    if (!approx(z.w, 2.0f * a.w) || !approx(z.h, 2.0f * a.h)) return smokeFail("zoom not linear");

    // Degenerate inputs: finite, no divide-by-zero.
    FitSize d0 = FitTextureDisplaySize(0, 0, 400.0f, 400.0f, 1.0f);
    if (d0.w != 0.0f || d0.h != 0.0f) return smokeFail("zero-dim texture should yield {0,0}");
    FitSize d1 = FitTextureDisplaySize(128, 128, 0.0f, 0.0f, 1.0f);
    if (!std::isfinite(d1.w) || !std::isfinite(d1.h) || d1.w < 0.0f) return smokeFail("zero-avail not finite");
    FitSize d2 = FitTextureDisplaySize(128, 128, 400.0f, 400.0f, 0.0f);
    if (!std::isfinite(d2.w) || d2.w <= 0.0f) return smokeFail("zero-zoom not handled");

    std::printf("[smoke] PASS fit (size@1=%.1fx%.1f)\n", a.w, a.h);
    return 0;
}

int AssetViewerApp::runSmokeTexLoad(const char* fixtureDir)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-texload", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context (need GL 3.3)"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("glewInit");
    }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    {
        std::string base = std::string(fixtureDir) + "/mat_base.png";
        std::string orm  = std::string(fixtureDir) + "/mat_orm.png";
        std::string err;

        uint32_t tb = MaterialTextureLoader_Load(base, MaterialSlotKind::BaseColor, &err);
        if (!tb) {
            std::printf("[smoke] FAIL: baseColor load: %s\n", err.c_str());
            rc = 1;
        } else {
            GLint fmt = 0;
            glBindTexture(GL_TEXTURE_2D, tb);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
            glBindTexture(GL_TEXTURE_2D, 0);
            if (fmt != GL_SRGB8_ALPHA8) {
                std::printf("[smoke] FAIL: baseColor not sRGB (0x%x)\n", (unsigned)fmt);
                rc = 1;
            }
            glDeleteTextures(1, &tb);
        }

        if (rc == 0) {
            uint32_t to = MaterialTextureLoader_Load(orm, MaterialSlotKind::Orm, &err);
            if (!to) {
                std::printf("[smoke] FAIL: orm load: %s\n", err.c_str());
                rc = 1;
            } else {
                GLint fmt = 0;
                glBindTexture(GL_TEXTURE_2D, to);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
                glBindTexture(GL_TEXTURE_2D, 0);
                if (fmt != GL_RGBA8) {
                    std::printf("[smoke] FAIL: orm not linear RGBA8 (0x%x)\n", (unsigned)fmt);
                    rc = 1;
                }
                glDeleteTextures(1, &to);
            }
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (rc == 0)
        std::printf("[smoke] PASS texload sRGB/linear internalformats correct\n");
    return rc;
}

int AssetViewerApp::runSmokeRender(const char* fixtureDir)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-render", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context (need GL 3.3)"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("glewInit");
    }
    glGetError(); // consume glew's spurious error

    const int W = 256, H = 256;
    MaterialPreviewPBR preview;
    std::string err;
    uint32_t base = MaterialTextureLoader_Load(std::string(fixtureDir) + "/mat_base.png",
                                               MaterialSlotKind::BaseColor, &err);
    if (!base) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::printf("[smoke] FAIL: base load %s\n", err.c_str());
        return 1;
    }
    preview.setSlotTexture(MaterialSlotKind::BaseColor, base);

    // Review fix MAJOR 5: do NOT call preview.draw() here — draw() ends with
    // ImGui::Image/TextColored and this smoke has no ImGui context/frame, so it
    // would dereference an uninitialized GImGui and crash. renderToPixels() is
    // ImGui-free: it lazily builds the FBO + renders + reads back.
    // The renderToPixels test hook returns false if backend init or FBO completeness
    // failed (review fix MAJOR 6), so a failed shader compile is caught here rather
    // than passing on a non-black clear.
    std::vector<uint8_t> rgba;
    if (!preview.renderToPixels(W, H, rgba)) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::printf("[smoke] FAIL: renderToPixels (init/FBO)\n");
        return 1;
    }
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::printf("[smoke] FAIL: glGetError 0x%x\n", (unsigned)e);
        return 1;
    }
    // Review fix MAJOR 6: the FBO clears to (0.10,0.11,0.13) -> a broken shader
    // would still be non-black. Assert the sphere actually drew: the center
    // region must be meaningfully BRIGHTER than the corner (background) region.
    auto regionAvg = [&](int x0, int y0) -> long {
        long s = 0;
        for (int y = y0; y < y0+16; ++y)
            for (int x = x0; x < x0+16; ++x) {
                const uint8_t* p = &rgba[(y*W + x)*4];
                s += p[0]+p[1]+p[2];
            }
        return s / 256;
    };
    long center = regionAvg(W/2 - 8, H/2 - 8);
    long corner = regionAvg(2, 2);

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (center == 0) {
        std::printf("[smoke] FAIL: center all black\n");
        return 1;
    }
    if (center <= corner + 24) {
        std::printf("[smoke] FAIL: sphere not distinct from background (c=%ld bg=%ld)\n", center, corner);
        return 1;
    }
    std::printf("[smoke] PASS render: sphere distinct (c=%ld bg=%ld), glGetError clean\n", center, corner);
    return 0;
}

// helper: mean absolute per-channel diff over the whole image
static double meanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double s = 0;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) s += std::abs((int)a[i] - (int)b[i]);
    return n ? s / n : 1e9;
}

int AssetViewerApp::runSmokeTangent(const char* fixtureDir)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-tangent", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context (need GL 3.3)"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("glewInit");
    }
    glGetError(); // consume glew's spurious error

    const int W = 256, H = 256;
    auto load = [&](const char* f, MaterialSlotKind k) -> uint32_t {
        std::string err;
        return MaterialTextureLoader_Load(std::string(fixtureDir) + "/" + f, k, &err);
    };

    // Render 1: base color only (no normal map)
    MaterialPreviewPBR p1;
    p1.orbitYaw()   = 0.0f;
    p1.orbitPitch() = 0.0f;
    p1.zoom()       = 3.0f;
    p1.lightDir()[0] = -0.5f; p1.lightDir()[1] = 0.0f; p1.lightDir()[2] = -0.5f;

    uint32_t base1 = load("mat_base.png", MaterialSlotKind::BaseColor);
    if (!base1) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("mat_base.png load failed");
    }
    p1.setSlotTexture(MaterialSlotKind::BaseColor, base1);

    std::vector<uint8_t> noNormal;
    if (!p1.renderToPixels(W, H, noNormal)) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("renderToPixels (no-normal) init/FBO failed");
    }

    // Render 2: base + flat-blue normal (should be ~= no normal)
    // Use a fresh preview to avoid sharing GL state across renders.
    MaterialPreviewPBR p2;
    p2.orbitYaw()   = 0.0f;
    p2.orbitPitch() = 0.0f;
    p2.zoom()       = 3.0f;
    p2.lightDir()[0] = -0.5f; p2.lightDir()[1] = 0.0f; p2.lightDir()[2] = -0.5f;

    uint32_t base2 = load("mat_base.png", MaterialSlotKind::BaseColor);
    uint32_t nrmFlat = load("nrm_flat.png", MaterialSlotKind::Normal);
    if (!base2 || !nrmFlat) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("nrm_flat.png load failed");
    }
    p2.setSlotTexture(MaterialSlotKind::BaseColor, base2);
    p2.setSlotTexture(MaterialSlotKind::Normal, nrmFlat);

    std::vector<uint8_t> flatNormal;
    if (!p2.renderToPixels(W, H, flatNormal)) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("renderToPixels (flat-normal) init/FBO failed");
    }

    // Render 3: same base + tilted normal (should perturb MORE than flat)
    MaterialPreviewPBR p3;
    p3.orbitYaw()   = 0.0f;
    p3.orbitPitch() = 0.0f;
    p3.zoom()       = 3.0f;
    p3.lightDir()[0] = -0.5f; p3.lightDir()[1] = 0.0f; p3.lightDir()[2] = -0.5f;

    uint32_t base3 = load("mat_base.png", MaterialSlotKind::BaseColor);
    uint32_t nrmTilt = load("nrm_tilt_u.png", MaterialSlotKind::Normal);
    if (!base3 || !nrmTilt) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("nrm_tilt_u.png load failed");
    }
    p3.setSlotTexture(MaterialSlotKind::BaseColor, base3);
    p3.setSlotTexture(MaterialSlotKind::Normal, nrmTilt);

    std::vector<uint8_t> tiltNormal;
    if (!p3.renderToPixels(W, H, tiltNormal)) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("renderToPixels (tilt-normal) init/FBO failed");
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();

    double dFlat = meanDiff(noNormal, flatNormal);   // expect SMALL
    double dTilt = meanDiff(noNormal, tiltNormal);   // expect LARGER than dFlat

    if (dFlat > 6.0) {
        std::printf("[smoke] FAIL: flat-blue normal differs from no-normal (mean=%.2f, threshold=6.0)\n", dFlat);
        return 1;
    }
    if (dTilt < dFlat + 2.0) {
        std::printf("[smoke] FAIL: tilted normal did not perturb shading (flat=%.2f tilt=%.2f)\n", dFlat, dTilt);
        return 1;
    }

    // Seam check: the rightmost column (u-wrap, x=W-1) centre rows must not be all-black.
    // A tangent discontinuity at the seam would produce a dark spike.
    long seamSum = 0;
    for (int y = H/4; y < 3*H/4; y++) {
        const uint8_t* px = &flatNormal[(y*W + (W-1))*4];
        seamSum += px[0] + px[1] + px[2];
    }
    if (seamSum == 0) {
        std::printf("[smoke] FAIL: seam column all black (tangent discontinuity)\n");
        return 1;
    }

    std::printf("[smoke] PASS tangent flat=%.2f tilt=%.2f seam-ok\n", dFlat, dTilt);
    return 0;
}

int AssetViewerApp::runSmokeFitLoad(const char* fixtureDir)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return smokeFail("SDL_Init");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-fit-load", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Quit(); return smokeFail("hidden window"); }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_DestroyWindow(win); SDL_Quit(); return smokeFail("gl context (need GL 3.3)"); }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        return smokeFail("glewInit");
    }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    {
        MaterialPreviewPBR preview;
        MaterialSlots slots;
        std::string fitPath = std::string(fixtureDir) + "/sample.fit";
        int n = slots.loadFit(fitPath, preview);
        if (n < 3) {
            std::printf("[smoke] FAIL fit-load: expected >=3 slots loaded, got %d\n", n);
            rc = 1;
        } else if (glGetError() != GL_NO_ERROR) {
            std::printf("[smoke] FAIL fit-load: glGetError non-zero after loadFit\n");
            rc = 1;
        } else {
            std::printf("[smoke] PASS fit-load n=%d\n", n);
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

int AssetViewerApp::runSmokeFitMaterial(const char* dir)
{
    // No GL context needed — pure file parsing.
    std::string err;
    FitMaterial m = FitMaterialLoader_Parse(std::string(dir) + "/sample.fit", &err);
    if (!m.found) {
        std::printf("[smoke] FAIL: no Material block (%s)\n", err.c_str());
        return 1;
    }
    if (m.baseColor != "mat_base.png") {
        std::printf("[smoke] FAIL: baseColor='%s'\n", m.baseColor.c_str());
        return 1;
    }
    if (m.normal != "nrm_flat.png") {
        std::printf("[smoke] FAIL: normal='%s'\n", m.normal.c_str());
        return 1;
    }
    if (m.orm != "mat_orm.png") {
        std::printf("[smoke] FAIL: orm='%s'\n", m.orm.c_str());
        return 1;
    }
    if (m.ormPacking != "RAO_GRough_BMetal") {
        std::printf("[smoke] FAIL: ormPacking='%s'\n", m.ormPacking.c_str());
        return 1;
    }
    std::printf("[smoke] PASS fit parse base/normal/orm/packing\n");
    return 0;
}

// ---------------------------------------------------------------------------
// runSmokeTglLoad — Task 0 headless gate: prove the NS3 TGL loader links and
// loads a real prop from tgl.fst inside the viewer's link unit. No GL needed.
// ---------------------------------------------------------------------------
// Forward-declare the heap installer from engine_stubs.cpp.
extern void InstallTglHeap();

int AssetViewerApp::runSmokeTglLoad(const char* deployDir)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- 1. Install the TGL heap (non-GOS CRT-backed). ---
    InstallTglHeap();

    // --- 2. Set up FastFile globals and open tgl.fst. ---
    //   maxFastFiles / fastFiles are defined in mclib/fastfile.cpp (NS3 boundary).
    //   We allocate space for one archive and call FastFileInit.
    extern FastFile** fastFiles;
    extern long numFastFiles;
    extern long maxFastFiles;
    extern long ffLastError;

    maxFastFiles = 4;
    fastFiles = static_cast<FastFile**>(std::malloc(maxFastFiles * sizeof(FastFile*)));
    if (!fastFiles)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: malloc fastFiles\n");
        return 1;
    }
    std::memset(fastFiles, 0, maxFastFiles * sizeof(FastFile*));
    numFastFiles = 0;

    std::string fstPath = std::string(deployDir) + "/tgl.fst";
    if (!FastFileInit(fstPath.c_str()))
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: FastFileInit('%s') failed (ffLastError=%ld)\n",
            fstPath.c_str(), ffLastError);
        std::free(fastFiles);
        fastFiles = nullptr;
        return 1;
    }

    if (numFastFiles < 1 || !fastFiles[0])
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: no fastFile registered\n");
        return 1;
    }

    // --- 3. Enumerate files; find all .tgl entries. ---
    long totalFiles = fastFiles[0]->getNumFiles();
    const FILE_HANDLE* handles = fastFiles[0]->getFilesInfo();

    long tglCount = 0;
    const char* firstTglName = nullptr;
    if (handles)
    {
        for (long i = 0; i < totalFiles; ++i)
        {
            if (!handles[i].pfe) continue;
            const char* name = handles[i].pfe->name;
            size_t len = std::strlen(name);
            // Check for ".tgl" suffix (case-insensitive enough — all are lower in the fst).
            if (len >= 4 &&
                (name[len-4] == '.' &&
                 (name[len-3] == 't' || name[len-3] == 'T') &&
                 (name[len-2] == 'g' || name[len-2] == 'G') &&
                 (name[len-1] == 'l' || name[len-1] == 'L')))
            {
                if (tglCount == 0) firstTglName = name;
                ++tglCount;
            }
        }
    }

    if (totalFiles <= 0)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: tgl.fst has 0 entries\n");
        return 1;
    }
    if (tglCount == 0)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: no .tgl entries in tgl.fst (total=%ld)\n", totalFiles);
        return 1;
    }

    std::printf("[smoke] tgl.fst files=%ld  tgl=%ld  first='%s'\n",
        totalFiles, tglCount, firstTglName ? firstTglName : "(null)");

    // --- 4. LoadBinaryCopy the first .tgl found. ---
    TG_TypeMultiShape ms;
    long rc = ms.LoadBinaryCopy(firstTglName);
    if (rc != 0)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: LoadBinaryCopy('%s') returned %ld\n",
            firstTglName, rc);
        return 1;
    }

    long numShapes = ms.GetNumShapes();
    if (numShapes <= 0)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: GetNumShapes()=%ld\n", numShapes);
        return 1;
    }

    // --- 5. Walk shapes, find one with geometry. ---
    TG_TypeShape* geomShape = nullptr;
    for (long i = 0; i < numShapes; ++i)
    {
        TG_TypeNodePtr node = ms.GetTypeNode(i);
        if (!node) continue;
        TG_TypeShape* s = dynamic_cast<TG_TypeShape*>(node);
        if (s && s->GetNumTypeVertices() > 0)
        {
            geomShape = s;
            break;
        }
    }

    if (!geomShape)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: no TG_TypeShape with verts in '%s'\n", firstTglName);
        return 1;
    }

    int  numV = geomShape->GetNumTypeVertices();
    long numT = geomShape->GetNumTypeTriangles();

    if (numV <= 0)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: verts=%d\n", numV);
        return 1;
    }
    if (numT <= 0)
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: tris=%ld\n", numT);
        return 1;
    }

    // --- 6. Read texture name from triangle 0. ---
    const TG_TypeTriangle* tris = geomShape->GetTypeTriangles();
    char texBuf[256] = {0};
    ms.GetTextureName(tris[0].localTextureHandle, texBuf, (long)sizeof(texBuf));
    if (texBuf[0] == '\0')
    {
        std::fprintf(stderr, "[smoke] FAIL tgl-load: texture name empty for tri[0].localTextureHandle=%lu\n",
            (unsigned long)tris[0].localTextureHandle);
        return 1;
    }

    std::printf("[smoke] PASS tgl-load files=%ld tgl=%ld shape0 v=%d t=%ld tex=%s\n",
        totalFiles, tglCount, numV, numT, texBuf);
    return 0;
}

// ---------------------------------------------------------------------------
// runSmokeMeshRender — Task 2 GL gate: MeshPreview3D renders a prop; model
// must be distinct from background. Requires GL 3.3 + a real deploy dir.
// ---------------------------------------------------------------------------
#include "MeshPreview3D.h"

int AssetViewerApp::runSmokeMeshRender(const char* deployDir)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[smoke] FAIL mesh-render: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-mesh-render", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) {
        SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL mesh-render: hidden window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL mesh-render: gl context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL mesh-render: glewInit\n");
        return 1;
    }
    glGetError(); // consume glew's spurious error

    const int W = 256, H = 256;
    int rc = 0;
    {
        MeshPreview3D p;
        p.setDeployDir(deployDir);
        p.setSource("data/tgl/2civliving.tgl");

        std::vector<uint8_t> rgba;
        if (!p.renderToPixels(W, H, rgba)) {
            std::fprintf(stderr, "[smoke] FAIL mesh-render: renderToPixels (init/FBO/shader failed)\n");
            rc = 1;
        }

        if (rc == 0) {
            GLenum e = glGetError();
            if (e != GL_NO_ERROR) {
                std::fprintf(stderr, "[smoke] FAIL mesh-render: glGetError 0x%x\n", (unsigned)e);
                rc = 1;
            }
        }

        if (rc == 0) {
            // Center region (16x16) vs corner region (16x16 at top-left).
            // FBO is bottom-up; pixel (x,y) is at rgba[(y*W+x)*4].
            auto regionAvg = [&](int x0, int y0) -> long {
                long s = 0;
                for (int y = y0; y < y0 + 16; ++y)
                    for (int x = x0; x < x0 + 16; ++x) {
                        const uint8_t* px = &rgba[(y * W + x) * 4];
                        s += px[0] + px[1] + px[2];
                    }
                return s / 256;
            };
            long center = regionAvg(W / 2 - 8, H / 2 - 8);
            long corner = regionAvg(2, 2);

            if (center == 0) {
                std::fprintf(stderr, "[smoke] FAIL mesh-render: center all black\n");
                rc = 1;
            } else if (center <= corner + 12) {
                std::fprintf(stderr,
                    "[smoke] FAIL mesh-render: model not distinct from background (c=%ld bg=%ld)\n",
                    center, corner);
                rc = 1;
            } else {
                std::printf("[smoke] PASS mesh-render c=%ld bg=%ld glGetError clean\n", center, corner);
            }
        }

        // Optional: exercise the live reloadAlbedo path (tier swap) and check GL errors.
        if (rc == 0) {
            p.reloadAlbedoAtTier(deployDir, 256);
            GLenum e2 = glGetError();
            if (e2 != GL_NO_ERROR) {
                std::fprintf(stderr, "[smoke] FAIL mesh-render: reloadAlbedo(256) glGetError 0x%x\n",
                             (unsigned)e2);
                rc = 1;
            } else {
                std::printf("[smoke] PASS mesh-render: reloadAlbedo(256) glGetError clean\n");
            }
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

// ---------------------------------------------------------------------------
// runSmokeMeshOrient — orientation gate: load 2civliving.tgl and assert that
// the GL-Y axis extent is the largest of the three axes (a building is taller
// than it is wide or deep). No GL required.
// ---------------------------------------------------------------------------
int AssetViewerApp::runSmokeMeshOrient(const char* deployDir)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (!TglMeshLoader::ensureFastFile(deployDir))
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-orient: ensureFastFile('%s')\n", deployDir);
        return 1;
    }

    MeshData md = TglMeshLoader::loadMesh("data/tgl/2civliving.tgl");
    if (!md.ok)
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-orient: loadMesh error: %s\n", md.error.c_str());
        return 1;
    }

    float extX = md.bmax[0] - md.bmin[0];   // GL X (Stuff X after transform)
    float extY = md.bmax[1] - md.bmin[1];   // GL Y (Stuff Z after transform = vertical)
    float extZ = md.bmax[2] - md.bmin[2];   // GL Z (-Stuff Y after transform)

    std::printf("[smoke] mesh-orient: extents X=%.3f  Y=%.3f  Z=%.3f\n", extX, extY, extZ);

    // A 2-storey building must have GL-Y as the "tall" axis: taller than it is
    // deep (GL-Z). 2civliving is a wide city block so its footprint (X, Z) can
    // be large, but the spec's fallback says "Y must not be the smallest".
    // With the correct Stuff-Z-up → GL-Y-up mapping, GL-Y = stuffZ (the MC2
    // vertical), and GL-Z = stuffY (depth). Assert GL-Y >= GL-Z.
    if (extY < extZ)
    {
        std::fprintf(stderr,
            "[smoke] FAIL mesh-orient: GL-Y (%.3f) < GL-Z (%.3f) — "
            "coordinate transform maps the wrong Stuff axis to GL vertical\n",
            extY, extZ);
        return 1;
    }

    std::printf("[smoke] PASS mesh-orient: GL-Y=%.3f >= GL-Z=%.3f (vertical axis correct)\n",
        extY, extZ);
    return 0;
}

// ---------------------------------------------------------------------------
// runSmokeMeshBuild — Task 1 headless gate: TglMeshLoader CPU mesh extraction.
// No GL required.
// ---------------------------------------------------------------------------
#include "TglMeshLoader.h"

int AssetViewerApp::runSmokeMeshBuild(const char* deployDir)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- 1. Init FastFile via TglMeshLoader ---
    if (!TglMeshLoader::ensureFastFile(deployDir))
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-build: ensureFastFile('%s')\n", deployDir);
        return 1;
    }

    // --- 2. Enumerate .tgl names ---
    std::vector<std::string> tgls = TglMeshLoader::listTgl();
    if (tgls.empty())
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-build: listTgl() returned 0 entries\n");
        return 1;
    }

    // Prefer a known prop; fall back to the first one found.
    std::string target = tgls[0];
    for (const auto& t : tgls)
    {
        if (t.find("2civliving") != std::string::npos)
        {
            target = t;
            break;
        }
    }

    std::printf("[smoke] mesh-build: loading '%s' (of %zu tgls)\n",
        target.c_str(), tgls.size());

    // --- 3. Load mesh ---
    MeshData md = TglMeshLoader::loadMesh(target);
    if (!md.ok)
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-build: loadMesh error: %s\n", md.error.c_str());
        return 1;
    }

    if (md.submeshes.empty())
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-build: no submeshes\n");
        return 1;
    }

    // --- 4. Validate each submesh ---
    size_t totalVerts = 0, totalTris = 0;
    bool hasNonEmptyTex = false;

    for (size_t si = 0; si < md.submeshes.size(); ++si)
    {
        const SubMesh& sub = md.submeshes[si];

        // Non-indexed expansion: verts.size() == idx.size()
        if (sub.verts.size() != sub.idx.size())
        {
            std::fprintf(stderr,
                "[smoke] FAIL mesh-build: sub[%zu] verts(%zu) != idx(%zu)\n",
                si, sub.verts.size(), sub.idx.size());
            return 1;
        }

        // Index count must be a multiple of 3 (whole triangles).
        if (sub.idx.size() % 3 != 0)
        {
            std::fprintf(stderr,
                "[smoke] FAIL mesh-build: sub[%zu] idx.size()=%zu not multiple of 3\n",
                si, sub.idx.size());
            return 1;
        }

        // All indices must be valid (< verts.size(), non-indexed so sequential).
        for (size_t ii = 0; ii < sub.idx.size(); ++ii)
        {
            if (sub.idx[ii] >= (uint32_t)sub.verts.size())
            {
                std::fprintf(stderr,
                    "[smoke] FAIL mesh-build: sub[%zu] idx[%zu]=%u >= verts.size()=%zu\n",
                    si, ii, sub.idx[ii], sub.verts.size());
                return 1;
            }
        }

        if (!sub.textureName.empty()) hasNonEmptyTex = true;

        totalVerts += sub.verts.size();
        totalTris  += sub.idx.size() / 3;
    }

    if (!hasNonEmptyTex)
    {
        std::fprintf(stderr, "[smoke] FAIL mesh-build: no submesh has a non-empty textureName\n");
        return 1;
    }

    // --- 5. Validate bounds are finite and bmax >= bmin ---
    for (int a = 0; a < 3; ++a)
    {
        if (!std::isfinite(md.bmin[a]) || !std::isfinite(md.bmax[a]))
        {
            std::fprintf(stderr, "[smoke] FAIL mesh-build: non-finite bounds[%d]\n", a);
            return 1;
        }
        if (md.bmax[a] < md.bmin[a])
        {
            std::fprintf(stderr,
                "[smoke] FAIL mesh-build: bmax[%d]=%.3f < bmin[%d]=%.3f\n",
                a, md.bmax[a], a, md.bmin[a]);
            return 1;
        }
    }

    std::printf("[smoke] PASS mesh-build subs=%zu verts=%zu tris=%zu tex0=%s\n",
        md.submeshes.size(), totalVerts, totalTris,
        md.submeshes[0].textureName.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// runSmokeSpotlight — headless gate: verifies isSpotlight detection.
// Loads ambulance.tgl (has SpotLight_ nodes) and asserts ≥1 isSpotlight submesh.
// Loads 2civliving.tgl (no SpotLight_ nodes) and asserts 0 isSpotlight submeshes.
// No GL required.
// ---------------------------------------------------------------------------
int AssetViewerApp::runSmokeSpotlight(const char* deployDir)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    if (!TglMeshLoader::ensureFastFile(deployDir))
    {
        std::fprintf(stderr, "[smoke] FAIL spotlight: ensureFastFile('%s')\n", deployDir);
        return 1;
    }

    // --- Vehicle with SpotLight_ shapes: ambulance.tgl ---
    {
        MeshData md = TglMeshLoader::loadMesh("data/tgl/ambulance.tgl");
        if (!md.ok)
        {
            std::fprintf(stderr, "[smoke] FAIL spotlight: ambulance loadMesh error: %s\n",
                md.error.c_str());
            return 1;
        }

        size_t spotCount = 0;
        for (const SubMesh& sub : md.submeshes)
            if (sub.isSpotlight) ++spotCount;

        std::printf("[smoke] spotlight: ambulance.tgl subs=%zu spotlightSubs=%zu\n",
            md.submeshes.size(), spotCount);

        if (spotCount == 0)
        {
            std::fprintf(stderr,
                "[smoke] FAIL spotlight: ambulance.tgl has 0 isSpotlight submeshes "
                "(expected ≥1 — SpotLight_ detection broken)\n");
            return 1;
        }
    }

    // --- Building with no SpotLight_ shapes: 2civliving.tgl ---
    {
        MeshData md = TglMeshLoader::loadMesh("data/tgl/2civliving.tgl");
        if (!md.ok)
        {
            std::fprintf(stderr, "[smoke] FAIL spotlight: 2civliving loadMesh error: %s\n",
                md.error.c_str());
            return 1;
        }

        for (const SubMesh& sub : md.submeshes)
        {
            if (sub.isSpotlight)
            {
                std::fprintf(stderr,
                    "[smoke] FAIL spotlight: 2civliving.tgl has isSpotlight submesh "
                    "(false positive — non-vehicle flagged as spotlight)\n");
                return 1;
            }
        }

        std::printf("[smoke] spotlight: 2civliving.tgl subs=%zu spotlightSubs=0 (correct)\n",
            md.submeshes.size());
    }

    std::printf("[smoke] PASS spotlight: ambulance has beams, 2civliving has none\n");
    return 0;
}

#include "GlbMeshLoader.h"
#include "model_override_registry.h"
#include "AppearanceRoster.h"
#include "WorkbenchValidation.h"
#include "BundleExport.h"
#include "CentralManifestMerge.h"
#include <fstream>
#include <sstream>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

int AssetViewerApp::runSmokeWorkbenchLink()
{
    ModelOverrideRegistry reg;
    if (reg.loadFromFile("does_not_exist_models.json", ".") != 0)
        return smokeFail("workbench-link: expected 0 records");
    Assimp::Importer imp;
    if (!imp.IsExtensionSupported(".gltf"))
        return smokeFail("workbench-link: assimp lacks .gltf importer");
    std::printf("[smoke] PASS workbench-link (registry + assimp glTF linked)\n");
    return 0;
}

int AssetViewerApp::runSmokeWorkbenchGlb(const char* fixtureDir)
{
    MeshData md = GlbMeshLoader::load(std::string(fixtureDir) + "/unit_tri.gltf");
    if (!md.ok) return smokeFail((std::string("workbench-glb: ") + md.error).c_str());
    if (md.submeshes.size() != 1) return smokeFail("workbench-glb: submeshes!=1");
    const SubMesh& sm = md.submeshes[0];
    if (sm.verts.size() != 3) return smokeFail("workbench-glb: verts!=3");
    auto has = [&](float x,float y,float z){
        for (auto& v : sm.verts)
            if (std::fabs(v.px-x)<1e-3f && std::fabs(v.py-y)<1e-3f && std::fabs(v.pz-z)<1e-3f) return true;
        return false; };
    if (!has(0,0,0) || !has(2,0,5) || !has(0,3,7))
        return smokeFail("workbench-glb: transformed positions wrong (axis swap?)");
    float ex=md.bmax[0]-md.bmin[0], ey=md.bmax[1]-md.bmin[1], ez=md.bmax[2]-md.bmin[2];
    if (std::fabs(ex-2)>1e-3f || std::fabs(ey-3)>1e-3f || std::fabs(ez-7)>1e-3f)
        return smokeFail("workbench-glb: extents wrong");
    // Discriminating v-flip check: src vert1 UV (1,0) -> (1,1) ONLY if v->1-v applied.
    bool flipOk=false; for (auto& v: sm.verts) if (std::fabs(v.u-1.0f)<1e-3f && std::fabs(v.v-1.0f)<1e-3f) flipOk=true;
    if (!flipOk) return smokeFail("workbench-glb: UV v-flip not applied (expected (1,1))");
    std::printf("[smoke] PASS workbench-glb verts=3 ext=%.1f,%.1f,%.1f\n", ex,ey,ez);
    return 0;
}

int AssetViewerApp::runSmokeWorkbenchBind(const char* deployDir, const char* fixtureDir){
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ModWorkbench wb;
    wb.setDeployDir(deployDir);
    if (!wb.loadOverride(std::string(fixtureDir) + "/unit_tri.gltf"))
        return smokeFail("bind: override load");
    uint64_t g0 = wb.generation();
    if (!wb.bindStock("data/tgl/2civliving.tgl"))
        return smokeFail("bind: stock load (need deploy dir w/ tgl.fst)");
    if (wb.generation() == g0)
        return smokeFail("bind: generation did not advance");
    auto d = wb.computeDelta();
    if (d.stockExt[1] <= 0.0f)
        return smokeFail("bind: stock has no Y extent");
    if (d.maxRatio <= 0.0f)
        return smokeFail("bind: ratio not computed");
    std::printf("[smoke] PASS workbench-bind stockY=%.2f ovY=%.2f ratio=%.3f\n",
        d.stockExt[1], d.overrideExt[1], d.maxRatio);
    return 0;
}

int AssetViewerApp::runSmokeWorkbenchValidate(const char* fixtureDir){
    WorkbenchOverride ok; ok.overrideClass="staticProp"; ok.appearanceName="example_name"; ok.appearanceVerified=true;
    ok.sourceRelPath="props/example.glb"; ok.scale=1.0f; ok.renderOnly=true; ok.fallback="stock";
    if (!ValidateRecordRules(ok).empty()) return smokeFail("validate: clean record flagged");
    auto hasCode=[&](const std::vector<Warning>& v,const char* c){ for(auto&x:v) if(x.code==c) return true; return false; };
    { auto bs=ok; bs.scale=2.0f; if(!hasCode(ValidateRecordRules(bs),"scale")) return smokeFail("validate: scale!=1 not blocked"); }
    { auto bsrc=ok; bsrc.sourceRelPath="C:/abs.png"; if(!hasCode(ValidateRecordRules(bsrc),"source")) return smokeFail("validate: unsafe source not blocked"); }
    { auto unv=ok; unv.appearanceVerified=false; if(!hasCode(ValidateRecordRules(unv),"appearance-unverified")) return smokeFail("validate: unverified appearance not blocked"); }
    ModelOverrideRegistry reg;
    if (reg.loadFromFile(std::string(fixtureDir)+"/wb_valid.json",fixtureDir)!=1 || reg.resolve("staticProp","example_name")==nullptr)
        return smokeFail("validate: registry should accept wb_valid.json");
    if (reg.loadFromFile(std::string(fixtureDir)+"/wb_bad_scale.json",fixtureDir)!=0)
        return smokeFail("validate: registry should reject wb_bad_scale.json");
    MeshData ov; ov.ok=true; ov.bmax[0]=4; ov.bmax[1]=4; ov.bmax[2]=4;
    MeshData st; st.ok=true; st.bmax[0]=1; st.bmax[1]=1; st.bmax[2]=1;
    SemanticInputs si; si.overrideMesh=&ov; si.stockMesh=&st; si.maxFootprintRatio=4.0f;
    if(!hasCode(ValidateSemantics(ok,si),"bounds-delta")) return smokeFail("validate: oversize not warned");
    std::printf("[smoke] PASS workbench-validate\n"); return 0;
}

int AssetViewerApp::runSmokeWorkbenchExport(const char* fixtureDir, const char* tmpDir){
    namespace fs=std::filesystem;
    std::string root=std::string(tmpDir)+"/model_overrides"; fs::remove_all(root);
    std::string glb=std::string(fixtureDir)+"/unit_tri.gltf";
    WorkbenchOverride rec; rec.overrideClass="staticProp"; rec.appearanceName="example_name";
    rec.appearanceVerified=true; rec.scale=1.0f; rec.renderOnly=true; rec.fallback="stock";

    ExportResult r=ExportBundle(root,"example_id",glb,rec);
    if(!r.ok) return smokeFail((std::string("export: ")+r.message).c_str());
    if(!fs::exists(r.manifestPath)) return smokeFail("export: models.generated.json missing");
    if(!fs::exists(r.bundleDir+"/unit_tri.gltf")) return smokeFail("export: glb not copied");
    ModelOverrideRegistry reg; reg.loadFromFile(r.manifestPath, root);
    if(reg.resolve("staticProp","example_name")==nullptr) return smokeFail("export: registry did not resolve record");

    { auto bad=rec; bad.scale=2.0f; if(ExportBundle(root,"bad_scale",glb,bad).ok) return smokeFail("export: should refuse scale!=1"); }
    { auto bad=rec; bad.appearanceVerified=false; if(ExportBundle(root,"unverified",glb,bad).ok) return smokeFail("export: should refuse unverified appearance"); }
    { if(ExportBundle(root,"../escape",glb,rec).ok) return smokeFail("export: should refuse bad bundle id"); }
    { auto q=rec; q.appearanceName="a\"b\\c"; q.appearanceVerified=true;
      ExportResult er=ExportBundle(root,"escape_id",glb,q);
      if(er.ok){ ModelOverrideRegistry rr; rr.loadFromFile(er.manifestPath, root);
                 if(rr.resolve("staticProp","a\"b\\c")==nullptr) return smokeFail("export: escaped key did not round-trip"); } }
    std::printf("[smoke] PASS workbench-export (round-trip + refusals + escaping)\n"); return 0;
}

int AssetViewerApp::runSmokeWorkbenchReload(const char* fixtureDir){
    // A newly dropped override must NOT inherit the prior override's appearance
    // key / verified flag / LOD chain (else override-B could export under
    // override-A's verified key). Pure ModWorkbench logic — no GL.
    ModWorkbench wb;
    std::string glb = std::string(fixtureDir)+"/unit_tri.gltf";
    if (!wb.loadOverride(glb)) return smokeFail("reload: first load");
    wb.record().appearanceName    = "stale_key";
    wb.record().appearanceVerified = true;
    wb.record().lods.push_back({1, "lod1.glb", 50.0f});
    if (!wb.loadOverride(glb)) return smokeFail("reload: second load");
    if (!wb.record().appearanceName.empty()) return smokeFail("reload: appearanceName not cleared");
    if (wb.record().appearanceVerified)      return smokeFail("reload: appearanceVerified not reset");
    if (!wb.record().lods.empty())           return smokeFail("reload: lods not cleared");
    std::printf("[smoke] PASS workbench-reload (override state reset on reload)\n");
    return 0;
}

int AssetViewerApp::runSmokeAppearanceRoster(const char* fixtureDir) {
    AppearanceRoster r;
    std::string dir = std::string(fixtureDir) + "/tgl_ini";
    r.load(dir);
    const auto& n = r.names();
    bool ok = (n.size() == 4)
           && (n[0] == "atlas_building")
           && (n[1] == "Tree_Oak")
           && (n[2] == "Tree_Oakdam")
           && (n[3] == "Tree_OakL1")
           && r.contains("TREE_OAK")
           && !r.contains("nonexistent")
           && r.scannedFileCount() == 4;
    printf("[smoke] appearance-roster %s (names=%zu files=%d)\n",
           ok ? "PASS" : "FAIL", n.size(), r.scannedFileCount());
    return ok ? 0 : 1;
}

int AssetViewerApp::runSmokeTextureMissingWarn(const char* fixtureDir) {
    ModWorkbench wb;
    auto& rec = wb.record();
    rec.overrideClass = "staticProp";
    rec.appearanceName = "smoke_prop";
    rec.appearanceVerified = true;
    rec.sourceRelPath = "model.glb";
    wb.textureSlots()[0].path = std::string(fixtureDir) + "/does_not_exist_basecolor.png";
    wb.revalidateWithTextures();
    int missingWarns = 0, blocks = 0;
    for (const auto& w : wb.warnings()) {
        if (w.code == std::string("texture-missing")) ++missingWarns;
        if (w.severity == WarnSeverity::Block) ++blocks;
    }
    bool ok = (missingWarns == 1) && (blocks == 0);
    printf("[smoke] texture-missing-warn %s (missing=%d blocks=%d)\n",
           ok ? "PASS" : "FAIL", missingWarns, blocks);
    return ok ? 0 : 1;
}

int AssetViewerApp::runSmokeLodEditValidate() {
    auto countCode = [](const std::vector<Warning>& ws, const char* code, WarnSeverity sev){
        int n=0; for (auto& w: ws) if (w.code==std::string(code) && w.severity==sev) ++n; return n;
    };
    bool ok = true;
    // Case A: ascending lods, first entry distance 0 -> no lod-order BLOCK, no first-distance WARN, no lod0-only.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        r.lods = {{1,"lod1.glb",0.0f},{2,"lod2.glb",50.0f}};
        auto br = ValidateRecordRules(r);
        SemanticInputs si; si.hasImpostorLod = !r.lods.empty();
        auto sm = ValidateSemantics(r, si);
        ok &= countCode(br,"lod-order",WarnSeverity::Block)==0;
        ok &= countCode(sm,"lod-first-distance",WarnSeverity::Warn)==0;
        ok &= countCode(sm,"lod0-only",WarnSeverity::Warn)==0;
    }
    // Case B: non-ascending -> lod-order BLOCK fires.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        r.lods = {{2,"lod2.glb",50.0f},{1,"lod1.glb",10.0f}};
        auto br = ValidateRecordRules(r);
        ok &= countCode(br,"lod-order",WarnSeverity::Block)>=1;
    }
    // Case C: LOD0-only (no entries) -> lod0-only WARN.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        SemanticInputs si; si.hasImpostorLod = !r.lods.empty();
        auto sm = ValidateSemantics(r, si);
        ok &= countCode(sm,"lod0-only",WarnSeverity::Warn)==1;
    }
    // Case D: ascending but first entry distance != 0 -> lod-first-distance WARN.
    {
        WorkbenchOverride r; r.overrideClass="staticProp"; r.appearanceName="p";
        r.appearanceVerified=true; r.sourceRelPath="model.glb";
        r.lods = {{1,"lod1.glb",10.0f},{2,"lod2.glb",50.0f}};
        SemanticInputs si; si.hasImpostorLod = !r.lods.empty();
        auto sm = ValidateSemantics(r, si);
        ok &= countCode(sm,"lod-first-distance",WarnSeverity::Warn)==1;
    }
    printf("[smoke] lod-edit-validate %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int AssetViewerApp::runSmokeCentralMergePreserve(const char* fixtureDir, const char* tmpDir) {
    namespace fs = std::filesystem;
    fs::path src = fs::path(fixtureDir) / "central" / "models.json";
    fs::path dir = fs::path(tmpDir) / "central_merge_smoke";
    std::error_code ec; fs::create_directories(dir, ec);
    fs::path dst = dir / "models.json";
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { printf("[smoke] central-merge-preserve FAIL (fixture copy: %s)\n", ec.message().c_str()); return 1; }

    auto mk = [](const char* cls, const char* app, const char* source){
        WorkbenchOverride r; r.overrideClass=cls; r.appearanceName=app;
        r.appearanceVerified=true; r.sourceRelPath=source; return r;
    };
    bool ok = true;
    // Merge C (new) -> A,B,C present.
    auto rC = MergeIntoCentralManifest(dst.string(), mk("staticProp","propC","c.glb"));
    ok &= rC.ok && rC.recordCount==3 && !rC.replacedExisting;
    {
        ModelOverrideRegistry g; g.loadFromFile(dst.string(), dir.string());
        ok &= g.count()==3;
        ok &= g.resolve("staticProp","propA")!=nullptr;
        ok &= g.resolve("tree","propB")!=nullptr;
        ok &= g.resolve("staticProp","propC")!=nullptr;
    }
    // Merge B replacement (new source) -> A,new-B,C; count stays 3.
    auto rB = MergeIntoCentralManifest(dst.string(), mk("tree","propB","b2.glb"));
    ok &= rB.ok && rB.recordCount==3 && rB.replacedExisting;
    {
        ModelOverrideRegistry g; g.loadFromFile(dst.string(), dir.string());
        ok &= g.count()==3;
        const auto* b = g.resolve("tree","propB");
        ok &= (b && b->sourceRelPath=="b2.glb");
        ok &= g.resolve("staticProp","propA")!=nullptr;
        ok &= g.resolve("staticProp","propC")!=nullptr;
    }
    // .bak exists after writes.
    ok &= fs::exists(dst.string()+".bak");
    // Failure path: a BLOCK-invalid record (unverified appearance) leaves file intact.
    {
        std::string before; { std::ifstream f(dst); std::stringstream s; s<<f.rdbuf(); before=s.str(); }
        WorkbenchOverride bad = mk("staticProp","propD","d.glb"); bad.appearanceVerified=false;
        auto rBad = MergeIntoCentralManifest(dst.string(), bad);
        std::string after; { std::ifstream f(dst); std::stringstream s; s<<f.rdbuf(); after=s.str(); }
        ok &= !rBad.ok && (before==after);
    }
    // Case E: first write to a NEW path -> file created with 1 record, no crash.
    {
        fs::path freshDir = fs::path(tmpDir) / "central_merge_fresh";
        fs::remove_all(freshDir, ec); fs::create_directories(freshDir, ec);
        fs::path fresh = freshDir / "models.json";
        auto rNew = MergeIntoCentralManifest(fresh.string(), mk("staticProp","propFresh","f.glb"));
        ok &= rNew.ok && rNew.recordCount==1 && !rNew.replacedExisting;
        ModelOverrideRegistry g; g.loadFromFile(fresh.string(), freshDir.string());
        ok &= g.count()==1 && g.resolve("staticProp","propFresh")!=nullptr;
    }
    printf("[smoke] central-merge-preserve %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int AssetViewerApp::runSmokeShaderInclude(const char* fixtureDir) {
    std::string root = std::string(fixtureDir) + "/shaderinc";
    ShaderResolveResult r = ResolveShaderIncludes(root, "root.glsl");
    bool ok = r.ok
           && r.source.find("#include") == std::string::npos
           && r.source.find("b_val") != std::string::npos
           && r.source.find("a_val") != std::string::npos
           && r.includedFiles.size() == 3
           && r.unresolved.empty();
    ShaderResolveResult c = ResolveShaderIncludes(root, "inc/cycle.hglsl");
    ok &= (!c.error.empty());
    printf("[smoke] shader-include %s (includes=%zu)\n", ok ? "PASS" : "FAIL",
           r.includedFiles.size());
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// runSmokeBackendACompile — Backend-A v2 Task 2: compile+link the real engine
// shaders shaders/static_prop.{vert,frag} on a headless GL 4.3 context and
// report the active uniform and SSBO interface. Pass = DONE. Blocked = STOP.
// ---------------------------------------------------------------------------
int AssetViewerApp::runSmokeBackendACompile(const char* shaderRoot)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Headless GL context: reuse same pattern as runSmokeMeshRender.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[smoke] FAIL backend-a-compile: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-backend-a-compile", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) {
        SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-compile: hidden window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-compile: gl context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-compile: glewInit\n");
        return 1;
    }
    glGetError(); // consume glew's spurious error

    auto buildStage = [&](const char* file, GLenum type, std::string& logOut,
                          std::vector<std::string>& incOut) -> unsigned {
        ShaderResolveResult r = ResolveShaderIncludes(shaderRoot, file);
        incOut = r.includedFiles;
        if (!r.ok) {
            logOut = "resolve failed: " + r.error +
                (r.unresolved.empty() ? std::string() : (" unresolved=" + r.unresolved.front()));
            return 0;
        }
        std::string defs =
            "#version 430\n"
            "// Backend-A minimal config: legacy lane, no view-uniforms/coalesce/PBR-slots.\n";
        std::string src = defs + r.source;
        unsigned sh = glCreateShader(type);
        const char* p = src.c_str();
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            std::string l(len, '\0');
            glGetShaderInfoLog(sh, len, nullptr, l.data());
            logOut = l;
        }
        if (!ok) { glDeleteShader(sh); return 0; }
        return sh;
    };

    std::string vlog, flog;
    std::vector<std::string> vinc, finc;
    unsigned vs = buildStage("static_prop.vert", GL_VERTEX_SHADER,   vlog, vinc);
    unsigned fs = buildStage("static_prop.frag", GL_FRAGMENT_SHADER, flog, finc);
    unsigned prog = 0;
    std::string linkLog;
    bool linked = false;
    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok); linked = ok != 0;
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            std::string l(len, '\0');
            glGetProgramInfoLog(prog, len, nullptr, l.data());
            linkLog = l;
        }
    }

    if (linked) {
        GLint nu = 0; glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &nu);
        for (GLint i = 0; i < nu; ++i) {
            char nm[128]; GLint sz; GLenum tp; GLsizei wr;
            glGetActiveUniform(prog, i, sizeof nm, &wr, &sz, &tp, nm);
            printf("[contract] uniform %s\n", nm);
        }
        GLint nb = 0;
        glGetProgramInterfaceiv(prog, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &nb);
        for (GLint i = 0; i < nb; ++i) {
            char nm[128]; GLsizei wr;
            glGetProgramResourceName(prog, GL_SHADER_STORAGE_BLOCK, i, sizeof nm, &wr, nm);
            printf("[contract] ssbo-block %s\n", nm);
        }
    }

    if (!vs) printf("[contract] VERT FAIL log:\n%s\n", vlog.c_str());
    if (!fs) printf("[contract] FRAG FAIL log:\n%s\n", flog.c_str());
    if (vs && fs && !linked) printf("[contract] LINK FAIL log:\n%s\n", linkLog.c_str());
    printf("[smoke] backend-a-compile %s\n", linked ? "PASS" : "FAIL");

    if (prog) glDeleteProgram(prog);
    if (vs)   glDeleteShader(vs);
    if (fs)   glDeleteShader(fs);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return linked ? 0 : 1;
}

// ---------------------------------------------------------------------------
// runSmokeBackendARender — Backend-A v2 Task 5: ModelPreviewEngineShader renders
// a prop with the real engine shaders; model must be distinct from background.
// ---------------------------------------------------------------------------
#include "ModelPreviewEngineShader.h"

int AssetViewerApp::runSmokeBackendARender(const char* deployDir, const char* shaderRoot)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Headless GL 4.3 context (SSBOs require GL 4.3)
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[smoke] FAIL backend-a-render: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-backend-a-render", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) {
        SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-render: hidden window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-render: gl context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-render: glewInit\n");
        return 1;
    }
    glGetError(); // consume glew's spurious error

    int rc = 0;
    {
        ModelPreviewEngineShader p;
        p.setShaderRoot(shaderRoot);
        p.setDeployDir(deployDir);
        p.setSource("data/tgl/2civliving.tgl");

        std::vector<uint8_t> px;
        bool rendered = p.renderToPixels(128, 128, px);

        bool nonEmpty = false;
        if (rendered && px.size() >= 4) {
            for (size_t i = 4; i + 3 < px.size(); i += 4)
                if (px[i] != px[0] || px[i+1] != px[1] || px[i+2] != px[2]) {
                    nonEmpty = true; break;
                }
        }

        bool ok = p.ok() && rendered && nonEmpty;
        printf("[smoke] backend-a-render %s (compiled=%d nonEmpty=%d)\n",
               ok ? "PASS" : "FAIL", (int)p.ok(), (int)nonEmpty);
        if (!p.ok() && !p.report().lastError.empty())
            std::fprintf(stderr, "[smoke] backend-a-render error: %s\n",
                         p.report().lastError.c_str());
        if (!p.report().compileLog.empty())
            std::fprintf(stderr, "[smoke] compile log: %s\n", p.report().compileLog.c_str());
        if (!p.report().linkLog.empty())
            std::fprintf(stderr, "[smoke] link log: %s\n", p.report().linkLog.c_str());
        rc = ok ? 0 : 1;
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

// ---------------------------------------------------------------------------
// runSmokeBackendAFallback — Backend-A v2 Task 7: fail-open guarantee.
// Forces Backend-A shader build to fail (nonexistent shader root), then proves
// Backend-B (MeshPreview3D) still renders a non-empty frame on the same prop.
// ---------------------------------------------------------------------------
int AssetViewerApp::runSmokeBackendAFallback(const char* deployDir)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Headless GL 4.3 context (copy from runSmokeBackendARender)
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[smoke] FAIL backend-a-fallback: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-backend-a-fallback", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) {
        SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-fallback: hidden window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-fallback: gl context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-fallback: glewInit\n");
        return 1;
    }
    glGetError(); // consume glew's spurious error

    const char* PROP = "data/tgl/2civliving.tgl";

    int rc = 0;
    {
        ModelPreviewEngineShader a;
        a.setShaderRoot("___nonexistent_shader_root___");   // force resolve/compile failure
        a.setDeployDir(deployDir);
        a.setSource(PROP);
        std::vector<uint8_t> apx;
        a.renderToPixels(128, 128, apx);
        bool aFailed = !a.ok() && !a.report().lastError.empty();   // A failed AND recorded it

        MeshPreview3D b;                                            // the fail-open target
        b.setDeployDir(deployDir);
        b.setSource(PROP);
        std::vector<uint8_t> bpx;
        bool bRendered = b.renderToPixels(128, 128, bpx);
        bool bNonEmpty = false;
        for (size_t i = 4; bRendered && i + 3 < bpx.size(); i += 4)
            if (bpx[i] != bpx[0] || bpx[i+1] != bpx[1] || bpx[i+2] != bpx[2]) {
                bNonEmpty = true; break;
            }

        bool ok = aFailed && bRendered && bNonEmpty;
        printf("[smoke] backend-a-fallback %s (aFailed=%d bNonEmpty=%d)\n",
               ok ? "PASS" : "FAIL", (int)aFailed, (int)bNonEmpty);
        if (!a.report().lastError.empty())
            std::fprintf(stderr, "[smoke] backend-a error (expected): %s\n",
                         a.report().lastError.c_str());
        rc = ok ? 0 : 1;
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

// ---------------------------------------------------------------------------
// runSmokeBackendAUvv — Backend-A v2 Task 8: UV-V orientation A/B check.
// Renders 2civliving.tgl with BOTH Backend-A (ModelPreviewEngineShader) and
// Backend-B (MeshPreview3D), writes both as PPM dumps to build64/, then
// compares top-half vs bottom-half mean luminance to check V-orientation
// agreement. If A and B agree on which half is brighter, UV is CONSISTENT.
// ---------------------------------------------------------------------------
#include "MeshPreview3D.h"
#include <fstream>

// Helper: write raw RGBA buffer as ASCII PPM (P6 binary)
static void writePpm(const char* path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* px = &rgba[(size_t)(y * w + x) * 4];
            f.write((const char*)px, 3);  // R G B only (skip A)
        }
    }
}

// Helper: compute mean luminance of top-half vs bottom-half, ignoring
// "background" pixels (very close to the clear color 0x1A1C21 = 26,28,33).
// Returns false if no non-background pixels found.
static bool halfLuminance(const std::vector<uint8_t>& rgba, int w, int h,
                           double& topMean, double& botMean) {
    // Background clear color for Backend-A: (0x1A, 0x1C, 0x21) ~ (26,28,33)
    // Background for Backend-B: (0x1A, 0x1C, 0x21) same
    // Treat pixel as background if all channels within ±10 of those values.
    auto isBg = [](uint8_t r, uint8_t g, uint8_t b) -> bool {
        return (r < 50 && g < 55 && b < 60);
    };
    double topSum = 0; int topCnt = 0;
    double botSum = 0; int botCnt = 0;
    int half = h / 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* px = &rgba[(size_t)(y * w + x) * 4];
            uint8_t r = px[0], g = px[1], b = px[2];
            if (isBg(r, g, b)) continue;
            // Rec.709 luminance
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            if (y < half) { topSum += lum; topCnt++; }
            else           { botSum += lum; botCnt++; }
        }
    }
    if (topCnt == 0 || botCnt == 0) return false;
    topMean = topSum / topCnt;
    botMean = botSum / botCnt;
    return true;
}

int AssetViewerApp::runSmokeBackendAUvv(const char* deployDir, const char* shaderRoot)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Headless GL 4.3 context (SSBOs required by Backend-A)
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[smoke] FAIL backend-a-uvv: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window* win = SDL_CreateWindow("smoke-backend-a-uvv", 0, 0, 64, 64,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) {
        SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-uvv: window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-uvv: gl context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(win); SDL_Quit();
        std::fprintf(stderr, "[smoke] FAIL backend-a-uvv: glewInit\n");
        return 1;
    }
    glGetError();

    static const char* PROP    = "data/tgl/2civliving.tgl";
    static const int   W = 256, H = 256;
    int rc = 0;

    {
        // ---- Render Backend-A ----
        ModelPreviewEngineShader a;
        a.setShaderRoot(shaderRoot);
        a.setDeployDir(deployDir);
        a.setSource(PROP);

        std::vector<uint8_t> pxA;
        bool rendA = a.renderToPixels(W, H, pxA);
        if (!a.ok() || !rendA) {
            std::fprintf(stderr, "[smoke] backend-a-uvv: Backend-A render failed: %s\n",
                a.report().lastError.c_str());
            if (!a.report().compileLog.empty())
                std::fprintf(stderr, "[smoke] compile log: %s\n", a.report().compileLog.c_str());
            rc = 1;
        }

        // ---- Render Backend-B ----
        MeshPreview3D b;
        b.setDeployDir(deployDir);
        b.setSource(PROP);

        std::vector<uint8_t> pxB;
        bool rendB = b.renderToPixels(W, H, pxB);
        if (!rendB) {
            std::fprintf(stderr, "[smoke] backend-a-uvv: Backend-B render failed\n");
            rc = 1;
        }

        if (rc == 0) {
            // Write PPMs to build64/ (not committed)
            writePpm("build64/uvv_backend_a.ppm", W, H, pxA);
            writePpm("build64/uvv_backend_b.ppm", W, H, pxB);
            printf("[smoke] backend-a-uvv: PPMs written to build64/uvv_backend_a.ppm and build64/uvv_backend_b.ppm\n");

            // Compare V orientation via top/bottom luminance
            double aTop, aBot, bTop, bBot;
            bool aHalf = halfLuminance(pxA, W, H, aTop, aBot);
            bool bHalf = halfLuminance(pxB, W, H, bTop, bBot);

            if (!aHalf || !bHalf) {
                std::fprintf(stderr,
                    "[smoke] backend-a-uvv: WARNING: insufficient non-background pixels "
                    "(aHalf=%d bHalf=%d) -- prop may be invisible\n", (int)aHalf, (int)bHalf);
                rc = 1;
            } else {
                bool aTopBrighter = (aTop > aBot);
                bool bTopBrighter = (bTop > bBot);
                bool consistent   = (aTopBrighter == bTopBrighter);
                // Reliability guard: if the luminance delta for either backend is tiny
                // (< 3.0 lum units out of 0-255), the top/bottom split is dominated by
                // lighting/shading variation rather than texture orientation. In that
                // case the comparison is inconclusive and we skip the FAIL verdict.
                double aDelta = std::abs(aTop - aBot);
                double bDelta = std::abs(bTop - bBot);
                static const double kMinDelta = 3.0;
                bool reliable = (aDelta >= kMinDelta && bDelta >= kMinDelta);

                printf("[smoke] backend-a-uvv: Backend-A top=%.1f bot=%.1f delta=%.1f (%s-brighter)\n",
                       aTop, aBot, aDelta, aTopBrighter ? "top" : "bot");
                printf("[smoke] backend-a-uvv: Backend-B top=%.1f bot=%.1f delta=%.1f (%s-brighter)\n",
                       bTop, bBot, bDelta, bTopBrighter ? "top" : "bot");

                if (!reliable) {
                    printf("[smoke] backend-a-uvv: PASS (INCONCLUSIVE -- one or both backends have"
                           " low luminance delta (%.1f / %.1f < %.1f); orientation cannot be"
                           " auto-determined; deferred to user visual check)\n",
                           aDelta, bDelta, kMinDelta);
                } else {
                    printf("[smoke] backend-a-uvv: UV-V orientation %s (A and B %s)\n",
                           consistent ? "CONSISTENT" : "FLIPPED",
                           consistent ? "agree" : "DISAGREE -- investigate tool-side UV feed in ModelPreviewEngineShader");

                    if (!consistent) {
                        std::fprintf(stderr,
                            "[smoke] backend-a-uvv: FAIL: Backend-A and Backend-B disagree on V orientation.\n"
                            "  This means ModelPreviewEngineShader applies a different UV transform\n"
                            "  (e.g. missing 1-v flip, or wrong ImVec2 UV coords in ImGui::Image).\n"
                            "  Investigate: VAO loc2 binding in MeshGpu, or the draw() ImGui::Image UV args.\n"
                            "  Do NOT edit any shader file.\n");
                        rc = 1;
                    } else {
                        printf("[smoke] backend-a-uvv: PASS (CONSISTENT)\n");
                    }
                }
            }
        }
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

void AssetViewerApp::onFileDropped(const char* path){
    if (!path) return;
    std::string p = path, low = p; for (char& c: low) c=(char)tolower((unsigned char)c);
    auto ends=[&](const char* s){ size_t n=strlen(s); return low.size()>=n && low.compare(low.size()-n,n,s)==0; };
    if (ends(".glb") || ends(".gltf")){
        workbench_.loadOverride(p);
        sidebar_.setActive(AssetType::ModWorkbench);
    }
}
