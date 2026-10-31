// Tick-owned commands; main commits only after joining the worker.
#pragma once
#include <functional>
#include <memory>
#include <utility>
#include <cstdlib>
#include <cstdint>

namespace mc2::tick {
enum class Queue { MoverUi, Release, Impact, Sound, Graveyard, Count };
inline thread_local bool collectingTick = false;
inline bool collecting() { return collectingTick; }
inline void setCollecting(bool active) { collectingTick = active; }
bool defer(Queue queue, std::function<void()> command);
bool deferRetirement(Queue queue, std::function<void()> command);
void commitTickQueues(); // textures, UI, releases, impacts, sound, then storage
// Main owns packet admission/retirement and destruction. Record completion must
// be reported back to Main; GPU resources still use their existing ticket queues.
// These leases protect lifetime only, not mutable RenderView/HUD contents.
uint64_t retainRenderPacket();
void retireRenderPacket(uint64_t sequence);
struct Scope {
    bool previous = collecting();
    ~Scope() { setCollecting(previous); }
};

template<class Effect> void releaseEffect(std::unique_ptr<Effect>& effect) {
    if (!effect) return;
    auto* old = effect.release();
    auto release = [old] { std::unique_ptr<Effect> owner(old); owner->Kill(); };
    if (!deferRetirement(Queue::Release, release)) release();
}
template<class Light, class WorldLights, class Id>
void releaseLight(Light*& light, WorldLights* lights, Id id) {
    if (!light) return;
    auto* old = std::exchange(light, nullptr);
    auto release = [old, lights, id] {
        if (lights) lights->removeWorldLight(id, old);
        std::free(old);
    };
    if (!deferRetirement(Queue::Release, release)) release();
}

// MC2_DESTROY only marks absence. Pooled objects keep their storage on death.
// Actual delete sites use this helper so the destructor and storage both wait
// until commit; never defer only operator delete after a destructor has run.
template<class Object> void retireObject(Object* object) {
    auto release = [object] { std::unique_ptr<Object> owner(object); };
    if (!deferRetirement(Queue::Graveyard, release)) release();
}
} // namespace mc2::tick
