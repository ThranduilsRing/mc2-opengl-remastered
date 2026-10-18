// tools/mc2fx_preview/main.cpp
//
// mc2fx_preview — GUI curve previewer for gosFX .fx blobs.
//
// Loads an .fx blob headless (reuses tools/mc2fx/mc2fx_core.cpp), lists the
// effect catalog, and PLOTS the selected effect's animation curves vs particle
// age (0..1). This is the editing aid for "replacing animations": a modder sees
// the color ramp / alpha fade / scale curve / emission-rate curve being tuned.
//
// NOT a particle-simulation renderer — the real MLR->GL draw path isn't
// available game-free. This is a curve grapher only.
//
// Usage:
//   mc2fx_preview [<mc2.fx>]                       open the GUI
//   mc2fx_preview --headless-smoke --frames N [fx] autonomous verify (no human)
//
// In --headless-smoke mode we try to bring up GL+ImGui and render N frames to
// the default framebuffer, then exit 0. If a GL context can't be created (no
// display / CI), we FALL BACK to exercising the shared curve-sampling path only
// (load + sample + print), still exiting 0 — clearly reporting which mode ran.

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "mc2fx_core.h"

#include <SDL.h>
#include "tool_vk_shell.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"

namespace {

const char* kDefaultFx =
    "A:/Games/mc2-opengl/mc2-win64-v0.4/data/effects/mc2.fx";

bool readFileBytes(const char* path, std::vector<unsigned char>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// ---- model ---------------------------------------------------------------
using EffectRow = mc2fx::CatalogEntry;

struct App {
    mc2::particles::SpecLibrary* lib = nullptr;
    std::vector<EffectRow> effects;
    int selected = -1;

    char filter[128] = {0};
    float seed = 0.5f;
    int   samples = 128;

    // cache of the selected effect's sampled curves
    std::vector<mc2fx::SampledCurve> curves;
    std::string selTypeName;
    std::string selName;
};

void buildCatalog(App& a)
{
    a.effects = mc2fx::effectCatalog(a.lib);
}

void resample(App& a)
{
    a.curves.clear();
    a.selTypeName.clear();
    a.selName.clear();
    if (a.selected < 0 || a.selected >= (int)a.effects.size()) return;
    const char* tn = nullptr;
    const char* nm = nullptr;
    a.curves = mc2fx::sampleEffectCurves(a.lib, a.effects[a.selected].index,
                                         a.samples, a.seed, &tn, &nm);
    if (tn) a.selTypeName = tn;
    if (nm) a.selName = nm;
}

bool loadFx(App& a, const char* fxPath, std::string& err)
{
    std::vector<unsigned char> bytes;
    if (!readFileBytes(fxPath, bytes)) {
        err = std::string("cannot read '") + fxPath + "'";
        return false;
    }
    mc2fx::initEngineHeadless();
    a.lib = mc2fx::loadBlob(bytes.data(), bytes.size());
    if (!a.lib) { err = "loadBlob failed"; return false; }
    buildCatalog(a);
    return true;
}

// ---- UI ------------------------------------------------------------------
void drawEffectList(App& a)
{
    ImGui::Begin("Effects");
    ImGui::Text("%zu effects", a.effects.size());
    ImGui::InputText("filter", a.filter, sizeof a.filter);
    ImGui::Separator();
    ImGui::BeginChild("list", ImVec2(0, 0), false);
    std::string flt = a.filter;
    std::transform(flt.begin(), flt.end(), flt.begin(), ::tolower);
    for (int i = 0; i < (int)a.effects.size(); ++i) {
        const EffectRow& r = a.effects[i];
        if (!flt.empty()) {
            std::string lname = r.name;
            std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
            if (lname.find(flt) == std::string::npos) continue;
        }
        char label[256];
        std::snprintf(label, sizeof label, "%s  [%s]##%d",
                      r.name.c_str(), r.typeName.c_str(), i);
        if (ImGui::Selectable(label, a.selected == i)) {
            a.selected = i;
            resample(a);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

ImVec4 groupColor(const std::string& g)
{
    if (g == "color")    return ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
    if (g == "scale")    return ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
    if (g == "emission") return ImVec4(0.6f, 1.0f, 0.4f, 1.0f);
    if (g == "lifetime") return ImVec4(1.0f, 0.9f, 0.3f, 1.0f);
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

// Plot one sampled curve.
void plotCurve(const mc2fx::SampledCurve& c)
{
    float lo = c.samples.empty() ? 0.0f : c.samples[0];
    float hi = lo;
    for (float v : c.samples) { lo = std::min(lo, v); hi = std::max(hi, v); }
    char overlay[160];
    std::snprintf(overlay, sizeof overlay, "%s (%s)%s  [%.3g .. %.3g]",
                  c.name.c_str(), c.type.c_str(),
                  c.constant ? " const" : "", lo, hi);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, groupColor(c.group));
    ImGui::PlotLines(("##" + c.name).c_str(),
                     c.samples.data(), (int)c.samples.size(),
                     0, overlay, lo, hi, ImVec2(0, 70));
    ImGui::PopStyleColor();
}

// Draw a small RGBA strip across age for color curves, if present.
void drawColorStrip(App& a, const char* rName, const char* gName,
                    const char* bName, const char* aName)
{
    const mc2fx::SampledCurve *R=nullptr,*G=nullptr,*B=nullptr,*A=nullptr;
    for (auto& c : a.curves) {
        if (c.name == rName) R = &c;
        else if (c.name == gName) G = &c;
        else if (c.name == bName) B = &c;
        else if (c.name == aName) A = &c;
    }
    if (!R || !G || !B) return;
    ImGui::Text("RGBA over age:");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (w < 16) w = 16;
    float h = 22.0f;
    int n = (int)R->samples.size();
    for (int k = 0; k < n; ++k) {
        float t0 = p0.x + w * (k / (float)n);
        float t1 = p0.x + w * ((k + 1) / (float)n);
        auto clamp01 = [](float v){ return v < 0 ? 0.f : (v > 1 ? 1.f : v); };
        float r = clamp01(R->samples[k]);
        float g = clamp01(G->samples[k]);
        float b = clamp01(B->samples[k]);
        float al = A ? clamp01(A->samples[k]) : 1.0f;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, al));
        dl->AddRectFilled(ImVec2(t0, p0.y), ImVec2(t1, p0.y + h), col);
    }
    ImGui::Dummy(ImVec2(w, h + 4));
}

void drawCurvePanel(App& a)
{
    ImGui::Begin("Curves");
    if (a.selected < 0) {
        ImGui::TextDisabled("Select an effect from the list.");
        ImGui::End();
        return;
    }
    ImGui::Text("%s  [%s]", a.selName.c_str(), a.selTypeName.c_str());
    bool dirty = false;
    if (ImGui::SliderFloat("seed", &a.seed, 0.0f, 1.0f)) dirty = true;
    if (ImGui::SliderInt("samples", &a.samples, 8, 256)) dirty = true;
    if (dirty) resample(a);
    ImGui::Separator();

    if (a.curves.empty()) {
        ImGui::TextDisabled("no decodable curves for this type");
        ImGui::End();
        return;
    }

    // RGBA strips (particle colors + singleton colors, whichever exist).
    drawColorStrip(a, "m_pRed", "m_pGreen", "m_pBlue", "m_pAlpha");
    drawColorStrip(a, "m_red",  "m_green",  "m_blue",  "m_alpha");
    ImGui::Separator();

    // Group plots by category for readability.
    const char* groups[] = {"color","scale","emission","lifetime","base","seed"};
    for (const char* grp : groups) {
        bool header = false;
        for (auto& c : a.curves) {
            if (c.group != grp) continue;
            if (!header) {
                ImGui::TextColored(groupColor(grp), "%s", grp);
                header = true;
            }
            plotCurve(c);
        }
    }
    ImGui::End();
}

// ---- headless smoke: sample-only fallback --------------------------------
int sampleOnlySmoke(App& a)
{
    std::printf("[mode] sample-only fallback (no GL window)\n");
    std::printf("loaded %u effects\n", mc2fx::effectCount(a.lib));
    // Sample curves for a few effects and print representative values.
    unsigned probes[] = {0u, 1u, 2u};
    for (unsigned pi : probes) {
        if (pi >= mc2fx::effectCount(a.lib)) continue;
        const char* tn = nullptr; const char* nm = nullptr;
        std::vector<mc2fx::SampledCurve> cs =
            mc2fx::sampleEffectCurves(a.lib, pi, 128, 0.5f, &tn, &nm);
        std::printf("sampled curves for %s [%s]: %zu curves\n",
                    nm ? nm : "?", tn ? tn : "?", cs.size());
        for (auto& c : cs) {
            if (c.samples.empty()) continue;
            std::printf("   %-22s %-14s [0]=%.4f [%d]=%.4f\n",
                        c.name.c_str(), c.type.c_str(),
                        c.samples.front(), (int)c.samples.size() - 1,
                        c.samples.back());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    // --- arg parse ---
    bool headlessSmoke = false;
    int frames = 30;
    const char* fxPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--headless-smoke") headlessSmoke = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a.size() && a[0] != '-') fxPath = argv[i];
    }
    if (!fxPath) fxPath = kDefaultFx;

    App app;
    std::string err;
    if (!loadFx(app, fxPath, err)) {
        std::fprintf(stderr, "mc2fx_preview: load error: %s\n", err.c_str());
        return 2;
    }
    std::printf("mc2fx_preview: loaded %u effects from %s\n", mc2fx::effectCount(app.lib), fxPath);

    // --- bring up SDL + GL + ImGui ---
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        if (headlessSmoke) return sampleOnlySmoke(app);  // fallback
        return 3;
    }

    Uint32 winFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (headlessSmoke || toolvk::captureRequested()) winFlags |= SDL_WINDOW_HIDDEN;
    SDL_Window* window = SDL_CreateWindow(
        "mc2fx_preview - gosFX curve previewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800, winFlags);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        if (headlessSmoke) return sampleOnlySmoke(app);  // fallback
        return 3;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    toolvk::Shell vk;
    if (!toolvk::init(vk, window, "mc2fx_preview")) {
        std::fprintf(stderr, "Vulkan shell init failed\n");
        toolvk::shutdown(vk);
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        if (headlessSmoke) return sampleOnlySmoke(app);  // fallback
        return 3;
    }
    // The launch-and-screenshot proof (MC2_TOOL_CAPTURE) shows a plotted effect, not the empty selection.
    if (std::getenv("MC2_TOOL_CAPTURE") && mc2fx::effectCount(app.lib) > 0) {
        app.selected = 0;
        resample(app);
    }

    std::printf("[mode] full-VK-headless-smoke (Vulkan shell + ImGui up)\n");
    if (headlessSmoke) {
        // Print sampling evidence too (proves the shared core path under GL).
        const char* tn = nullptr; const char* nm = nullptr;
        if (mc2fx::effectCount(app.lib) > 0) {
            std::vector<mc2fx::SampledCurve> cs =
                mc2fx::sampleEffectCurves(app.lib, 0, 128, 0.5f, &tn, &nm);
            std::printf("sampled curves for %s [%s]: %zu curves\n",
                        nm ? nm : "?", tn ? tn : "?", cs.size());
            for (auto& c : cs) {
                if (c.samples.empty()) continue;
                std::printf("   %-22s %-14s [0]=%.4f [127]=%.4f\n",
                            c.name.c_str(), c.type.c_str(),
                            c.samples.front(), c.samples.back());
            }
        }
        // select effect 0 so the curve panel exercises real plotting code
        app.selected = 0;
        resample(app);
    }

    // --- main loop ---
    bool running = true;
    int frame = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        if (!toolvk::begin(vk)) { SDL_Delay(10); continue; }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        drawEffectList(app);
        drawCurvePanel(app);

        ImGui::Render();
        if (toolvk::end(vk)) running = false;

        ++frame;
        if (headlessSmoke && frame >= frames) running = false;
    }

    if (headlessSmoke)
        std::printf("rendered %d frames, exiting cleanly\n", frame);

    toolvk::shutdown(vk);
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
