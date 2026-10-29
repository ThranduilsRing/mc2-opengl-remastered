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
    DWORD capacity() const { std::lock_guard lock(indexMutex_); return static_cast<DWORD>(assets_.size()); }
    std::vector<std::weak_ptr<TextureSource>> assets() const { std::lock_guard lock(indexMutex_); return assets_; }
    std::shared_ptr<TextureSource> get(DWORD asset) const {
        std::lock_guard lock(indexMutex_);
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
        std::lock_guard lock(indexMutex_);
        if (source->cpuTexture) cpuTextures_[source->cpuTexture] = source;
    }
    std::shared_ptr<TextureSource> fromCpuTexture(DWORD texture) const {
        std::lock_guard lock(indexMutex_);
        auto it = cpuTextures_.find(texture);
        return it == cpuTextures_.end() ? nullptr : it->second.lock();
    }
    // Only the tick appends; main drains after joining. Asset ids never recycle.
    void requestCreate(DWORD asset) { creates_.push_back(asset); }
    std::vector<DWORD> takeCreates() { std::vector<DWORD> out; out.swap(creates_); return out; }
private:
    // Main render lookups may overlap tick admission (vector growth).
    // Lock order: indexMutex_ -> TextureService::mu_. publish() acquires the service handle under indexMutex_ so the
    // asset id and handle land together; nothing may take indexMutex_ while holding mu_. Lookups (find) drop
    // indexMutex_ before calling into the service.
    mutable std::mutex indexMutex_;
    std::vector<DWORD> creates_;
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
