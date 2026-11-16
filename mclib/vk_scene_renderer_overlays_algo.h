#pragma once
#include <array>
#include <cstdint>
#include <vector>

// Pure legacy-overlay scheduling. No renderer objects or state are read here.
namespace mc2_overlay_algo {
enum class Pass { Water, NoUnderlayer, Shadow, Alpha, Effects, Spotlight, Compass, Hud };
enum class Op { PassBegin, Submit, ResetVertex, AlphaTest, RenderStateBlock, Flush, PassEnd };
enum class Block { Initial, WaterNode, NoUnderlayer, Shadow, Alpha, AlphaRestore,
                   Effects, Spotlight, Compass, Hud, End };
struct Masks {
    uint32_t terrain, alpha, shadows, effects, compass, spotlight, hud;
    uint32_t alphaTest, water, waterDetail, gpuOverlay;
};
struct Node {
    uint32_t flags;
    bool vertices;
    uint32_t capacity, current;
};
struct Event {
    Op op;
    Pass pass;
    int node = -1;
    uint32_t start = 0, count = 0;
    int value = 0;
    bool operator==(const Event& b) const {
        return op == b.op && pass == b.pass && node == b.node && start == b.start
            && count == b.count && value == b.value;
    }
};

// Access each node once; retain list order independently in all ten buckets
// (the water and non-terrain alpha passes each have two alpha-test buckets).
// Emit synchronously: Submit retains vkLegacySubmitNode's implicit reset, and
// ResetVertex records the separate caller assignment, including reset-only work.
template<class View, class Emit>
void Schedule(int size, bool software, const Masks& m, View view, Emit emit) {
    std::array<std::vector<int>, 10> buckets;
    std::vector<Node> nodes;
    nodes.reserve(size);
    for (int i = 0; i < size; ++i) {
        const Node n = view(i);
        nodes.push_back(n);
        if (!n.vertices) continue;
        const auto f = n.flags;
        const int a = (f & m.alphaTest) ? 1 : 0;
        if ((f & m.terrain) && (f & m.alpha)) buckets[a].push_back(i);
        if ((f & m.terrain) && !(f & m.alpha) && (f & m.gpuOverlay)) buckets[2].push_back(i);
        if (!software && (f & m.shadows) && (f & m.alpha)) buckets[3].push_back(i);
        if (!(f & m.terrain) && !(f & m.shadows) && !(f & m.compass) && (f & m.alpha))
            buckets[4 + a].push_back(i);
        if (f & m.effects) buckets[6].push_back(i);
        if (f & m.spotlight) buckets[7].push_back(i);
        if (f & m.compass) buckets[8].push_back(i);
        if (f & m.hud) buckets[9].push_back(i);
    }
    Pass pass = Pass::Water;
    const auto event = [&](Op op, int value = 0, int node = -1, uint32_t count = 0) {
        emit(Event{op, pass, node, 0, count, value});
    };
    const auto block = [&](Block id, int node = -1) {
        event(Op::RenderStateBlock, static_cast<int>(id), node);
    };
    const auto begin = [&](Pass p) { pass = p; event(Op::PassBegin); };
    const auto end = [&] { event(Op::PassEnd); };
    const auto drain = [&](int bucket, bool submit, int alpha = 0) {
        for (int i : buckets[bucket]) {
            auto& n = nodes[i];
            if (pass == Pass::Water && (n.flags & (m.water | m.waterDetail)))
                block(Block::WaterNode, i);
            if (submit) {
                event(Op::Submit, alpha, i, n.current);
                n.current = 0; // vkLegacySubmitNode resets even an empty range.
            }
            event(Op::ResetVertex, 0, i, n.current);
            n.current = 0;
        }
    };
    block(Block::Initial);
    begin(Pass::Water);
    for (int a = 0; a < 2; ++a) { event(Op::AlphaTest, a); drain(a, false); }
    event(Op::AlphaTest); end();
    begin(Pass::NoUnderlayer); block(Block::NoUnderlayer); drain(2, false); end();
    begin(Pass::Shadow); block(Block::Shadow); drain(3, true); event(Op::Flush); end();
    begin(Pass::Alpha); block(Block::Alpha);
    for (int a = 0; a < 2; ++a) { event(Op::AlphaTest, a); drain(4 + a, true, a); }
    event(Op::AlphaTest); block(Block::AlphaRestore); end();
    begin(Pass::Effects); block(Block::Effects); drain(6, true); end();
    begin(Pass::Spotlight); block(Block::Spotlight); drain(7, true); event(Op::Flush); end();
    begin(Pass::Compass); block(Block::Compass); drain(8, false); end();
    begin(Pass::Hud); block(Block::Hud); drain(9, false); block(Block::End); end();
}
} // namespace mc2_overlay_algo
