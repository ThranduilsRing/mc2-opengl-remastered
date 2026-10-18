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

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Handle.h"

namespace RenderCore {

constexpr uint32_t kNoTextureNode = 0xFFFFFFFFu;

// What a texture IS. `source` is the resolved source id (see makeTextureSourceId); `variant` separates instances of one source (the manager's uniqueInstance,
// a paint scheme hash); `format` and `hints` are the usage: colour space + alpha policy (the format) and mip policy (the hints).
struct TextureKey {
    std::string source;
    uint32_t    variant = 0;
    uint32_t    format = 0;
    uint32_t    hints = 0;
    bool operator==(const TextureKey& o) const {
        return variant == o.variant && format == o.format && hints == o.hints && source == o.source;
    }
};

struct TextureKeyHash {
    size_t operator()(const TextureKey& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        for (unsigned char c : k.source) mix(c);
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
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        id += c;
    }
    id += '|';
    static const char* kHex = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) id += kHex[(contentHash >> shift) & 0xF];
    return id;
}

class TextureService {
public:
    // The handle of `key`: a new one bound to `node`, or the existing one (its user count rises; `node` is ignored, the first residency stays).
    TextureHandle acquire(const TextureKey& key, uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = byKey_.find(key);
        if (it != byKey_.end()) {
            Entry& e = entries_[it->second];
            ++e.users;
            return TextureHandle::make(it->second, e.generation);
        }
        uint32_t idx;
        if (!free_.empty()) { idx = free_.back(); free_.pop_back(); }
        else { idx = static_cast<uint32_t>(entries_.size()); entries_.emplace_back(); }
        Entry& e = entries_[idx];
        e.live = true;
        e.key = key;
        e.node = node;
        e.users = 1;
        e.contentVersion = 0;
        byKey_.emplace(key, idx);
        if (node != kNoTextureNode) byNode_[node] = idx;
        return TextureHandle::make(idx, e.generation);
    }

    // One user of the node's texture let go (mirrors the manager's reference count). The entry stays until the node dies.
    void dropRef(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = byNode_.find(node);
        if (it != byNode_.end() && entries_[it->second].users > 0) --entries_[it->second].users;
    }

    // The node died: the entry goes, its generation bumps (a held handle is stale), its index is reusable.
    void onNodeDestroyed(uint32_t node) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = byNode_.find(node);
        if (it == byNode_.end()) return;
        retire(it->second);
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
        return it == byNode_.end() ? TextureHandle::invalid() : TextureHandle::make(it->second, entries_[it->second].generation);
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
    }

private:
    struct Entry {
        bool        live = false;
        uint32_t    generation = 1;
        uint32_t    node = kNoTextureNode;
        uint32_t    users = 0;
        uint32_t    contentVersion = 0;
        TextureKey  key;
    };
    const Entry* resolve(TextureHandle h) const {
        if (!h.isValid() || h.index() >= entries_.size()) return nullptr;
        const Entry& e = entries_[h.index()];
        return (e.live && e.generation == h.generation()) ? &e : nullptr;
    }
    Entry* resolve(TextureHandle h) { return const_cast<Entry*>(static_cast<const TextureService*>(this)->resolve(h)); }
    void retire(uint32_t idx) {
        Entry& e = entries_[idx];
        byKey_.erase(e.key);
        e.live = false;
        e.node = kNoTextureNode;
        e.generation = e.generation >= 0xFFFu ? 1u : e.generation + 1u;
        e.key = TextureKey{};
        free_.push_back(idx);
    }

    mutable std::mutex mu_;
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_;
    std::unordered_map<TextureKey, uint32_t, TextureKeyHash> byKey_;
    std::unordered_map<uint32_t, uint32_t> byNode_;
};

// The process's texture service. Main thread for the engine hooks; the lock makes the lookups safe from a job.
inline TextureService& textureService() {
    static TextureService s;
    return s;
}

}  // namespace RenderCore
