#include "TexturePreview2D.h"
#include "TextureDecoderRegistry.h"
#include "imgui.h"
#include "tool_vk_gpu.h"
#include <filesystem>
#include <system_error>

void TexturePreview2D::releaseOwned()
{
    if (current_.ownsTexture && current_.texId) toolvk::destroyTexture(current_.texId);
    current_ = DecodedTexture{};
}

TexturePreview2D::~TexturePreview2D() { releaseOwned(); }

void TexturePreview2D::setSource(const std::string& path)
{
    releaseOwned();                 // free previous owned texture before replacing
    path_ = path;
    meta_ = TextureMetadata{};
    hasError_ = false;
    errorText_.clear();

    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (!ec) meta_.fileBytes = sz;

    current_ = textureDecoderRegistry().load(path);
    if (!current_.error.empty() || current_.texId == 0) {
        hasError_ = true;
        errorText_ = current_.error.empty() ? "Failed to load texture." : current_.error;
        // Do not hold a half-result that could be mistaken for a live texture.
        current_ = DecodedTexture{};
        return;
    }
    meta_.width       = current_.width;
    meta_.height      = current_.height;
    meta_.channels    = 0;
    meta_.formatLabel = current_.formatLabel;
    meta_.mipCount    = current_.mipCount;
}

void TexturePreview2D::draw(const ImVec2& availableSize)
{
    if (hasError_) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", errorText_.c_str());
        ImGui::TextWrapped("Path: %s", path_.c_str());
        return;
    }
    if (current_.texId == 0) {
        ImGui::TextDisabled("No texture selected.");
        return;
    }
    // Zoom is now a multiple of the fit-to-region size, so source resolution
    // (128/256/512) no longer changes the on-screen size. 1.00x == fit.
    ImGui::SliderFloat("Zoom", &zoom_, 0.25f, 8.0f, "%.2fx (fit)");
    ImGui::BeginChild("tex_scroll", availableSize, true, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 region = ImGui::GetContentRegionAvail();
    FitSize fs = FitTextureDisplaySize(meta_.width, meta_.height, region.x, region.y, zoom_);
    ImGui::Image((ImTextureID)toolvk::imguiTex(current_.texId), ImVec2(fs.w, fs.h));
    ImGui::EndChild();
}
