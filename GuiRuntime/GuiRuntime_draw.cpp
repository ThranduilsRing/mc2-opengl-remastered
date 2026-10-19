// GuiRuntime: IsReadyForUiText and the DrawUi* / CalcUiTextWrapLines / DrawUiEditBox immediate-mode draw API.
// Declared in the class headers; shared names in GuiRuntime_internal.h.
#include "GuiRuntime_internal.h"


bool GuiRuntime::IsReadyForUiText()
{
    return g_imguiInitialized && s_frameActive && ImGui::GetCurrentContext() != nullptr;
}


bool GuiRuntime::DrawUiRect(float x,
                            float y,
                            float w,
                            float h,
                            std::uint32_t argb,
                            bool filled)
{
    if (!IsReadyForUiText() || w <= 0.0f || h <= 0.0f)
        return false;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    const ImVec2 a(x, y);
    const ImVec2 b(x + w, y + h);
    if (filled)
        drawList->AddRectFilled(a, b, argbToImU32(argb));
    else
        drawList->AddRect(a, b, argbToImU32(argb));
    return true;
}

bool GuiRuntime::DrawUiTriangle(float x, float y, float w, float h, std::uint32_t argb)
{
    if (!IsReadyForUiText() || w <= 0.0f || h <= 0.0f)
        return false;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    // Down-pointing filled triangle centered in the given rect.
    // Occupies ~64% of width and ~56% of height, leaving visual padding.
    const float cx  = x + w * 0.5f;
    const float tw  = w * 0.32f;
    const float th  = h * 0.28f;
    const float top = y + (h - th) * 0.5f;
    drawList->AddTriangleFilled(
        ImVec2(cx - tw, top),
        ImVec2(cx + tw, top),
        ImVec2(cx,      top + th),
        argbToImU32(argb));
    return true;
}

bool GuiRuntime::DrawUiCircle(float cx, float cy, float radius, std::uint32_t argb, bool filled)
{
    if (!IsReadyForUiText() || radius <= 0.0f)
        return false;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    // num_segments 0 = ImGui auto-tessellates for a smooth circle at this radius.
    if (filled)
        drawList->AddCircleFilled(ImVec2(cx, cy), radius, argbToImU32(argb), 0);
    else
        drawList->AddCircle(ImVec2(cx, cy), radius, argbToImU32(argb), 0, 1.5f);
    return true;
}

bool GuiRuntime::DrawUiTriangleVerts(float x0, float y0, float x1, float y1,
                                     float x2, float y2, std::uint32_t argb)
{
    if (!IsReadyForUiText())
        return false;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    drawList->AddTriangleFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x2, y2),
                                argbToImU32(argb));
    return true;
}

bool GuiRuntime::PushUiClipRect(float x, float y, float w, float h)
{
    if (!IsReadyForUiText() || w <= 0.0f || h <= 0.0f)
        return false;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    drawList->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true /*intersect*/);
    return true;
}

void GuiRuntime::PopUiClipRect()
{
    if (!IsReadyForUiText())
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (drawList)
        drawList->PopClipRect();
}

bool GuiRuntime::DrawUiImage(std::uint64_t glTextureName,
                             float x,
                             float y,
                             float w,
                             float h,
                             float u1,
                             float v1,
                             float u2,
                             float v2,
                             std::uint32_t argb,
                             bool nearest,
                             bool alphaTest)
{
    if (!IsReadyForUiText() || glTextureName == 0 || w <= 0.0f || h <= 0.0f)
        return false;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    ImTextureID tex = makeImTextureID<ImTextureID>(glTextureName);
    // UI-PHASE1-INTEGRATION-GAP: the ui-phase1 fork's DrawUiImage() used a
    // nearest-sampler draw-callback (pio.DrawCallback_SetSamplerNearest /
    // ...Linear — a real upstream ImGui platform_io feature, but newer than
    // the version vendored in this repo's 3rdparty/imgui) plus custom
    // ImGui_ImplOpenGL3_GetAlphaTestOnCallback/OffCallback functions that
    // don't exist anywhere in this tree — they must live in the fork's own
    // patched imgui_impl_opengl3.cpp/.h, which wasn't included in their file
    // drop (their manifest listed zero 3rdparty/imgui files). Degrading to a
    // plain AddImage here so this compiles; `nearest`/`alphaTest` params are
    // accepted but currently no-ops. To restore crisp pixel-art sampling +
    // binary alpha-test cutout for mission markers, either (a) upgrade the
    // vendored ImGui to a version with DrawCallback_SetSamplerNearest/Linear
    // and port their alpha-test callback additions into
    // imgui_impl_opengl3.cpp/.h, or (b) get those two files from the
    // modder directly.
    (void)nearest;
    (void)alphaTest;
    drawList->AddImage(tex, ImVec2(x, y), ImVec2(x + w, y + h),
                       ImVec2(u1, v1), ImVec2(u2, v2), argbToImU32(argb));
    return true;
}

