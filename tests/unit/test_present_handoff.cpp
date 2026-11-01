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

// C6: jittered stress of the enqueue/seal/waitSealed mailbox. The model mirrors GameOS/gameos/present_thread.cpp (same mutex/cv
// protocol and the same mc2::overlap transitions: enqueue -> setRenderWorkerOwns(true), seal -> setRenderWorkerOwns(false) then
// Present role, then the WSI step outside the lock). A sequence log is replayed through the allowed state machine afterwards.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace {
enum class Ev : uint8_t { Enq, WorkBegin, Seal, Wsi, WaitRet };

struct MailboxModel {
    std::mutex m;
    std::condition_variable cv;
    bool pending = false, sealed = true, quitting = false;
    std::atomic<uint32_t> seq{0};
    struct Entry { Ev ev; uint32_t cycle; };
    std::vector<Entry> log;
    explicit MailboxModel(size_t cycles) : log(cycles * 5 + 8) {}
    void note(Ev ev, uint32_t cycle) { log[seq.fetch_add(1)] = {ev, cycle}; }
};

void jitter(std::mt19937& rng) {
    const unsigned r = rng();
    if ((r & 1023u) == 0) { std::this_thread::sleep_for(std::chrono::microseconds(50 + (r >> 10) % 200)); return; }
    if ((r & 7u) == 0) { std::this_thread::yield(); return; }
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(((r >> 3) & 15u) * 500u);
    while (std::chrono::steady_clock::now() < until) {}
}

// Returns an empty string when the log obeys the mailbox state machine, else the first violation.
std::string replay(const MailboxModel& mb, size_t cycles) {
    uint32_t enq = 0, wb = 0, seal = 0, wsi = 0, ret = 0;
    const size_t n = mb.seq.load();
    if (n != cycles * 5) return "log length " + std::to_string(n);
    for (size_t i = 0; i < n; ++i) {
        const auto [ev, c] = mb.log[i];
        const char* bad = nullptr;
        switch (ev) {
        case Ev::Enq:       if (enq != ret || c != enq) bad = "enqueue before previous waitSealed returned"; ++enq; break;
        case Ev::WorkBegin: if (wb + 1 != enq || wsi != wb || c != wb) bad = "worker took packet before enqueue or before prior WSI"; ++wb; break;
        case Ev::Seal:      if (seal + 1 != wb || c != seal) bad = "seal without a worked packet"; ++seal; break;
        case Ev::Wsi:       if (wsi + 1 != seal || c != wsi) bad = "WSI before seal"; ++wsi; break;
        case Ev::WaitRet:   if (ret + 1 != seal || c != ret) bad = "waitSealed returned before seal"; ++ret; break;
        }
        if (bad) return std::string(bad) + " at log index " + std::to_string(i) + " cycle " + std::to_string(c);
    }
    return enq == cycles && wb == cycles && seal == cycles && wsi == cycles && ret == cycles ? "" : "incomplete cycle counts";
}
}

TEST_CASE("present mailbox stress: ownership, seal-before-WSI and waitSealed ordering over 100k jittered cycles") {
    using namespace mc2::overlap;
    constexpr size_t kCycles = 100000;
    registerThread(Owner::Main);
    setRenderWorkerOwns(false);
    setPresentWorkerRegistered(true);
    MailboxModel mb(kCycles);
    std::atomic<uint64_t> workerFailures{0};

    std::thread worker([&] {
        std::mt19937 rng(0xC6u);
        registerThread(Owner::Render);
        for (uint32_t cycle = 0;; ++cycle) {
            {
                std::unique_lock lock(mb.m);
                mb.cv.wait(lock, [&] { return mb.pending || mb.quitting; });
                if (!mb.pending) break;
                mb.pending = false;
                mb.note(Ev::WorkBegin, cycle);
            }
            registerThread(Owner::Render);
            if (!isOwner(Owner::Render) || isOwner(Owner::Main)) ++workerFailures;   // render work only while the worker owns it
            jitter(rng);                                                             // record + submit
            {
                std::lock_guard lock(mb.m);                                          // seal()
                setRenderWorkerOwns(false);
                registerThread(Owner::Present);
                mb.sealed = true;
                mb.note(Ev::Seal, cycle);
                if (!isOwner(Owner::Present) || isOwner(Owner::Render)) ++workerFailures;   // WSI step: after the seal only
                mb.cv.notify_all();
            }
            jitter(rng);                                                             // acquire/present blocking
            mb.note(Ev::Wsi, cycle);
        }
    });

    std::mt19937 rng(0x5EEDu);
    uint64_t detected = 0, allowed = 0, unexpected = 0;
    for (uint32_t cycle = 0; cycle < kCycles; ++cycle) {
        {   // enqueue()
            std::lock_guard lock(mb.m);
            REQUIRE((!mb.pending && mb.sealed));
            mb.sealed = false;
            mb.pending = true;
            setRenderWorkerOwns(true);
            mb.note(Ev::Enq, cycle);
            mb.cv.notify_all();
        }
        // Simulated main VK access while the worker may be recording: judged under the mailbox lock, so "unsealed" is exact.
        const unsigned probes = 1 + (rng() & 3u);
        for (unsigned p = 0; p < probes; ++p) {
            {
                std::lock_guard lock(mb.m);
                const bool owner = isOwner(Owner::Render);   // the predicate behind assertOwner(Render)
                if (!mb.sealed) { if (owner) ++unexpected; else ++detected; }
                else { if (owner) ++allowed; else ++unexpected; }
            }
            jitter(rng);
        }
        {   // waitSealed()
            std::unique_lock lock(mb.m);
            mb.cv.wait(lock, [&] { return mb.sealed; });
            mb.note(Ev::WaitRet, cycle);
            if (!isOwner(Owner::Render) || !isOwner(Owner::Main)) ++unexpected;   // Main owns Render again once sealed
        }
        jitter(rng);   // main-side frame work that may overlap the worker's WSI
    }
    {
        std::lock_guard lock(mb.m);
        mb.quitting = true;
    }
    mb.cv.notify_all();
    worker.join();
    setPresentWorkerRegistered(false);
    setRenderWorkerOwns(false);

    CHECK(unexpected == 0);
    CHECK(workerFailures.load() == 0);
    CHECK(detected > 0);            // a main VK access inside [enqueue, seal) is caught by the ownership predicate
    CHECK(allowed > 0);             // and the same access after the seal is allowed
    const std::string why = replay(mb, kCycles);
    CHECK(why.empty());
    if (!why.empty()) MESSAGE(why);
}

TEST_CASE("present mailbox state machine rejects a seal-less WSI and an early waitSealed return") {
    MailboxModel good(1);
    good.note(Ev::Enq, 0); good.note(Ev::WorkBegin, 0); good.note(Ev::Seal, 0); good.note(Ev::Wsi, 0); good.note(Ev::WaitRet, 0);
    CHECK(replay(good, 1).empty());
    MailboxModel noSeal(1);
    noSeal.note(Ev::Enq, 0); noSeal.note(Ev::WorkBegin, 0); noSeal.note(Ev::Wsi, 0); noSeal.note(Ev::Seal, 0); noSeal.note(Ev::WaitRet, 0);
    CHECK_FALSE(replay(noSeal, 1).empty());
    MailboxModel early(1);
    early.note(Ev::Enq, 0); early.note(Ev::WorkBegin, 0); early.note(Ev::WaitRet, 0); early.note(Ev::Seal, 0); early.note(Ev::Wsi, 0);
    CHECK_FALSE(replay(early, 1).empty());
}
