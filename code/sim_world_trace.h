// Read-only ordered-world witness. No allocation or object reads with the gate off.
#pragma once
#include "Gates/gate.h"

class GameObject;
class GameObjectManager;

namespace mc2::world_trace {
inline bool enabled() {
    static const bool value = mc2::gate::get<"MC2_WORLD_ORDER_TRACE">();
    return value;
}
void begin(long tick);
void end();
void snapshot(GameObjectManager& world, const char* phase);
void update(const char* view, GameObject* object);
void event(const char* action, GameObject* object, long detail = 0);

struct TickScope {
    bool active;
    explicit TickScope(long tick) : active(enabled()) { if (active) begin(tick); }
    ~TickScope() { if (active) end(); }
    TickScope(const TickScope&) = delete;
    TickScope& operator=(const TickScope&) = delete;
};
}

// Keep argument evaluation and all diagnostic object reads behind the gate.
#define MC2_WORLD_UPDATE(view, object) do { if (mc2::world_trace::enabled()) mc2::world_trace::update(view, object); } while (false)
#define MC2_WORLD_EVENT(action, object, detail) do { if (mc2::world_trace::enabled()) mc2::world_trace::event(action, object, detail); } while (false)
