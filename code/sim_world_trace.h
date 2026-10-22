// Read-only ordered-world witness. No allocation or object reads with the gate off.
#pragma once
#include "Gates/gate.h"
#include <string_view>

class GameObject;
class GameObjectManager;

namespace mc2::world_trace {
inline bool enabled() {
    static const bool value = mc2::gate::get<"MC2_WORLD_ORDER_TRACE">();
    return value;
}
void begin(long tick);
void end();
void snapshot(GameObjectManager& world, std::string_view phase);
void update(std::string_view view, GameObject* object);
// Header-only object facades also serve standalone tests. A tick-local sink
// keeps their existence setter independent of the engine recorder at link time.
namespace detail {
using EventSink = void (*)(std::string_view, GameObject*, long);
inline EventSink eventSink = nullptr;
}
inline void event(std::string_view action, GameObject* object, long value = 0) {
    if (detail::eventSink) detail::eventSink(action, object, value);
}

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
