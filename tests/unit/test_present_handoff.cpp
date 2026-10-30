#include "doctest.h"
#include "sim_overlap.h"
#include "present_imgui_snapshot.h"
#include <barrier>
#include <thread>

TEST_CASE("present handoff transfers GPU ownership while keeping WSI on its worker") {
    using namespace mc2::overlap;
    registerThread(Owner::Main);
    setPresentWorkerRegistered(true);
    std::barrier phase(2);
    std::thread worker([&] {
        registerThread(Owner::Render);
        CHECK(isOwner(Owner::Render));
        CHECK_FALSE(isOwner(Owner::Main));
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        setRenderWorkerOwns(false);
        registerThread(Owner::Present);
        CHECK(isOwner(Owner::Present));
        CHECK_FALSE(isOwner(Owner::Render));
        phase.arrive_and_wait();
        phase.arrive_and_wait();
    });
    phase.arrive_and_wait();
    CHECK_FALSE(isOwner(Owner::Render));
    CHECK_FALSE(isOwner(Owner::Present));
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    CHECK(isOwner(Owner::Main));
    CHECK(isOwner(Owner::Render));
    CHECK_FALSE(isOwner(Owner::Present));
    phase.arrive_and_wait();
    worker.join();
    setPresentWorkerRegistered(false);
    CHECK(isOwner(Owner::Present));
    std::thread sim([&] {
        registerThread(Owner::Sim);
        CHECK(isOwner(Owner::Sim));
        CHECK_FALSE(isOwner(Owner::Render));
        CHECK_FALSE(isOwner(Owner::Present));
    });
    sim.join();
}

TEST_CASE("present ImGui snapshot survives producer buffer reuse") {
    auto* context = ImGui::CreateContext();
    {
        auto& io = ImGui::GetIO();
        io.DisplaySize = {800, 600};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGui::NewFrame();
        ImDrawList& source = *ImGui::GetBackgroundDrawList();
        source.PushClipRect({1, 2}, {30, 40});
        source.AddRectFilled({12, 34}, {22, 44}, 0xaabbccddu);
        ImGui::Render();
        ImDrawData& data = *ImGui::GetDrawData();
        mc2vk::PresentImguiSnapshot snapshot;
        REQUIRE(snapshot.assign(data));
        source.VtxBuffer[0].pos.x = 99;
        source.CmdBuffer[0].ClipRect.x = 99;
        source.IdxBuffer.clear();
        REQUIRE(snapshot.data.CmdListsCount == 1);
        CHECK(snapshot.data.CmdLists[0] != &source);
        CHECK(snapshot.data.CmdLists[0]->VtxBuffer[0].pos.x == 12);
        CHECK(snapshot.data.CmdLists[0]->CmdBuffer[0].ClipRect.x == 1);
        CHECK(snapshot.data.CmdLists[0]->IdxBuffer.Size == 6);
        snapshot.clear();
        CHECK(snapshot.data.CmdListsCount == 0);
        source.CmdBuffer[0].UserCallback = [](const ImDrawList*, const ImDrawCmd*) {};
        source.CmdBuffer[0].UserCallbackData = &source;
        CHECK_FALSE(snapshot.assign(data));
        CHECK(snapshot.data.CmdListsCount == 0);
    }
    ImGui::DestroyContext(context);
}
