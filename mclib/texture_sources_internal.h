// Engine-side VFS sources. TextureService owns the index and each live payload.
#pragma once
#include "texture_assets.h"
#include "../RenderCore/TextureService.h"
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mc2::textures {

struct CpuTextureRetireQueue {
    std::mutex mutex;
    std::vector<DWORD> pending;
};

struct TextureSource final : RenderCore::TextureSourcePayload {
    ~TextureSource() override;
    std::string name;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    RenderCore::TextureKey key;
    RenderCore::TextureHandle handle;
    DWORD asset = 0xffffffffu;
    DWORD format = 0, hints = 0, variant = 0, lifetime = 0;
    DWORD users = 1, pins = 0;
    DWORD ownedReferences = 0;
    DWORD uvScale = 1, width = 0, height = 0;
    DWORD logicalWidth = 0, logicalHeight = 0;
    DWORD cpuTexture = 0;
    bool file = false;
    std::shared_ptr<CpuTextureRetireQueue> retired;
};

class TextureSources final : public RenderCore::TextureSourceOwner {
public:
    explicit TextureSources(std::function<void(DWORD)> retireCpu = {}) : retireCpu_(std::move(retireCpu)) { assets_.resize(1); }
    void collectRetired() override;
    DWORD capacity() const { return static_cast<DWORD>(assets_.size()); }
    const std::vector<std::weak_ptr<TextureSource>>& assets() const { return assets_; }
    std::shared_ptr<TextureSource> get(DWORD asset) const {
        return asset < assets_.size() ? assets_[asset].lock() : nullptr;
    }
    std::shared_ptr<TextureSource> find(const char* name, DWORD variant);
    DWORD publish(std::shared_ptr<TextureSource> source);
    bool retain(const std::shared_ptr<TextureSource>& source);
    void release(const std::shared_ptr<TextureSource>& source);
    void flush(bool all);
    std::shared_ptr<const std::vector<uint8_t>> internBytes(
        const std::string& identity, std::shared_ptr<const std::vector<uint8_t>> bytes);
    void noteCpuTexture(const std::shared_ptr<TextureSource>& source) {
        if (source->cpuTexture) cpuTextures_[source->cpuTexture] = source;
    }
    std::shared_ptr<TextureSource> fromCpuTexture(DWORD texture) const {
        auto it = cpuTextures_.find(texture);
        return it == cpuTextures_.end() ? nullptr : it->second.lock();
    }
private:
    static std::string nameKey(const char* name, DWORD variant);
    // Weak indices never extend payload lifetime. IDs are monotonic for the session;
    // retiring a source cannot make a stale DWORD name a different source later.
    std::vector<std::weak_ptr<TextureSource>> assets_;
    std::unordered_map<std::string, DWORD> named_;
    std::unordered_map<DWORD, std::weak_ptr<TextureSource>> cpuTextures_;
    std::unordered_map<std::string, std::weak_ptr<const std::vector<uint8_t>>> bytes_;
    std::function<void(DWORD)> retireCpu_;
    std::shared_ptr<CpuTextureRetireQueue> retired_ = std::make_shared<CpuTextureRetireQueue>();
};

TextureSources* sourcesIfReady();
std::shared_ptr<TextureSource> liveSource(DWORD asset);
void queueCpuTexture(TextureSource& source);
TextureSources& sourceStore();
void startSources();
void flushSources(bool all);

} // namespace mc2::textures
