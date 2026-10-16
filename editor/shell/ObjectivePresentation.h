#pragma once
#include "ResourceStrings.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace editor::shell {
// The string owns every line, including empty final lines; no line-list conversion.
inline bool textControl(const char* label, std::string& text, float height = 96)
{
    return ImGui::InputTextMultiline(label, &text, ImVec2(-1, height), ImGuiInputTextFlags_CtrlEnterForNewLine);
}
inline void resourcePreview(const document::ResourceStrings* strings, std::int64_t id)
{
    if (!strings) { ImGui::TextUnformatted("Resource tables unavailable; authored ID retained."); return; }
    const auto resolved = strings->resolve(id);
    if (!resolved.found) { ImGui::Text("Missing resource ID: %lld (retained)", static_cast<long long>(id)); return; }
    ImGui::TextWrapped("%s / %s", resolved.key.c_str(), resolved.source.layer.c_str());
    ImGui::TextUnformatted(resolved.text.data(), resolved.text.data() + resolved.text.size());
}
}
