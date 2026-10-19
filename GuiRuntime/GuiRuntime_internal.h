// Shared surface of the GuiRuntime.cpp translation units (GuiRuntime.cpp, GuiRuntime_draw.cpp).
//
// GuiRuntime.cpp keeps the ImGui lifecycle (init, new frame, render, shutdown), the viewport / dock handling and the font loading;
// the UI draw API is split out (see DOC in the piece). The font-key, colour and texture-id helpers both pieces read live here, inline.
#ifndef GUIRUNTIME_INTERNAL_H
#define GUIRUNTIME_INTERNAL_H

#include "GuiRuntime.h"
#include "EditorInspector.h"
#include "GraphicsOptionsWindow.h"
#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* + ImGuiDockNode (docking branch)
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>    // fprintf
#include <cfloat>
#include <cstdlib>
#include <cstring>   // strcmp
#include <cstdint>   // intptr_t (ImTextureID cast)
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <vector>

#include <GL/glew.h>  // glGetError, glGetIntegerv for Render() diagnostics

#include "../GameOS/gameos/gos_render.h"
#include "../GameOS/gameos/gos_input.h"
#include "Gates/gate.h"
#ifdef MC2_VULKAN
#include "../GameOS/gameos/engine_present_vk.h"
#include "../GameOS/gameos/render_backend_select.h"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef PLATFORM_WINDOWS
#include <SDL2/SDL_syswm.h>
#endif
#else
#include <unistd.h>
#endif

// Globals defined in GuiRuntime.cpp (or elsewhere in the engine) that the split pieces share.
extern void __stdcall gos_InvalidateRenderStateCache();
extern void __stdcall gos_PrepareForPostImGuiRender();
extern bool __stdcall gos_ComputeUiCanvasBox(int w, int h, int* ox, int* oy, int* obw, int* obh);
extern void __stdcall gos_GetEnvironmentDrawableSize(int* w, int* h);
extern bool cliffDecal_hasDecal();
extern void cliffDecal_getKnobs(float* scale, float* offset, float* lateral, float* lift, float* yawDeg, float* pitchDeg);
extern void cliffDecal_setKnobsAndApply(float scale, float offset, float lateral, float lift, float yawDeg, float pitchDeg);
extern void cliffDecal_logValues();
extern bool g_imguiInitialized;
namespace gui_runtime_detail {
extern bool s_frameActive;
extern std::unordered_map<std::string, ImFont*> s_uiFonts;

inline std::string lower(std::string v)
{
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

inline std::string canonicalFamily(std::string_view family)
{
    const std::string f = lower(std::string(family));
    if (f.find("agency") != std::string::npos) {
        if (f.find("bold") != std::string::npos) return "agency_bold";
        return "agency";
    }
    if (f.find("blade") != std::string::npos)
        return "blade_runner";
    if (f.find("narrow") != std::string::npos)
        return "liberation_sans_narrow";
    if (f.find("impact") != std::string::npos || f.find("arialblack") != std::string::npos)
        return "impact";
    if (f.find("cour") != std::string::npos || f.find("cousine") != std::string::npos)
        return "cousine";
    if (f.find("roboto") != std::string::npos)
        return "roboto";
    if (f.find("arial") != std::string::npos || f.find("liberation") != std::string::npos)
        return "liberation_sans";
    return "agency";
}

inline int normalizeSize(int size)
{
    if (size <= 0)
        return 16;
    if (size < 8)
        return 8;
    if (size > 48)
        return 48;
    return size;
}

inline std::string fontKey(std::string_view family, int size)
{
    return std::string(family) + ":" + std::to_string(size);
}

inline ImFont* findUiFont(const char* family, int requestedSize)
{
    if (!g_imguiInitialized || !ImGui::GetCurrentContext())
        return nullptr;

    const std::string fam = canonicalFamily(family ? family : "");
    const int wanted = normalizeSize(requestedSize);
    const int sizes[] = {8, 10, 12, 14, 16, 17, 18, 20, 22, 24, 28, 32, 36, 48};

    int nearest = sizes[0];
    int bestDelta = std::abs(wanted - nearest);
    for (int size : sizes) {
        const int delta = std::abs(wanted - size);
        if (delta < bestDelta) {
            bestDelta = delta;
            nearest = size;
        }
    }

    auto it = s_uiFonts.find(fontKey(fam, nearest));
    if (it != s_uiFonts.end())
        return it->second;
    return ImGui::GetFont();
}

inline ImU32 argbToImU32(std::uint32_t argb)
{
    return IM_COL32((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff);
}

// ImGui 1.91+ defaults ImTextureID to an integer type, while older builds
// commonly use void*.  Keep the UI image path source-compatible with both
// without pulling in backend-specific casts at call sites.
template <typename T>
inline typename std::enable_if<std::is_pointer<T>::value, T>::type makeImTextureID(unsigned int glTextureName)
{
    return reinterpret_cast<T>(static_cast<intptr_t>(glTextureName));
}

template <typename T>
inline typename std::enable_if<!std::is_pointer<T>::value, T>::type makeImTextureID(unsigned int glTextureName)
{
    return static_cast<T>(glTextureName);
}
}  // namespace gui_runtime_detail
using namespace gui_runtime_detail;

#endif // GUIRUNTIME_INTERNAL_H
