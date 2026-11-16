// Texture identity and source ownership for one engine session.
// Keys name resolved providers and content; handles are generation checked, never
// saved or read by simulation. Compatibility asset numbers index CPU sources;
// Vulkan residency and descriptor indices belong to the backend and may change.
// The session owns the source index and each live entry owns its payload. Raw
// bindings and explicit service references share the count; detaching the final
// raw binding preserves payloads retained by explicit references.
// Pure standard C++; the engine supplies VFS sources and deferred CPU consumers.
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
#include <deque>

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
    return isUiTextureId(id) ? TextureHandle::make(static_cast<uint32_t>(id) & 0xfffffu, static_cast<uint32_t>(id) >> 20) : TextureHandle::invalid();
}

// Source ownership is pure standard C++; the engine supplies VFS payloads and
// drains deferred CPU consumers on the render thread, outside the service mutex.
struct TextureSourcePayload { virtual ~TextureSourcePayload() = default; };
struct TextureSourceOwner {
    virtual ~TextureSourceOwner() = default;
    virtual void collectRetired() = 0;
};

class TextureService {
public:
    void ownSources(std::unique_ptr<TextureSourceOwner> owner) {
        assert(!sourceOwner_); sourceOwner_ = std::move(owner);
    }
    TextureSourceOwner* sourceOwner() const noexcept { return sourceOwner_.get(); }
    // Never called under mu_: draining a CPU consumer may release a service reference.
    void collectSourceRetirements() { if (sourceOwner_) sourceOwner_->collectRetired(); }
    void attachSource(TextureHandle h, std::shared_ptr<TextureSourcePayload> payload) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto* e = resolve(h)) e->source = std::move(payload);
    }
    std::shared_ptr<TextureSourcePayload> source(TextureHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        const auto* e = resolve(h); return e ? e->source : nullptr;
    }
    // Restore a raw asset binding only while this exact generation is still alive.
    bool retainSource(TextureHandle h, uint32_t asset) {
        std::lock_guard<std::mutex> lk(mu_);
        auto* e = resolve(h);
        if (!e || e->users == UINT32_MAX || asset == kNoTextureNode) return false;
        auto it = byNode_.find(asset);
        if (it != byNode_.end()) {
            const auto& [boundNode, bound] = *it;
            if (bound.index != h.index()) return false;
        }
        bindNode(h.index(), asset, 1);
        ++e->users;
        if (e->node == kNoTextureNode) e->node = asset;
        return true;
    }
    // Render-thread material identity shares the texture service session lifetime.
    MaterialRegistry& materials() { return materials_; }
    std::function<void(TextureHandle)> onRetired; // context residency borrows the session; callback only queues invalidations
    // The handle of `key`: a new one bound to `node`, or the existing one (its user count rises; every alias node is indexed, the first residency stays).
    TextureHandle acquire(const TextureKey& key, uint32_t node = kNoTextureNode, TextureDimensions dimensions = {}) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto it = byKey_.find(key); it != byKey_.end()) {
            const auto& [k, idx] = *it;
            Entry& e = entries_[idx];
            if (e.users == UINT32_MAX) return TextureHandle::invalid();
            ++e.users;
            if (node != kNoTextureNode) {
                bindNode(idx, node, 1);
                if (e.node==kNoTextureNode) e.node=node;
            }
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
        if (node != kNoTextureNode) bindNode(idx, node, 1);
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
        auto it = cooked_.find(key);
        if (it == cooked_.end()) return nullptr;
        const auto& [cookedKey, image] = *it;
        return image;
    }
    void cacheCooked(const TextureKey& key, std::shared_ptr<const KtxImage> image) {
        std::lock_guard<std::mutex> lk(mu_); cooked_[key] = std::move(image);
    }
    // Mission-boundary eviction; live handles and borrowed decode results stay valid.
    void clearCookedCache() {
        std::lock_guard<std::mutex> lk(mu_); cooked_.clear();
    }
    bool retainNode(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it=byNode_.find(node);
        if (it==byNode_.end()) return false;
        auto& [boundNode, binding]=*it; auto& e=entries_[binding.index];
        if (e.users==UINT32_MAX) return false;
        ++e.users; ++binding.users; return true;
    }
    // One user of the node's texture let go (mirrors the manager's reference count). The entry stays until the node dies.
    void dropRef(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto it = byNode_.find(node); it != byNode_.end()) {
            auto& [boundNode, binding]=*it;
            if (binding.users) { --binding.users; if (entries_[binding.index].users) --entries_[binding.index].users; }
        }
    }

    // Detach a raw asset binding. Explicit service references keep its payload alive.
    void onNodeDestroyed(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = byNode_.find(node);
        if (it == byNode_.end()) return;
        const auto [boundNode, binding]=*it;
        Entry& e=entries_[binding.index];
        e.users=e.users>binding.users ? e.users-binding.users : 0;
        unbindNode(node);
        if (!e.aliases.empty()) return; // another manager node still owns this identity
        if (e.users == 0) retire(binding.index);
    }

    // The same texture now lives in `newNode` (new pixels): the handle stays, the content version rises.
    bool rebind(TextureHandle h, uint32_t newNode) {
        std::lock_guard<std::mutex> lk(mu_);
        Entry* e = resolve(h);
        if (!e) return false;
        uint32_t nodeUsers=0;
        if (auto it=byNode_.find(e->node); it!=byNode_.end()) { nodeUsers=it->second.users; unbindNode(e->node); }
        e->node = newNode;
        ++e->contentVersion;
        if (newNode != kNoTextureNode) bindNode(h.index(), newNode, nodeUsers);
        else if (!e->aliases.empty()) e->node=e->aliases.front();
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
        const auto& [boundNode, binding]=*it;
        const uint32_t idx=binding.index;
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
    // Session teardown: force every entry stale, including entries with explicit refs.
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
        std::shared_ptr<TextureSourcePayload> source;
        std::vector<uint32_t> aliases;
    };
    // Each binding indexes its slot in the entry's dense alias list. Swap removal
    // makes mission teardown linear in the destroyed nodes, without a global scan.
    void bindNode(uint32_t index, uint32_t node, uint32_t users) {
        auto [it, inserted] = byNode_.try_emplace(node);
        auto& binding = it->second;
        if (inserted) {
            binding.index = index;
            binding.slot = static_cast<uint32_t>(entries_[index].aliases.size());
            entries_[index].aliases.push_back(node);
        }
        binding.users += users;
    }
    void unbindNode(uint32_t node) {
        auto it = byNode_.find(node);
        const auto binding = it->second;
        auto& e = entries_[binding.index];
        const uint32_t moved = e.aliases.back();
        e.aliases[binding.slot] = moved;
        byNode_.at(moved).slot = binding.slot;
        e.aliases.pop_back();
        byNode_.erase(it);
        if (e.node == node) e.node = e.aliases.empty() ? kNoTextureNode : e.aliases.front();
    }
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
        e.aliases.clear();
        // Quarantine exhausted slots: wrapping would revive an old CPU handle.
        if (e.generation < 0xFFFu) { ++e.generation; free_.push_back(idx); }
        e.source.reset();   // payload teardown only queues consumer cleanup
        e.key = TextureKey{};
    }

    std::unordered_map<TextureKey, std::shared_ptr<const KtxImage>, TextureKeyHash> cooked_;
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_;
    std::unordered_map<TextureKey, uint32_t, TextureKeyHash> byKey_;
    struct NodeBinding { uint32_t index=0, users=0, slot=0; };
    std::unordered_map<uint32_t, NodeBinding> byNode_;
    std::unique_ptr<TextureSourceOwner> sourceOwner_;
};

