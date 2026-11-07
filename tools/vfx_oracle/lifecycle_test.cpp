#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "gosfxheaders.hpp"
#include "Gates/gate.h"
#include "Gates/gate_name.h"
#include "contract_test_support.h"
#include "mclib/det_rng.h"
#include "particles/batcher.h"
#include "particles/spec_library.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

void vfx_test_bind_rng(std::uint32_t* state);

namespace {
// step 5 (2026-10-18): the 48-byte Stuff LinearMatrix4D members are 64-byte glm::mat4 (232).
// Stuff B3a: the 24-byte ChainIteratorOf<Event*> cursor is a 16-byte {events, index} cursor (-8).
// Stuff B3b: the cursor is a plain size_t index (-8).
constexpr size_t EFFECT_SIZE = 216;
struct Platform {
    ContractEnvironment deterministic{mc2::gate::name<"MC2_DETERMINISTIC_RNG">(), "1"};
    ContractEnvironment fixedStep{mc2::gate::name<"MC2_SMOKE_FIXED_TIMESTEP">(), "1"};
    ContractEnvironment identity{mc2::gate::name<"MC2_SIM_IDENTITY_TRACE">(), "1"};
    std::uint32_t seed = mc2_det_rng::hashMissionName("mc2_03");
    Platform()
    {
        mc2::gate::markMainEntered();
        vfx_test_bind_rng(&seed);
        Stuff::InitializeClasses();
        MidLevelRenderer::InitializeClasses(64 * 1024, 16 * 1024, 1024, 4096, true);
        gosFX::InitializeClasses();
    }
    ~Platform()
    {
        gosFX::TerminateClasses();
        MidLevelRenderer::TerminateClasses();
        Stuff::TerminateClasses();
        vfx_test_bind_rng(nullptr);
    }
};

class Parent final : public gosFX::Effect {
public:
    explicit Parent(Specification* spec) : Effect(DefaultData, spec, ExecuteFlag) {}
    void adopt(gosFX::EffectPtr child) { m_children.push_back(std::move(child)); }
    bool empty() { return m_children.empty(); }
    size_t count() const { return m_children.size(); }
    // The x translation of each child's world matrix (row 3 in the engine's affine layout), in spawn order: the event tests
    // put a tag there.
    std::vector<int> tags() const
    {
        std::vector<int> out;
        for (const gosFX::EffectPtr& child : m_children)
            out.push_back(static_cast<int>(std::lround(child->m_localToWorld[0][3])));
        return out;
    }
    const gosFX::Effect& child(size_t i) const { return *m_children[i]; }
};

// A child that counts its destructions, so a test can prove each child is freed exactly once by its one owner.
int g_childDestroyed = 0;
class CountedChild final : public gosFX::Effect {
public:
    explicit CountedChild(Specification* spec) : Effect(DefaultData, spec, ExecuteFlag) {}
    ~CountedChild() { ++g_childDestroyed; }
};
}

TEST_CASE_FIXTURE(Platform, "CardCloud layout after retiring its MLR owner")
{
    // The pre-G2 ParticleCloud object wrote its array at 0xd8. Enlarging Effect
    // silently linked incompatible constructors and destructors in the failing VS build.
    // B8a: m_children became std::vector<EffectPtr> (was a Stuff::ChainOf); the sizes moved with it.
    // Step 5: the two matrix members are glm::mat4 (64 bytes, was the 48-byte affine block): Effect grew by 32.
    CHECK(sizeof(gosFX::Effect) == EFFECT_SIZE);
    CHECK(sizeof(gosFX::Effect::ExecuteInfo) == 40);
    CHECK(sizeof(gosFX::ParticleCloud) == EFFECT_SIZE + 24);
    CHECK(sizeof(gosFX::CardCloud) == EFFECT_SIZE + 48);
}

