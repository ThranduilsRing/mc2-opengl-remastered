#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

namespace mc2::world {
struct EntityId {
    uint64_t session = 0;
    uint32_t slot = 0;
    uint64_t generation = 0;
    explicit operator bool() const { return session && slot && generation; }
    bool operator==(const EntityId&) const = default;
};

// Slot order is the legacy handle/save order. Pointers are borrowed; the object
// manager still owns actors. Like X1, reset and reuse must reject stale IDs.
template<class Base> class EntityRegistry {
    inline static std::atomic<uint64_t> nextSession_{1};
    uint64_t session_ = 0;
    std::vector<Base*> slots_;
    std::vector<uint64_t> generations_;
public:
    EntityRegistry() = default;
    EntityRegistry(const EntityRegistry&) = delete;
    EntityRegistry& operator=(const EntityRegistry&) = delete;
    Base** reset(std::size_t count) {
        session_ = nextSession_.fetch_add(1, std::memory_order_relaxed);
        if (!session_) std::terminate();
        slots_.assign(count, nullptr);
        generations_.assign(count, 1);
        return slots_.data();
    }
    void clear() { reset(0); }
    Base* get(uint32_t slot) const {
        return slot && slot < slots_.size() ? slots_[slot] : nullptr;
    }
    EntityId id(uint32_t slot) const {
        return get(slot) ? EntityId{session_, slot, generations_[slot]} : EntityId{};
    }
    Base* resolve(EntityId id) const {
        return id.session == session_ && get(id.slot) &&
            id.generation == generations_[id.slot] ? slots_[id.slot] : nullptr;
    }
    template<class T> T* resolve(EntityId id) const { return dynamic_cast<T*>(resolve(id)); }
    void invalidate(uint32_t slot, const Base* object) {
        if (get(slot) != object || !object) return;
        if (++generations_[slot] == 0) std::terminate();
    }
};
}
