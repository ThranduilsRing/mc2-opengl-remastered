// RenderCore/TextureService.h -- texture identity in front of the texture manager's node index (shared-foundations P4a).
//
// Three layers (design section 3): a KEY says what the texture is and decides deduplication; a HANDLE names it for the session (generation-checked, never
// saved, never read by sim); the RESIDENCY (today the manager's node index, later a bindless slot) may change while the handle stays. This service is the
// façade for the first two layers over the manager that still owns the pixels: the manager reports each named node it creates, reuses and destroys, and
// the service keeps key <-> handle <-> node. It changes no behavior: nothing reads a node through a handle yet.
//
// Content change (a repaint, a refresh) keeps the handle and moves it to a new node: rebind(). The handle's content version counts the changes, which is
// what a material that cached the old residency compares.
//
// Pure standard C++ (doctest-driven); the engine's instance is textureService().
#pragma once

#include <cassert>
#include <functional>
#include <cstdint>
#include <utility>
#include <mutex>
#include <memory>
#include "KtxLoader.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Handle.h"
#include "MaterialRegistry.h"

namespace RenderCore {

constexpr uint32_t kNoTextureNode = 0xFFFFFFFFu;

// What a texture IS. `source` is the resolved source id (see makeTextureSourceId); `variant` separates instances of one source (the manager's uniqueInstance,
// a paint scheme hash); `format` and `hints` are the usage: colour space + alpha policy (the format) and mip policy (the hints).
struct TextureKey {
    std::string source;
    uint32_t    cookVersion = 1;
    uint32_t    variant = 0;
    uint32_t    format = 0;
    uint32_t    hints = 0;
    bool operator==(const TextureKey& o) const {
        return cookVersion == o.cookVersion && variant == o.variant && format == o.format && hints == o.hints && source == o.source;
    }
};

struct TextureKeyHash {
    size_t operator()(const TextureKey& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        for (unsigned char c : k.source) mix(c);
        mix(k.cookVersion);
        mix(k.variant);
        mix(k.format);
        mix(k.hints);
        return static_cast<size_t>(h);
    }
};

// The resolved source id: the provider that supplied the bytes (mod, dependency, base, FST), the case-folded path with '/' separators, and the content hash
// of the bytes actually read. A mod file that outranks a base file resolves to a different provider and hash, so it is a different key; the same file
// reached through different spellings of its path is one key.
inline std::string makeTextureSourceId(std::string_view provider, std::string_view path, uint64_t contentHash) {
    std::string id(provider);
    id += '|';
    for (char c : path) {
        if (c == static_cast<char>(92)) c = '/';   // a backslash
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        id += c;
    }
    id += '|';
    static const char* kHex = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) id += kHex[(contentHash >> shift) & 0xF];
    return id;
}

// Descriptor zero is always a visible placeholder. Handles, legacy nodes and descriptor indices are distinct identities.
inline constexpr uint32_t kPlaceholderDescriptor = 0;
struct TextureDimensions {
    uint32_t width = 16, height = 16;
    uint32_t logicalWidth = 16, logicalHeight = 16;
};