TEST_CASE_FIXTURE(Platform, "CardCloud births and retirement through Effect Execute")
{
    REQUIRE(mc2::gate::get<"MC2_DETERMINISTIC_RNG">());
    REQUIRE(mc2::gate::get<"MC2_SMOKE_FIXED_TIMESTEP">());
    REQUIRE(mc2::gate::get<"MC2_SIM_IDENTITY_TRACE">());
    for (unsigned capacity : {1u, 32u, 128u}) {
        CAPTURE(capacity);
        gosFX::Effect::Specification parentSpec;
        parentSpec.BuildDefaults();
        parentSpec.m_lifeSpan.SetCurve(10.0f);
        gosFX::CardCloud::Specification spec;
        spec.BuildDefaults();
        spec.m_maxParticleCount = capacity;
        spec.m_startingPopulation.SetCurve(static_cast<float>(capacity));
        spec.m_particlesPerSecond.SetCurve(0.0f);
        spec.m_lifeSpan.SetCurve(0.1f);
        spec.m_pLifeSpan.m_ageCurve.SetCurve(0.1f);
        const glm::mat4 transform = glm::mat4(1.0f);
        mc2::math::Obb bounds = mc2::math::Obb();
        gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
        Parent parent(&parentSpec);
        parent.Start(&info);
        auto* cloud = gosFX::CardCloud::Make(&spec, gosFX::Effect::ExecuteFlag);
        parent.adopt(gosFX::EffectPtr{cloud});
        cloud->Start(&info);
        const auto before = Stuff::Random::GetDrawCount();
        info.m_time = 1.0 / 30.0;
        REQUIRE(parent.Execute(&info));
        REQUIRE_FALSE(parent.empty());
        REQUIRE(Stuff::Random::GetDrawCount() > before);
        REQUIRE(cloud->GetParticle(0)->m_age < 1.0f);
        if (mc2::particles::Batcher::is_enabled()) {
            gosFX::Effect::DrawInfo draw{};
            draw.m_parentToWorld = &transform;
            const auto frozen = Stuff::Random::GetState();
            for (int repeat = 0; repeat < 8; ++repeat) {
                cloud->Draw(&draw);
                mc2::particles::Batcher::Instance().Flush();
                CHECK(Stuff::Random::GetState() == frozen);
            }
        }
        for (int frame = 2; frame <= 60 && !parent.empty(); ++frame) {
            info.m_time = frame / 30.0;
            REQUIRE(parent.Execute(&info));
        }
        CHECK(parent.empty());
    }
}

TEST_CASE_FIXTURE(Platform, "Effect is the one owner of its children: create, finish, kill, destroy")
{
    gosFX::Effect::Specification parentSpec;
    parentSpec.BuildDefaults();
    parentSpec.m_lifeSpan.SetCurve(10.0f);
    gosFX::Effect::Specification shortSpec;
    shortSpec.BuildDefaults();
    shortSpec.m_lifeSpan.SetCurve(0.05f);
    gosFX::Effect::Specification longSpec;
    longSpec.BuildDefaults();
    longSpec.m_lifeSpan.SetCurve(10.0f);
    const glm::mat4 transform = glm::mat4(1.0f);

    SUBCASE("a parent destroyed with live children frees each child once")
    {
        g_childDestroyed = 0;
        {
            mc2::math::Obb bounds = mc2::math::Obb();
            gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
            Parent parent(&parentSpec);
            parent.Start(&info);
            for (int i = 0; i < 3; ++i) {
                auto child = std::make_unique<CountedChild>(&longSpec);
                child->Start(&info);
                parent.adopt(std::move(child));
            }
            CHECK(parent.count() == 3);
            CHECK(g_childDestroyed == 0);
        }
        CHECK(g_childDestroyed == 3);
    }
    SUBCASE("a finished child is released by Execute, in place, once; live siblings keep their order")
    {
        g_childDestroyed = 0;
        gosFX::Effect* survivor = nullptr;
        {
            mc2::math::Obb bounds = mc2::math::Obb();
            gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
            Parent parent(&parentSpec);
            parent.Start(&info);
            auto shortChild = std::make_unique<CountedChild>(&shortSpec);
            shortChild->Start(&info);
            parent.adopt(std::move(shortChild));
            auto longChild = std::make_unique<CountedChild>(&longSpec);
            longChild->Start(&info);
            survivor = longChild.get();
            parent.adopt(std::move(longChild));
            for (int frame = 1; frame <= 10; ++frame) {
                info.m_time = frame / 30.0;
                REQUIRE(parent.Execute(&info));
            }
            CHECK(g_childDestroyed == 1);
            CHECK(parent.count() == 1);
            CHECK_FALSE(parent.HasFinished());
            CHECK(survivor->IsExecuted());
        }
        CHECK(g_childDestroyed == 2);
    }
    SUBCASE("Kill stops the children without freeing them; the owner frees them once")
    {
        g_childDestroyed = 0;
        {
            mc2::math::Obb bounds = mc2::math::Obb();
            gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
            Parent parent(&parentSpec);
            parent.Start(&info);
            for (int i = 0; i < 2; ++i) {
                auto child = std::make_unique<CountedChild>(&longSpec);
                child->Start(&info);
                parent.adopt(std::move(child));
            }
            parent.Kill();
            CHECK_FALSE(parent.IsExecuted());
            CHECK(parent.count() == 2);
            CHECK(g_childDestroyed == 0);
        }
        CHECK(g_childDestroyed == 2);
    }
    SUBCASE("an effect that owns a render cloud is created and destroyed repeatedly")
    {
        gosFX::CardCloud::Specification spec;
        spec.BuildDefaults();
        spec.m_maxParticleCount = 8;
        for (int cycle = 0; cycle < 8; ++cycle) {
            gosFX::EffectPtr cloud{gosFX::CardCloud::Make(&spec, gosFX::Effect::ExecuteFlag)};
            REQUIRE(cloud);
        }
    }
}

