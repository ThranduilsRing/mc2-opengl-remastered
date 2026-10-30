#pragma once
#include "imgui.h"

namespace mc2vk {
// ImGui::NewFrame may reuse the producer's buffers after sealing.
class PresentImguiSnapshot {
public:
    ~PresentImguiSnapshot() { clear(); }
    PresentImguiSnapshot() = default;
    PresentImguiSnapshot(const PresentImguiSnapshot&) = delete;
    PresentImguiSnapshot& operator=(const PresentImguiSnapshot&) = delete;
    void clear() {
        for (ImDrawList* list : data.CmdLists) IM_DELETE(list);
        data.Clear();
    }
    bool assign(const ImDrawData& source) {
        clear();
        for (ImDrawList* list : source.CmdLists)
            for (const ImDrawCmd& command : list->CmdBuffer)
                if (command.UserCallback && command.UserCallback != ImDrawCallback_ResetRenderState) return false;
        data = source;
        data.CmdLists.clear();
        for (ImDrawList* list : source.CmdLists) {
            ImDrawList* copy = list->CloneOutput();
            for (ImDrawCmd& command : copy->CmdBuffer) {
                command.UserCallbackData = nullptr;
                command.UserCallbackDataSize = 0;
                command.UserCallbackDataOffset = -1;
            }
            data.CmdLists.push_back(copy);
        }
        return true;
    }
    ImDrawData data;
};
}
