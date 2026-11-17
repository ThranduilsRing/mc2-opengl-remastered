#include "doctest.h"
#include "prewarm.h"
#include <atomic>

using namespace mc2::prewarm;

TEST_CASE("prewarm: only covered front-end and load frames may pump") {
    for (auto screen : {Screen::FrontEndMovie, Screen::MainMenu, Screen::CampaignVideo, Screen::LoadScreen}) {
        CHECK(mayPumpMain(screen, false));
        CHECK_FALSE(mayPumpMain(screen, true));
    }
    CHECK_FALSE(mayPumpMain(Screen::MissionWorld, false));
    CHECK_FALSE(mayPumpMain(Screen::Other, false));
}

TEST_CASE("prewarm: unbounded logistics steps wait for finish") {
    Service service;
    int steps = 0;
    auto job = service.submitMain([&] { return ++steps < 3; });
    for (auto screen : {Screen::FrontEndMovie, Screen::MainMenu, Screen::CampaignVideo, Screen::Other, Screen::MissionWorld})
        service.pumpMain(4.0, screen, false);
    service.pumpMain(4.0, Screen::LoadScreen, true);
    CHECK(steps == 0);
    CHECK_FALSE(job->complete);
    service.finish(job);
    CHECK(steps == 3);
    CHECK(job->complete);
}

TEST_CASE("prewarm: cancelling pending Phase B releases its coroutine without running") {
    Service service;
    int bodies = 0;
    auto phaseB = [](int& runs, std::shared_ptr<int> lifetime) -> Sequence {
        ++runs;
        co_yield *lifetime;
    };
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> retained = lifetime;
    auto sequence = std::make_shared<Sequence>(phaseB(bodies, lifetime));
    auto job = service.submitMain([sequence] { return sequence->step(); });
    sequence.reset();
    lifetime.reset();
    service.pumpMain(4.0, Screen::MainMenu, false);
    CHECK_FALSE(retained.expired());
    CHECK_FALSE(job->complete);
    service.cancelMain(job);
    CHECK(retained.expired());
    CHECK(job->complete);
    CHECK_FALSE(job->step);
    service.cancelMain(job);
    service.finish(job);
    service.pumpMain(4.0, Screen::LoadScreen, false);
    CHECK(bodies == 0);
    auto needed = service.submitMain([&] { ++bodies; return false; });
    service.finish(needed);
    CHECK(bodies == 1);
}

TEST_CASE("prewarm: private worker decode adopts on main and never in world") {
    Service service;
    const auto main = std::this_thread::get_id();
    std::thread::id decodeThread, adoptThread;
    bool privateOwner = false;
    std::atomic<bool> decoded{false};
    int unrelatedSteps = 0;
    auto logistics = service.submitMain([&] { ++unrelatedSteps; return false; });
    auto job = service.submit([&] {
        mc2::overlap::assertOwner(mc2::overlap::Owner::Prewarm, "prewarm.test.decode");
        privateOwner = mc2::overlap::isOwner(mc2::overlap::Owner::Prewarm);
        decodeThread = std::this_thread::get_id();
        decoded.store(true);
        return [&] { adoptThread = std::this_thread::get_id(); };
    });
    while (!decoded.load()) std::this_thread::yield();
    service.pumpMain(4.0, Screen::MissionWorld, true);
    CHECK_FALSE(job->complete);
    CHECK(adoptThread == std::thread::id{});
    service.finish(job);
    CHECK(decodeThread != main);
    CHECK(privateOwner);
    CHECK(adoptThread == main);
    CHECK(unrelatedSteps == 0);
    service.finish(logistics);
    CHECK(unrelatedSteps == 1);
}