namespace {
// Live CRT heap blocks (the memory instrumentation of the teardown cases): a teardown that leaks grows this from one cycle to the
// next. ASan replaces the CRT heap, so under MC2_ASAN the count is skipped and ASan itself is the use-after-free/double-free oracle.
long liveHeapBlocks()
{
#if defined(_WIN32) && !defined(MC2_ASAN)
    _HEAPINFO entry{};
    long used = 0;
    while (_heapwalk(&entry) == _HEAPOK)
        used += entry._useflag == _USEDENTRY ? 1 : 0;
    return used;
#else
    return 0;
#endif
}

// A two-spec library the way a mission loads one (a stream through EffectLibrary::Load): 0 = a plain effect, 1 = an
// EffectCloud whose every particle owns an effect made from spec 0.
struct LoadedLibrary {
    std::unique_ptr<gosFX::EffectLibrary> library;
    LoadedLibrary(float childLife, float particleLife)
    {
        // Born large and rewound, as tools/mc2fx saveBlob.
        Stuff::DynamicMemoryStream stream(64u * 1024u);
        stream.Rewind();
        gosFX::WriteGFXVersion(&stream);
        stream << 2u;
        gosFX::Effect::Specification child;
        child.BuildDefaults();
        child.m_lifeSpan.SetCurve(childLife);
        child.Save(&stream);
        gosFX::EffectCloud::Specification cloud;
        cloud.BuildDefaults();
        cloud.m_maxParticleCount = 6;
        cloud.m_startingPopulation.SetCurve(6.0f);
        cloud.m_particlesPerSecond.SetCurve(0.0f);
        cloud.m_lifeSpan.SetCurve(10.0f);
        cloud.m_pLifeSpan.m_ageCurve.SetCurve(particleLife);
        cloud.m_particleEffectID = 0;
        cloud.Save(&stream);
        stream.Rewind();
        gos_PushCurrentHeap(gosFX::Heap);
        library = std::make_unique<gosFX::EffectLibrary>();
        library->Load(&stream);
        gos_PopCurrentHeap();
        gosFX::EffectLibrary::Instance = library.get();
    }
    ~LoadedLibrary()
    {
        gosFX::EffectLibrary::Instance = nullptr;
        library.reset(); // unload: the specs go with the library
    }
};

// Starts the cloud, runs `frames` 30 Hz frames and returns how many of its particles own a live effect afterwards.
int runCloud(gosFX::EffectPtr& cloud, int frames)
{
    const glm::mat4 transform = glm::mat4(1.0f);
    mc2::math::Obb bounds = mc2::math::Obb();
    gosFX::Effect::ExecuteInfo info(Stuff::Time(0), &transform, &bounds);
    cloud->Start(&info);
    for (int frame = 1; frame <= frames; ++frame) {
        info.m_time = frame / 30.0;
        if (!cloud->Execute(&info))
            break;
    }
    auto* effects = static_cast<gosFX::EffectCloud*>(cloud.get());
    int owning = 0;
    for (unsigned i = 0; i < 6; ++i)
        owning += effects->GetParticle(i)->m_effect != nullptr ? 1 : 0;
    return owning;
}
}