bool GuiRuntime::DrawUiText(float x,
                            float y,
                            float w,
                            float h,
                            const char* text,
                            std::uint32_t argb,
                            int fontSize,
                            int align,
                            const char* fontFamily)
{
    if (!text || !text[0] || !IsReadyForUiText())
        return false;

    ImFont* font = findUiFont(fontFamily, fontSize);
    if (!font)
        return false;

    const float size = static_cast<float>(normalizeSize(fontSize));
    const float wrapWidth = (w > 0.0f) ? w : 0.0f;
    ImVec2 pos(x, y);

    const char* textEnd = nullptr;
    ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, wrapWidth, text, textEnd);
    if (align == 1 && w > 0.0f) {
        pos.x = x + std::max(0.0f, w - textSize.x);
    } else if (align == 2 && w > 0.0f) {
        pos.x = x + std::max(0.0f, (w - textSize.x) * 0.5f);
        if (h > 0.0f)
            pos.y = y + std::max(0.0f, (h - textSize.y) * 0.5f);
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return false;

    const ImVec4 clip(x, y, x + std::max(w, 0.0f), y + std::max(h, size));
    drawList->AddText(font, size, pos, argbToImU32(argb), text, nullptr, wrapWidth, &clip);
    return true;
}

bool GuiRuntime::CalcUiTextWrapLines(const char* text,
                                     float wrapWidth,
                                     int fontSize,
                                     const char* fontFamily,
                                     std::vector<std::string>& outLines)
{
    outLines.clear();
    if (!text || wrapWidth <= 0.0f || !IsReadyForUiText())
        return false;

    ImFont* font = findUiFont(fontFamily, fontSize);
    if (!font)
        return false;

    // Mirror DrawUiText: it renders at normalizeSize(fontSize); wrap positions are
    // computed with the same scale ImGui uses internally (size / font->FontSize).
    const float size  = static_cast<float>(normalizeSize(fontSize));
    const float scale = (font->FontSize > 0.0f) ? size / font->FontSize : 1.0f;

    const char* s   = text;
    const char* end = text + std::strlen(text);
    while (s <= end) {
        const char* nl     = static_cast<const char*>(memchr(s, '\n', end - s));
        const char* segEnd = nl ? nl : end;
        if (s == segEnd) {
            outLines.push_back(std::string());
        } else {
            const char* p = s;
            while (p < segEnd) {
                const char* wrap = font->CalcWordWrapPositionA(scale, p, segEnd, wrapWidth);
                if (wrap <= p)
                    wrap = p + 1;  // guarantee forward progress (ultra-narrow width)
                outLines.push_back(std::string(p, wrap));
                p = wrap;
                // ImGui's renderer skips inter-word blanks at the start of the
                // next wrapped line (CalcWordWrapNextLineStartA) -- match it.
                while (p < segEnd && (*p == ' ' || *p == '\t'))
                    ++p;
            }
        }
        if (!nl)
            break;
        s = nl + 1;
    }
    return true;
}

bool GuiRuntime::DrawUiEditBox(const char* id,
                                float x, float y, float w, float h,
                                char* buf, std::size_t bufSize,
                                std::uint32_t fillArgb,
                                std::uint32_t borderArgb,
                                std::uint32_t textArgb,
                                int fontSize, const char* fontFamily,
                                bool requestFocus)
{
    if (!IsReadyForUiText() || !buf || bufSize == 0 || w <= 0.0f || h <= 0.0f || !id)
        return false;

    ImFont* font = findUiFont(fontFamily, fontSize);
    const float sz  = static_cast<float>(normalizeSize(fontSize));
    const float padY = std::max(1.0f, (h - sz) * 0.5f - 1.0f);

    // Convert ARGB to ImVec4 for style colors.
    auto toVec4 = [](ImU32 c) -> ImVec4 {
        return ImVec4(((c >> IM_COL32_R_SHIFT) & 0xff) / 255.0f,
                      ((c >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
                      ((c >> IM_COL32_B_SHIFT) & 0xff) / 255.0f,
                      ((c >> IM_COL32_A_SHIFT) & 0xff) / 255.0f);
    };
    const ImU32   fillU32   = argbToImU32(fillArgb);
    const ImU32   borderU32 = argbToImU32(borderArgb);
    const ImVec4  fillV4    = toVec4(fillU32);
    const ImVec4  textV4    = toVec4(argbToImU32(textArgb));

    // Transparent, borderless ImGui window pinned to the edit box rect.
    // NoBringToFrontOnFocus / NoFocusOnAppearing keep it from jumping to the
    // top of the window stack whenever the user clicks into it.
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(3.0f, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text,           textV4);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        fillV4);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, fillV4);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  fillV4);
    ImGui::PushStyleColor(ImGuiCol_Border,         toVec4(borderU32));

    static constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration        |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoSavedSettings     |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing  |
        ImGuiWindowFlags_NoNav;

    bool hasFocus = false;
    if (ImGui::Begin(id, nullptr, kFlags)) {
        if (font) ImGui::PushFont(font);
        ImGui::SetNextItemWidth(-1.0f);
        if (requestFocus)
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##e", buf, bufSize, ImGuiInputTextFlags_NoHorizontalScroll);
        hasFocus = ImGui::IsItemActive();
        if (font) ImGui::PopFont();
    }
    ImGui::End();

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);
    return hasFocus;
}
