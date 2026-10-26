#include "doctest.h"
#include "tgl_registry.h"
#include <memory>
#include <vector>

namespace {
struct ShapeOwner {
    TglIdentity<ShapeOwner> identity{this};
    int payload = 7;
};
}

TEST_CASE("TGL registry: IDs survive directory and owner-vector growth") {
    auto& registry = TglRegistry<ShapeOwner>::get();
    std::vector<std::unique_ptr<ShapeOwner>> owners;
    owners.push_back(std::make_unique<ShapeOwner>());
    const auto id = owners.front()->identity.id();
    auto* pointer = owners.front().get();
    for (int i = 0; i != 4096; ++i) owners.push_back(std::make_unique<ShapeOwner>());
    CHECK(registry.resolve(id) == pointer);
    CHECK(registry.resolve(id)->payload == 7);
    CHECK(registry.resolve({}) == nullptr);
}

TEST_CASE("TGL registry: destroy and slot reuse reject stale IDs") {
    auto& registry = TglRegistry<ShapeOwner>::get();
    auto owner = std::make_unique<ShapeOwner>();
    const auto stale = owner->identity.id();
    owner.reset();
    CHECK(registry.resolve(stale) == nullptr);
    owner = std::make_unique<ShapeOwner>();
    CHECK(owner->identity.id() != stale);
    CHECK(registry.resolve(stale) == nullptr);
    CHECK(registry.resolve(owner->identity.id()) == owner.get());
    registry.erase(stale); // repeated retirement cannot evict the replacement
    CHECK(registry.resolve(owner->identity.id()) == owner.get());
}

TEST_CASE("TGL registry: payload reload at the same address invalidates borrows") {
    ShapeOwner owner;
    const auto stale = owner.identity.id();
    owner.identity.renew(&owner);
    CHECK(TglRegistry<ShapeOwner>::get().resolve(stale) == nullptr);
    CHECK(TglRegistry<ShapeOwner>::get().resolve(owner.identity.id()) == &owner);
}