// Choose provider before representation: a mod TGA beats base KTX even in prefer-cooked mode.
inline bool preferCookedSource(bool sourceExists, int sourcePriority, int cookedPriority, bool preferCooked) {
    return !sourceExists || cookedPriority < sourcePriority || (cookedPriority == sourcePriority && preferCooked);
}
inline uint64_t textureContentHash(const uint8_t* bytes, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

inline constexpr uint64_t kUiTextureTag = 0x54584d3100000000ull;
inline uint64_t uiTextureId(TextureHandle handle) { return handle.isValid() ? kUiTextureTag | handle.raw() : 0; }
inline bool isUiTextureId(uint64_t id) { return (id & 0xffffffff00000000ull)==kUiTextureTag; }
inline TextureHandle uiTextureHandle(uint64_t id) {
    return isUiTextureId(id) ? TextureHandle::make(uint32_t(id) & 0xfffffu, uint32_t(id) >> 20) : TextureHandle::invalid();
}

class TextureService {
public:
    // Render-thread material identity shares the texture service session lifetime.
    MaterialRegistry& materials() { return materials_; }
    std::function<void(TextureHandle)> onRetired; // context residency borrows the session; callback only queues invalidations
    // The handle of `key`: a new one bound to `node`, or the existing one (its user count rises; `node` is ignored, the first residency stays).
    TextureHandle acquire(const TextureKey& key, uint32_t node = kNoTextureNode, TextureDimensions dimensions = {}) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto it = byKey_.find(key); it != byKey_.end()) {
            const auto& [k, idx] = *it;
            Entry& e = entries_[idx];
            if (e.users == UINT32_MAX) return TextureHandle::invalid();
            ++e.users;
            return TextureHandle::make(idx, e.generation);
        }
        uint32_t idx;
        if (!free_.empty()) { idx = free_.back(); free_.pop_back(); }
        else {
            if (entries_.size() >= 0x100000u) return TextureHandle::invalid();
            idx = static_cast<uint32_t>(entries_.size()); entries_.emplace_back();
        }
        Entry& e = entries_[idx];
        e.live = true;
        e.key = key;
        e.node = node;
        e.users = 1;
        e.contentVersion = 0;
        e.dimensions = dimensions;
        byKey_.emplace(key, idx);
        if (node != kNoTextureNode) byNode_[node] = idx;
        return TextureHandle::make(idx, e.generation);
    }

    // acquire returns an already-counted reference (adopt it); sharing requires retain. Legacy nodes remain owned by txmmgr until P9.
    bool retain(TextureHandle h) {
        std::lock_guard<std::mutex> lk(mu_);
        Entry* e = resolve(h);
        if (!e || e->users == UINT32_MAX) return false;
        ++e->users;
        return true;
    }
    bool release(TextureHandle h) {
        std::lock_guard<std::mutex> lk(mu_);
        Entry* e = resolve(h);
        if (!e || !e->users) return false;
        if (--e->users == 0 && e->node == kNoTextureNode) retire(h.index());
        return true;
    }
    TextureDimensions dimensions(TextureHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        const Entry* e = resolve(h);
        return e ? e->dimensions : TextureDimensions{};
    }

    std::shared_ptr<const KtxImage> cooked(const TextureKey& key) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = cooked_.find(key); return it == cooked_.end() ? nullptr : it->second;
    }
    void cacheCooked(const TextureKey& key, std::shared_ptr<const KtxImage> image) {
        std::lock_guard<std::mutex> lk(mu_); cooked_[key] = std::move(image);
    }
    // One user of the node's texture let go (mirrors the manager's reference count). The entry stays until the node dies.
    void dropRef(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto it = byNode_.find(node); it != byNode_.end()) {
            const auto& [n, idx] = *it;
            if (entries_[idx].users > 0) --entries_[idx].users;
        }
    }

    // The node died: the entry goes, its generation bumps (a held handle is stale), its index is reusable.
    void onNodeDestroyed(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = byNode_.find(node);
        if (it == byNode_.end()) return;
        const auto& [n, idx] = *it;
        retire(idx);
        byNode_.erase(it);
    }

    // The same texture now lives in `newNode` (new pixels): the handle stays, the content version rises.
    bool rebind(TextureHandle h, uint32_t newNode) {
        std::lock_guard<std::mutex> lk(mu_);
        Entry* e = resolve(h);
        if (!e) return false;
        if (e->node != kNoTextureNode) byNode_.erase(e->node);
        e->node = newNode;
        ++e->contentVersion;
        if (newNode != kNoTextureNode) byNode_[newNode] = h.index();
        return true;
    }

    // The residency of a live handle; kNoTextureNode for a stale or invalid one.
    uint32_t nodeOf(TextureHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        const Entry* e = resolve(h);
        return e ? e->node : kNoTextureNode;
    }
    TextureHandle handleOfNode(uint32_t node) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = byNode_.find(node);
        if (it == byNode_.end()) return TextureHandle::invalid();
        const auto& [n, idx] = *it;
        return TextureHandle::make(idx, entries_[idx].generation);
    }
    uint32_t contentVersion(TextureHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        const Entry* e = resolve(h);
        return e ? e->contentVersion : 0u;
    }
    uint32_t users(TextureHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        const Entry* e = resolve(h);
        return e ? e->users : 0u;
    }
    bool isLive(TextureHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        return resolve(h) != nullptr;
    }
    uint32_t liveCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return static_cast<uint32_t>(byKey_.size());
    }
    // A mission boundary: every entry goes (the manager's nodes are all destroyed with the mission).
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        for (uint32_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].live) retire(i);
        byNode_.clear();
        cooked_.clear();
    }

