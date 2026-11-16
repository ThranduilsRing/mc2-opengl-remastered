#pragma once
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace mc2vfx {
// Scoped to one run/parity, after get_page has finished mutating the page map.
template<class Pages>
class PageOwners {
    using Page = typename Pages::mapped_type;
    using Set = std::decay_t<decltype(std::declval<Page>().set[0])>;
    std::unordered_map<Set, const Page*> owners_;
public:
    PageOwners(const Pages& pages, unsigned parity) {
        owners_.reserve(pages.size());
        for (const auto& [key, page] : pages) {
            // Keep the first owner in map iteration order, exactly as find_if.
            owners_.try_emplace(page.set[parity], &page);
        }
    }
    const Page* find(Set set) const {
        const auto owner = owners_.find(set);
        if (owner == owners_.end()) return nullptr;
        const auto& [key, page] = *owner;
        return page;
    }
};
}
