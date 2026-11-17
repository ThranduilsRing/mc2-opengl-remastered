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

TEST_CASE("prewarm: private worker decode adopts on main and never in world") {
    Service service;
    const auto main = std::this_thread::get_id();
    std::thread::id decodeThread, adoptThread;
    std::atomic<bool> decoded{false};
    int unrelatedSteps = 0;
    auto logistics = service.submitMain([&] { ++unrelatedSteps; return false; });
    auto job = service.submit([&] {
        mc2::overlap::assertOwner(mc2::overlap::Owner::Prewarm, "prewarm.test.decode");
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
    CHECK(adoptThread == main);
    CHECK(unrelatedSteps == 0);
    service.finish(logistics);
    CHECK(unrelatedSteps == 1);
}