private:
    MaterialRegistry materials_;
    struct Entry {
        bool        live = false;
        uint32_t    generation = 1;
        uint32_t    node = kNoTextureNode;
        uint32_t    users = 0;
        uint32_t    contentVersion = 0;
        TextureKey  key;
        TextureDimensions dimensions;
    };
    const Entry* resolve(TextureHandle h) const {
        if (!h.isValid() || h.index() >= entries_.size()) return nullptr;
        const Entry& e = entries_[h.index()];
        return (e.live && e.generation == h.generation()) ? &e : nullptr;
    }
    Entry* resolve(TextureHandle h) { return const_cast<Entry*>(static_cast<const TextureService*>(this)->resolve(h)); }
    void retire(uint32_t idx) {
        Entry& e = entries_[idx];
        if (onRetired) onRetired(TextureHandle::make(idx,e.generation));
        byKey_.erase(e.key);
        e.live = false;
        e.node = kNoTextureNode;
        // Quarantine exhausted slots: wrapping would revive an old CPU handle.
        if (e.generation < 0xFFFu) { ++e.generation; free_.push_back(idx); }
        e.key = TextureKey{};
    }

    std::unordered_map<TextureKey, std::shared_ptr<const KtxImage>, TextureKeyHash> cooked_;
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_;
    std::unordered_map<TextureKey, uint32_t, TextureKeyHash> byKey_;
    std::unordered_map<uint32_t, uint32_t> byNode_;
};

// Immutable descriptor snapshots retain image owners until that flight completes. Reusing a slot in a newer
// snapshot cannot change the view or ownership in an older submitted snapshot.
class TextureDescriptorTable {
public:
    struct Slot { TextureHandle handle; std::shared_ptr<void> owner; };
    explicit TextureDescriptorTable(uint32_t capacity) : capacity_(capacity) { slots_.resize(1); }
    void placeholder(std::shared_ptr<void> owner) { slots_[0].owner = std::move(owner); ++revision_; }
    uint32_t insert(TextureHandle h, std::shared_ptr<void> owner) {
        if (!h.isValid() || !owner) return 0;
        if (auto it = indices_.find(h.raw()); it != indices_.end()) return it->second;
        uint32_t slot;
        if (!free_.empty()) { slot = free_.back(); free_.pop_back(); }
        else { if (slots_.size() >= capacity_) return 0; slot = uint32_t(slots_.size()); slots_.push_back({}); }
        slots_[slot] = {h, std::move(owner)}; indices_[h.raw()] = slot; ++revision_; return slot;
    }
    bool erase(TextureHandle h) {
        auto it = indices_.find(h.raw()); if (it == indices_.end()) return false;
        slots_[it->second] = {}; free_.push_back(it->second); indices_.erase(it); ++revision_; return true;
    }
    uint32_t index(TextureHandle h) const {
        auto it = indices_.find(h.raw()); return it == indices_.end() ? 0 : it->second;
    }
    bool full() const { return free_.empty() && slots_.size() >= capacity_; }
    std::vector<Slot> snapshot() const { return slots_; }
    uint64_t revision() const { return revision_; }
private:
    uint32_t capacity_;
    uint64_t revision_ = 0;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::unordered_map<uint32_t, uint32_t> indices_;
};

// GameOS main owns the session service on its stack, ahead of renderer init and through renderer shutdown.
// This pointer borrows it; no static-duration texture/resource owner runs during process teardown.
inline TextureService* activeTextureService = nullptr;
class TextureServiceScope {
public:
    explicit TextureServiceScope(TextureService& service) { assert(!activeTextureService); activeTextureService = &service; }
    ~TextureServiceScope() { activeTextureService = nullptr; }
    TextureServiceScope(const TextureServiceScope&) = delete;
    TextureServiceScope& operator=(const TextureServiceScope&) = delete;
};
inline TextureService& textureService() {
    assert(activeTextureService);
    return *activeTextureService;
}

// Move-only owner of one already-counted service reference. The session owner outlives all refs.
class ServiceTextureRef {
public:
    ServiceTextureRef() = default;
    static ServiceTextureRef adopt(TextureService& service, TextureHandle handle) { return ServiceTextureRef(service, handle); }
    static ServiceTextureRef retain(TextureService& service, TextureHandle handle) {
        return service.retain(handle) ? ServiceTextureRef(service, handle) : ServiceTextureRef{};
    }
    ~ServiceTextureRef() { if (service_) service_->release(handle_); }
    ServiceTextureRef(const ServiceTextureRef&) = delete;
    ServiceTextureRef& operator=(const ServiceTextureRef&) = delete;
    ServiceTextureRef(ServiceTextureRef&& o) noexcept : service_(std::exchange(o.service_, nullptr)), handle_(o.handle_) {}
    ServiceTextureRef& operator=(ServiceTextureRef&& o) noexcept {
        if (this != &o) { if (service_) service_->release(handle_); service_ = std::exchange(o.service_, nullptr); handle_ = o.handle_; }
        return *this;
    }
    TextureHandle get() const { return service_ ? handle_ : TextureHandle::invalid(); }
private:
    ServiceTextureRef(TextureService& s, TextureHandle h) : service_(&s), handle_(h) {}
    TextureService* service_ = nullptr;
    TextureHandle handle_;
};

}  // namespace RenderCore