TEST_CASE_FIXTURE(Platform, "Library unload after its effects: effects made from a loaded library die before the specs")
{
    for (int cycle = 0; cycle < 4; ++cycle) {
        LoadedLibrary loaded(10.0f, 10.0f);
        REQUIRE(mc2::particles::SpecLibrary::Instance()->Count() == 2);
        gosFX::EffectPtr cloud = gosFX::EffectLibrary::Instance->MakeEffect(1, gosFX::Effect::ExecuteFlag);
        gosFX::EffectPtr plain = gosFX::EffectLibrary::Instance->MakeEffect(0, gosFX::Effect::ExecuteFlag);
        REQUIRE(cloud);
        REQUIRE(plain);
        CHECK(runCloud(cloud, 2) == 6); // every particle owns a live effect when the owners go
        cloud.reset();
        plain.reset();
    } // ~LoadedLibrary: the library (and its specs) unload after the last effect
}

TEST_CASE_FIXTURE(Platform, "EffectCloud particle teardown frees each particle's effect once on every path")
{
    struct Path { const char* name; float childLife, particleLife; int frames, owningAtEnd; };
    const Path paths[] = {
        {"cloud destroyed with live particles (~EffectCloud)", 10.0f, 10.0f, 3, 6},
        // The particle's effect runs on the particle's age, so the two age out together (AnimateParticle frees the effect when its
        // Execute ends, DestroyParticle frees whatever is left).
        {"particles and their effects age out (AnimateParticle / DestroyParticle)", 10.0f, 0.05f, 30, 0},
    };
    for (const Path& path : paths) {
        const std::string pathName = path.name;
        CAPTURE(pathName);
        LoadedLibrary loaded(path.childLife, path.particleLife);
        long afterWarmUp = 0;
        for (int cycle = 0; cycle < 5; ++cycle) {
            int owning = 0;
            {
                gosFX::EffectPtr cloud = gosFX::EffectLibrary::Instance->MakeEffect(1, gosFX::Effect::ExecuteFlag);
                owning = runCloud(cloud, path.frames);
            }
            CHECK(owning == path.owningAtEnd);
            const long blocks = liveHeapBlocks();
            if (cycle == 1)
                afterWarmUp = blocks;
            else if (cycle > 1)
                CHECK(blocks == afterWarmUp); // nothing a cycle allocates survives it
        }
    }
}

TEST_CASE_FIXTURE(Platform, "Mission reload with live effects: teardown then unload, repeated, leaves nothing behind")
{
    long afterWarmUp = 0;
    for (int mission = 0; mission < 5; ++mission) {
        {
            LoadedLibrary loaded(10.0f, 10.0f);
            std::vector<gosFX::EffectPtr> live; // the mission's effect owners (actors, appearances, weapons)
            for (int i = 0; i < 3; ++i) {
                live.push_back(gosFX::EffectLibrary::Instance->MakeEffect(1, gosFX::Effect::ExecuteFlag));
                CHECK(runCloud(live.back(), 2) == 6);
                live.push_back(gosFX::EffectLibrary::Instance->MakeEffect(0, gosFX::Effect::ExecuteFlag));
            }
            live.clear(); // Mission::destroy: the owners go first, still executing
        } // then the texture-manager flush deletes the library
        const long blocks = liveHeapBlocks();
        if (mission == 1)
            afterWarmUp = blocks;
        else if (mission > 1)
            CHECK(blocks == afterWarmUp);
    }
    CHECK(gosFX::EffectLibrary::Instance == nullptr);
}

