#include "doctest.h"
#include "anim_binding.h"
#include "platform_str.h"
#include <vector>

namespace {
using Names = std::vector<const char*>;
std::vector<long> linear(const Names& tracks, const Names& nodes)
{
    std::vector<long> result(tracks.size(), -1);
    for (std::size_t t = 0; t < tracks.size(); ++t)
        for (std::size_t n = 0; n < nodes.size(); ++n)
            if (S_stricmp(tracks[t], nodes[n]) == 0) { result[t] = static_cast<long>(n); break; }
    return result;
}
}

TEST_CASE("anim_binding_cache/reuse and destination node order")
{
    const Names tracks{"TORSO", "arm", "missing", "torso"};
    const Names body{"torso", "ARM", "torso"};
    const Names shadow{"arm", "torso"};
    AnimBindingCache bodyCache, shadowCache;
    const auto clip = nextAnimationBindingId();
    int scans = 0;
    auto bind = [&](AnimBindingCache& cache, const Names& nodes) -> const std::vector<long>& {
        return cache.bind(clip, static_cast<long>(tracks.size()), static_cast<long>(nodes.size()),
            [&](long t, long n) { ++scans; return S_stricmp(tracks[t], nodes[n]) == 0; });
    };
    const auto first = bind(bodyCache, body);
    CHECK(first == linear(tracks, body));
    CHECK(first == std::vector<long>{0, 1, -1, 0});
    const int firstScans = scans;
    CHECK(bind(bodyCache, body) == first);
    CHECK(scans == firstScans); // A cache that always rebinds fails this oracle.
    CHECK(bind(shadowCache, shadow) == linear(tracks, shadow));
    CHECK(bind(shadowCache, shadow) != first);
    CHECK(bind(bodyCache, body) == first);
    CHECK(&bind(bodyCache, body) != &bind(shadowCache, shadow));
}

TEST_CASE("anim_binding_cache/invalidation and reload")
{
    const Names tracks{"torso", "arm", "missing"};
    Names nodes{"torso", "arm"};
    AnimBindingCache cache;
    auto clip = nextAnimationBindingId();
    int scans = 0;
    auto bind = [&]() -> const std::vector<long>& {
        return cache.bind(clip, static_cast<long>(tracks.size()), static_cast<long>(nodes.size()),
            [&](long t, long n) { ++scans; return S_stricmp(tracks[t], nodes[n]) == 0; });
    };
    CHECK(bind() == linear(tracks, nodes));
    for (const auto* reason : {"ClearAnimation", "shape replacement", "StopUsing", "imported nodes"}) {
        CAPTURE(reason);
        const auto revision = cache.revision();
        const int before = scans;
        cache.invalidate(); // The destination's lifecycle hooks call this operation.
        std::swap(nodes[0], nodes[1]); // Same count, different topology.
        CHECK(cache.revision() != revision);
        CHECK(bind() == linear(tracks, nodes));
        CHECK(scans > before);
    }
    const int beforeReload = scans;
    clip = nextAnimationBindingId(); // Reload/reuse at the same source address.
    CHECK(bind() == linear(tracks, nodes));
    CHECK(scans > beforeReload);
    nodes.clear();
    CHECK(bind() == std::vector<long>{-1, -1, -1});
}
