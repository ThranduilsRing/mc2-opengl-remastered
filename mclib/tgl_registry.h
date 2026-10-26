// TGL P0 identity directory. Existing appearance/actor owners remain authoritative
// until P1/P3. IDs are process-local borrows, never serialized asset identifiers.
#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <vector>

template<class T> struct TglId {
    uint64_t value = 0;
    explicit operator bool() const { return value != 0; }
    bool operator==(const TglId&) const = default;
};

template<class T> class TglRegistry {
    struct Entry { T* pointer = nullptr; uint32_t generation = 1; uint32_t next = 0; };
    std::vector<Entry> entries_;
    uint32_t free_ = 0; // one-based slot; zero is the invalid ID
    TglRegistry() = default;
public:
    TglRegistry(const TglRegistry&) = delete;
    TglRegistry& operator=(const TglRegistry&) = delete;

    static TglRegistry& get() { static TglRegistry registry; return registry; }

    TglId<T> insert(T* pointer) {
        if (!pointer) return {};
        uint32_t slot = free_;
        if (slot) free_ = entries_[slot - 1].next;
        else {
            if (entries_.size() == std::numeric_limits<uint32_t>::max()) std::terminate();
            entries_.emplace_back();
            slot = static_cast<uint32_t>(entries_.size());
        }
        auto& entry = entries_[slot - 1];
        entry.pointer = pointer;
        return {(uint64_t(entry.generation) << 32) | slot};
    }

    T* resolve(TglId<T> id) const {
        const uint32_t slot = static_cast<uint32_t>(id.value);
        if (!slot || slot > entries_.size()) return nullptr;
        const auto& entry = entries_[slot - 1];
        return entry.generation == (id.value >> 32) ? entry.pointer : nullptr;
    }

    void erase(TglId<T> id) {
        if (!resolve(id)) return;
        const uint32_t slot = static_cast<uint32_t>(id.value);
        auto& entry = entries_[slot - 1];
        entry.pointer = nullptr;
        // Retire a wrapped slot permanently, so an old ID can never revive.
        if (++entry.generation) { entry.next = free_; free_ = slot; }
    }
};

// Nonmovable like the shapes themselves: moving an owner would invalidate its
// raw views. Reset before destroying/reloading payload, not just on delete.
template<class T> class TglIdentity {
    TglId<T> id_;
public:
    explicit TglIdentity(T* pointer) : id_(TglRegistry<T>::get().insert(pointer)) {}
    ~TglIdentity() { TglRegistry<T>::get().erase(id_); }
    TglIdentity(const TglIdentity&) = delete;
    TglIdentity& operator=(const TglIdentity&) = delete;
    TglId<T> id() const { return id_; }
    void renew(T* pointer) {
        TglRegistry<T>::get().erase(id_);
        id_ = TglRegistry<T>::get().insert(pointer);
    }
};

class TG_TypeMultiShape;
class TG_TypeNode;
class TG_MultiShape;
using TG_TypeId = TglId<TG_TypeMultiShape>;
// Authored index is independent of the directory's storage. The node token
// supplies a generation even for standalone tool/helper nodes without an owner.
struct TG_TypeNodeId {
    TG_TypeId type;
    uint32_t index = 0;
    uint64_t value = 0;
    explicit operator bool() const { return value != 0; }
    bool operator==(const TG_TypeNodeId&) const = default;
};
using TG_InstanceId = TglId<TG_MultiShape>;
using TglTypeRegistry = TglRegistry<TG_TypeMultiShape>;
using TglNodeRegistry = TglRegistry<TG_TypeNode>;
using TglInstanceRegistry = TglRegistry<TG_MultiShape>;

template<class T> struct std::hash<TglId<T>> {
    size_t operator()(TglId<T> id) const noexcept { return std::hash<uint64_t>{}(id.value); }
};

template<> struct std::hash<TG_TypeNodeId> {
    size_t operator()(TG_TypeNodeId id) const noexcept { return std::hash<uint64_t>{}(id.value); }
};
