#pragma once

#include <cstddef>
#include <unordered_map>

namespace mc2::hud_join {

// Visit in source order and insert only eligible views: the first eligible
// duplicate wins, even if an ineligible view of the object precedes it.
template<class Views, class Mode>
auto barStatusIndex(const Views& views, Mode full, Mode own)
{
    using View = typename Views::value_type;
    std::unordered_map<decltype(View::obj), float> index;
    index.reserve(views.size());
    for (const auto& view : views) {
        if (view.mode == full || view.mode == own) {
            index.try_emplace(view.obj, view.barStatus);
        }
    }
    return index;
}

// Include null keys too: lookup must have exactly the old equality semantics.
template<class Rows>
auto moverIndex(const Rows& rows)
{
    using Row = typename Rows::value_type;
    std::unordered_map<decltype(Row::object), std::size_t> index;
    index.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        index.try_emplace(rows[i].object, i);
    }
    return index;
}

} // namespace mc2::hud_join