struct TexSlot { uint16_t slot = 0, gen = 0; };

// Slot reuse waits for every flight to observe the tombstone. Snapshots also retain
// image owners until submission completes; generations reject cached CPU indices.
class TextureDescriptorTable {
public:
    struct Slot { TextureHandle handle; std::shared_ptr<void> owner; uint16_t gen = 1; };
    struct Change { uint32_t slot; uint64_t revision; };
    explicit TextureDescriptorTable(uint32_t capacity, uint32_t flights = 3)
        : capacity_(capacity), flights_(flights) { assert(capacity > 0 && capacity <= 65536); slots_.resize(1); }
    void beginFrame(uint64_t frame) {
        assert(frame >= frame_); frame_ = frame;
        while (!quarantine_.empty() && frame_ - quarantine_.front().frame >= flights_) {
            free_.push_back(quarantine_.front().slot); quarantine_.pop_front();
        }
    }
    void changed(uint32_t slot) {
        journal_.push_back({slot, ++revision_});
        if (journal_.size() > capacity_) journal_.pop_front();
    }
    // A lagging flight falls back to a full snapshot after bounded journal overflow.
    std::vector<Change> changesSince(uint64_t applied, bool& full) const {
        full = !journal_.empty() && applied < journal_.front().revision - 1;
        std::vector<Change> result;
        if (applied == revision_) return result;
        if (!full) for (const auto& change : journal_) if (change.revision > applied) result.push_back(change);
        return result;
    }
    void placeholder(std::shared_ptr<void> owner) { slots_[0].owner = std::move(owner); changed(0); }
    uint32_t reserve(TextureHandle h) { const auto i = index(h); return i ? i : insert(h, {}); }
    uint32_t insert(TextureHandle h, std::shared_ptr<void> owner) {
        if (!h.isValid()) return 0;
        if (auto it = indices_.find(h.raw()); it != indices_.end()) {
            const auto& [raw, existing] = *it;
            if (slots_[existing].owner != owner && slots_[existing].gen < UINT16_MAX) ++slots_[existing].gen;
            slots_[existing].owner = std::move(owner); changed(existing);
            return existing;
        }
        uint32_t slot;
        if (!free_.empty()) { slot = free_.back(); free_.pop_back(); }
        else { if (slots_.size() >= capacity_) return 0; slot = static_cast<uint32_t>(slots_.size()); slots_.push_back({}); }
        slots_[slot].handle = h; slots_[slot].owner = std::move(owner);
        indices_[h.raw()] = slot; changed(slot); return slot;
    }
    bool erase(TextureHandle h) {
        auto it = indices_.find(h.raw()); if (it == indices_.end()) return false;
        const auto [raw, slot] = *it;
        auto& entry = slots_[slot]; entry.handle = {}; entry.owner.reset();
        // Never wrap: a very old CPU token must not become valid again.
        if (entry.gen < UINT16_MAX) { ++entry.gen; quarantine_.push_back({slot, frame_}); }
        indices_.erase(it); changed(slot); return true;
    }
    void invalidate(TextureHandle h) {
        if (const auto i = index(h)) { slots_[i].owner.reset(); changed(i); }
    }
    uint32_t index(TextureHandle h) const {
        auto it = indices_.find(h.raw());
        if (it == indices_.end()) return 0;
        const auto& [raw, slot] = *it;
        return slot;
    }
    TexSlot token(TextureHandle h) const {
        const auto i = index(h); return {static_cast<uint16_t>(i), slots_[i].gen};
    }
    uint32_t resolve(TexSlot h) const {
        return h.slot < slots_.size() && slots_[h.slot].gen == h.gen &&
            (h.slot == 0 || slots_[h.slot].handle.isValid()) ? h.slot : 0;
    }
    const Slot& slot(uint32_t i) const { return slots_[i < slots_.size() ? i : 0]; }
    size_t availableSlots() const { return free_.size() + capacity_ - slots_.size(); }
    bool full(size_t reserved = 0) const { return reserved >= availableSlots(); }
    std::vector<Slot> snapshot() const { return slots_; }
    uint64_t revision() const { return revision_; }
private:
    uint32_t capacity_;
    uint64_t revision_ = 0;
    uint64_t frame_ = 0;
    uint32_t flights_;
    struct Retired { uint32_t slot; uint64_t frame; };
    std::deque<Retired> quarantine_;
    std::deque<Change> journal_;
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