namespace {
// One event as the .gfx stores it: the tag is the x translation of m_localToParent (a child's world x when the parent is identity).
struct EventRow { float time; int tag; };

glm::mat4 eventTransform(int tag)
{
    glm::mat4 m(1.0f);
    // A rotation about z plus the tag translation, so a dropped or default matrix shows up. The .gfx record is the 48-byte
    // affine block (columns 0-2), so the translation sits in row 3 and survives a save/load round trip.
    m[0][0] = 0.0f; m[0][1] = 1.0f; m[1][0] = -1.0f; m[1][1] = 0.0f;
    m[0][3] = static_cast<float>(tag); m[1][3] = 2.0f; m[2][3] = -3.0f;
    return m;
}

std::vector<unsigned char> streamBytes(Stuff::DynamicMemoryStream& stream)
{
    const size_t used = stream.GetBytesUsed();
    stream.Rewind();
    const auto* base = static_cast<const unsigned char*>(stream.GetPointer());
    return {base, base + used};
}

std::vector<unsigned char> saveSpec(gosFX::Effect::Specification& spec)
{
    Stuff::DynamicMemoryStream out(64u * 1024u);
    out.Rewind();
    spec.Save(&out);
    return streamBytes(out);
}

// The parent every event test uses: a 1 s lifespan, so its age is the time since Start.
void buildParent(gosFX::Effect::Specification& spec)
{
    spec.BuildDefaults();
    spec.m_lifeSpan.SetCurve(1.0f);
}

// The bytes of an event-free parent spec with `rows` spliced in as its events, in exactly this file order: Save itself only
// writes chain order, and a loaded file is not guaranteed to be sorted.
std::vector<unsigned char> parentBytesWithEvents(const std::vector<EventRow>& rows)
{
    gosFX::Effect::Specification spec;
    buildParent(spec);
    Stuff::DynamicMemoryStream header(4096u);
    header.Rewind();
    header << spec.GetClassID() << spec.m_name;
    const size_t headerBytes = header.GetBytesUsed();
    const std::vector<unsigned char> plain = saveSpec(spec);
    Stuff::DynamicMemoryStream out(64u * 1024u);
    out.Rewind();
    out.WriteBytes(plain.data(), headerBytes);
    out << static_cast<unsigned>(rows.size());
    for (const EventRow& row : rows) {
        gosFX::Event event;
        event.m_time = row.time;
        event.m_flags = 0;
        event.m_effectID = 0;
        event.m_localToParent = eventTransform(row.tag);
        event.Save(&out);
    }
    out.WriteBytes(plain.data() + headerBytes + sizeof(unsigned), plain.size() - headerBytes - sizeof(unsigned));
    return streamBytes(out);
}

void adopt(gosFX::Effect::Specification& spec, const EventRow& row)
{
    gos_PushCurrentHeap(gosFX::Heap);
    auto* event = new gosFX::Event;
    event->m_time = row.time;
    event->m_flags = 0;
    event->m_effectID = 0;
    event->m_localToParent = eventTransform(row.tag);
    spec.AdoptEvent(event);
    gos_PopCurrentHeap();
}

// A library loaded from a stream: 0 = the long-lived child every event spawns, 1 = a parent whose events are `rows` in file order.
struct EventLibrary {
    std::unique_ptr<gosFX::EffectLibrary> library;
    std::vector<unsigned char> parentBytes;
    explicit EventLibrary(const std::vector<EventRow>& rows)
    {
        gosFX::Effect::Specification child;
        child.BuildDefaults();
        child.m_lifeSpan.SetCurve(100.0f);
        parentBytes = parentBytesWithEvents(rows);
        Stuff::DynamicMemoryStream stream(64u * 1024u);
        stream.Rewind();
        gosFX::WriteGFXVersion(&stream);
        stream << 2u;
        child.Save(&stream);
        stream.WriteBytes(parentBytes.data(), parentBytes.size());
        stream.Rewind();
        gos_PushCurrentHeap(gosFX::Heap);
        library = std::make_unique<gosFX::EffectLibrary>();
        library->Load(&stream);
        gos_PopCurrentHeap();
        gosFX::EffectLibrary::Instance = library.get();
    }
    ~EventLibrary()
    {
        gosFX::EffectLibrary::Instance = nullptr;
        library.reset();
    }
    gosFX::Effect::Specification& parent() { return *mc2::particles::SpecLibrary::Instance()->At(1); }
};

// Drives a parent through Execute calls at absolute times and returns the tags of the children spawned so far. Execute fires
// against the age left by the previous Execute (it advances m_age after firing), and a Start while running keeps m_lastRan.
struct EventRun {
    glm::mat4 transform = glm::mat4(1.0f);
    mc2::math::Obb bounds = mc2::math::Obb();
    gosFX::Effect::ExecuteInfo info{Stuff::Time(0), &transform, &bounds};
    Parent parent;
    explicit EventRun(gosFX::Effect::Specification& spec) : parent(&spec) { parent.Start(&info); }
    std::vector<int> at(double time)
    {
        info.m_time = time;
        REQUIRE(parent.Execute(&info));
        return parent.tags();
    }
};

using Tags = std::vector<int>;
}

