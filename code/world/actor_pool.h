#pragma once
#include "../tick_commit.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

namespace mc2::world {
template<class T> struct ActorId {
    uint64_t session = 0;
    uint32_t index = 0;
    uint64_t generation = 0;
    explicit operator bool() const { return session && generation; }
    bool operator==(const ActorId&) const = default;
};

// Session-owned typed pool: the single owner and construction path of one actor
// family. Index order is the legacy family array order (save-slot order), so
// spawn fills the lowest free index. IDs carry the session and a per-index
// generation; reset and retire make every earlier ID stale. Retired actors go
// to the render-packet graveyard (mc2::tick::retireObject), so an actor an
// in-flight frame still references is freed only after that frame retires.
template<class T> class ActorPool {
    inline static std::atomic<uint64_t> nextSession_{1};
    uint64_t session_ = 0;
    std::vector<T*> slots_; // owned; data() is the legacy family array view
    std::vector<uint64_t> generations_;
    std::size_t firstFree_ = 0;
    // Bound where spawn instantiates with T complete, so headers that only
    // destroy an empty pool never need T's destructor.
    void (*retire_)(T*) = nullptr;
    static void retireActor(T* actor) { mc2::tick::retireObject(actor); }

public:
    ActorPool() = default;
    ActorPool(const ActorPool&) = delete;
    ActorPool& operator=(const ActorPool&) = delete;
    ~ActorPool() { clear(); }

    // Starts a new session of `capacity` empty indices; live actors are retired.
    T** reset(std::size_t capacity) {
        clear();
        session_ = nextSession_.fetch_add(1, std::memory_order_relaxed);
        if (!session_) std::terminate();
        slots_.assign(capacity, nullptr);
        generations_.assign(capacity, 1);
        return data();
    }
    void clear() {
        for (T*& actor : slots_)
            if (actor) retire_(std::exchange(actor, nullptr));
        slots_.clear();
        generations_.clear();
        firstFree_ = 0;
        session_ = 0;
    }
    template<class... Args> ActorId<T> spawn(Args&&... args) {
        retire_ = &retireActor;
        while (firstFree_ < slots_.size() && slots_[firstFree_]) ++firstFree_;
        if (firstFree_ >= slots_.size()) return {};
        const auto index = firstFree_++;
        slots_[index] = new T(static_cast<Args&&>(args)...);
        return id(static_cast<uint32_t>(index));
    }
    bool retire(ActorId<T> actorId) {
        if (!resolve(actorId)) return false;
        T* actor = std::exchange(slots_[actorId.index], nullptr);
        if (++generations_[actorId.index] == 0) std::terminate();
        firstFree_ = std::min<std::size_t>(firstFree_, actorId.index);
        retire_(actor);
        return true;
    }
    T* get(std::size_t index) const { return index < slots_.size() ? slots_[index] : nullptr; }
    ActorId<T> id(uint32_t index) const {
        return get(index) ? ActorId<T>{session_, index, generations_[index]} : ActorId<T>{};
    }
    T* resolve(ActorId<T> actorId) const {
        return actorId.session == session_ && get(actorId.index) &&
            actorId.generation == generations_[actorId.index] ? slots_[actorId.index] : nullptr;
    }
    T** data() { return slots_.data(); }
    std::size_t capacity() const { return slots_.size(); }
};
}