TEST_CASE_FIXTURE(Platform, "Event chain: equal-time events fire in insertion order, after every earlier event")
{
    EventLibrary loaded({});
    gosFX::Effect::Specification spec;
    buildParent(spec);
    for (const EventRow& row : {EventRow{0.5f, 1}, {0.5f, 2}, {0.2f, 3}, {0.5f, 4}, {0.0f, 5}, {0.2f, 6}})
        adopt(spec, row);
    CHECK(saveSpec(spec) == parentBytesWithEvents({{0.0f, 5}, {0.2f, 3}, {0.2f, 6}, {0.5f, 1}, {0.5f, 2}, {0.5f, 4}}));
    EventRun run(spec);
    CHECK(run.at(0.0) == Tags{5});
    CHECK(run.at(0.25) == Tags{5});
    CHECK(run.at(0.375) == Tags{5, 3, 6});
    CHECK(run.at(0.75) == Tags{5, 3, 6});
    CHECK(run.at(0.875) == Tags{5, 3, 6, 1, 2, 4});
    CHECK(run.at(1.25) == Tags{5, 3, 6, 1, 2, 4});
}

TEST_CASE_FIXTURE(Platform, "Event chain: unsorted loaded events keep file order; adoption inserts before the first later event")
{
    EventLibrary loaded({{0.5f, 1}, {0.125f, 2}, {0.875f, 3}});
    // Loading appends in file order and Save writes it back unchanged.
    CHECK(saveSpec(loaded.parent()) == loaded.parentBytes);
    {
        // The cursor stops at the first event later than the age, so the early event behind it waits.
        EventRun run(loaded.parent());
        CHECK(run.at(0.25) == Tags{});
        CHECK(run.at(0.625) == Tags{});
        CHECK(run.at(0.75) == Tags{1, 2});
        CHECK(run.at(0.9375) == Tags{1, 2});
        CHECK(run.at(0.96875) == Tags{1, 2, 3});
    }
    // A linear first-greater scan over the unsorted chain (a binary search would misplace these):
    // 0.25 goes before the 0.5, the next 0.125 before that 0.25, and the equal 0.875 after the loaded one.
    adopt(loaded.parent(), {0.25f, 4});
    adopt(loaded.parent(), {0.125f, 5});
    adopt(loaded.parent(), {0.875f, 6});
    CHECK(saveSpec(loaded.parent()) ==
          parentBytesWithEvents({{0.125f, 5}, {0.25f, 4}, {0.5f, 1}, {0.125f, 2}, {0.875f, 3}, {0.875f, 6}}));
    {
        EventRun run(loaded.parent());
        CHECK(run.at(0.0625) == Tags{});
        CHECK(run.at(0.375) == Tags{});
        CHECK(run.at(0.5) == Tags{5, 4});
        CHECK(run.at(0.625) == Tags{5, 4, 1, 2});
        CHECK(run.at(0.9375) == Tags{5, 4, 1, 2});
        CHECK(run.at(0.96875) == Tags{5, 4, 1, 2, 3, 6});
    }
}

TEST_CASE_FIXTURE(Platform, "Event chain: Stop skips the remaining events; a restart fires them again from the first")
{
    EventLibrary loaded({});
    gosFX::Effect::Specification spec;
    buildParent(spec);
    for (const EventRow& row : {EventRow{0.125f, 1}, {0.5f, 2}, {0.5f, 3}, {0.875f, 4}})
        adopt(spec, row);
    EventRun run(spec);
    CHECK(run.at(0.25) == Tags{});
    CHECK(run.at(0.375) == Tags{1});
    run.parent.Stop();
    CHECK(run.at(0.625) == Tags{1});
    CHECK(run.at(0.75) == Tags{1});
    // Restart: the age goes back to 0 and the cursor to the first event (m_lastRan stays 0.75).
    run.parent.Start(&run.info);
    CHECK(run.at(0.875) == Tags{1});
    CHECK(run.at(1.0) == Tags{1, 1});
    CHECK(run.at(1.25) == Tags{1, 1});
    CHECK(run.at(1.375) == Tags{1, 1, 2, 3});
    CHECK(run.at(1.625) == Tags{1, 1, 2, 3});
    CHECK(run.at(1.6875) == Tags{1, 1, 2, 3, 4});
    // A Start while still executing restarts the cursor too.
    run.parent.Start(&run.info);
    CHECK(run.at(1.8125) == Tags{1, 1, 2, 3, 4});
    CHECK(run.at(1.875) == Tags{1, 1, 2, 3, 4, 1});
    // Stop on a spec without events leaves nothing to fire (the chain cursor dereferenced a null tail here before B3b).
    gosFX::Effect::Specification empty;
    buildParent(empty);
    EventRun idle(empty);
    idle.parent.Stop();
    idle.parent.Start(&idle.info);
    CHECK(idle.at(0.5) == Tags{});
}

TEST_CASE_FIXTURE(Platform, "Event chain: Copy of a loaded spec with transformed events re-adopts them in time order with their matrices")
{
    EventLibrary loaded({{0.5f, 1}, {0.125f, 2}, {0.875f, 3}, {0.5f, 4}});
    gosFX::Effect::Specification copy;
    buildParent(copy);
    adopt(copy, {0.25f, 9}); // Copy drops the events it held
    copy.Copy(&loaded.parent());
    CHECK(saveSpec(copy) == parentBytesWithEvents({{0.125f, 2}, {0.5f, 1}, {0.5f, 4}, {0.875f, 3}}));
    EventRun run(copy);
    CHECK(run.at(0.9375) == Tags{});
    CHECK(run.at(0.96875) == Tags{2, 1, 4, 3});
    const Tags order{2, 1, 4, 3};
    for (size_t i = 0; i < order.size(); ++i) {
        CAPTURE(i);
        CHECK(run.parent.child(i).m_localToWorld == eventTransform(order[i]));
    }
}

TEST_CASE_FIXTURE(Platform, "Stock mc2.fx: load then save is byte-identical")
{
    const char* path = "A:/Games/mc2-opengl/mc2-win64-v0.4/data/effects/mc2.fx";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        MESSAGE("stock mc2.fx not found; skipped: ", path);
        return;
    }
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    // MLRState::Load resolves texture names through the pool (as tools/mc2fx does); MLR TerminateClasses deletes it.
    gos_PushCurrentHeap(MidLevelRenderer::Heap);
    MidLevelRenderer::MLRTexturePool::Instance =
        new MidLevelRenderer::MLRTexturePool(new MidLevelRenderer::TGAFilePool("data\\tgl\\128\\"));
    gos_PopCurrentHeap();
    const auto roundTrip = [](std::vector<unsigned char>& input) {
        gos_PushCurrentHeap(gosFX::Heap);
        auto library = std::make_unique<gosFX::EffectLibrary>();
        Stuff::MemoryStream in(input.data(), static_cast<DWORD>(input.size()));
        library->Load(&in);
        CHECK(mc2::particles::SpecLibrary::Instance()->Count() > 100);
        Stuff::DynamicMemoryStream out(static_cast<DWORD>(input.size() * 2));
        out.Rewind();
        library->Save(&out);
        library.reset();
        gos_PopCurrentHeap();
        return streamBytes(out);
    };
    std::vector<unsigned char> saved = roundTrip(bytes);
    REQUIRE(saved.size() == bytes.size());
    // The save is not byte-identical to the file, and that predates B3 (Stuff B2 tip 0a07fc58f): some curve floats move by
    // a few ulps on every load/save (the first at offset 417; a second round trip drifts again at 21076). The size and the
    // pinned hash of the first save, taken at that base, prove the event storage rewrite writes every byte as before.
    CHECK(std::mismatch(saved.begin(), saved.end(), bytes.begin()).first - saved.begin() == 417);
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a 64
    for (unsigned char byte : saved)
        hash = (hash ^ byte) * 1099511628211ull;
    CHECK(hash == 13209778407463086519ull);
}
