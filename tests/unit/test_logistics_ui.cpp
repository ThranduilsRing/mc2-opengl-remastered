#include "doctest.h"
#include "../../code/preview_choice.h"
#include <future>
#include <chrono>
#include <optional>
#include <string>
#include <memory>
#include <new>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif
namespace {
#include "import_job.inc"
constexpr unsigned ES_EDITREADONLY = 1;
constexpr int ENTRY_MARGIN = 2;
struct aEdit {
    unsigned dwStyleFlags = 0;
    int nLeftOffset = 0, nInsertion2 = 40;
    int width() const { return 20; }
    int charXPos(int n) const { return n; }
    void makeCursorVisible();
    void flushCursorRight();
};
#include "edit_scroll.inc"
struct SimpleCamera {
    bool pObject = true;
    mutable int composites = 0;
    void drawPreviewToPanel(float, float, float, float) const;
};
#include "preview_panel_guard.inc"
}
TEST_CASE("LogisticsUI/enc choice releases failures and preserves precedence") {
    struct Type { bool baseShapeLoadFailed; } enc{false}, plain{false};
    for (int scenario = 0; scenario < 5; ++scenario) {
        enc.baseShapeLoadFailed = scenario == 1 || scenario == 4;
        plain.baseShapeLoadFailed = scenario == 4;
        bool selected = false;
        int loads = 0, releases = 0;
        auto* result = choosePreviewType(true, selected, [&](bool useEnc) -> Type* {
            ++loads;
            if (!useEnc) CHECK(releases == (scenario == 1 || scenario == 4 ? 1 : 0));
            return useEnc ? (scenario == 2 || scenario == 3 ? nullptr : &enc) : (scenario == 3 ? nullptr : &plain);
        }, [&](Type* type) { CHECK(type->baseShapeLoadFailed); ++releases; });
        CHECK(selected == (scenario == 0));
        CHECK(result == (scenario == 0 ? &enc : scenario >= 3 ? nullptr : &plain));
        CHECK(loads == (scenario == 0 ? 1 : 2));
        CHECK(releases == (scenario == 4 ? 2 : scenario == 1 ? 1 : 0));
    }
}
TEST_CASE("LogisticsUI/read-only label starts left; editable cursor still scrolls") {
    aEdit edit;
    edit.flushCursorRight(); CHECK(edit.nLeftOffset == 24);
    edit.dwStyleFlags = ES_EDITREADONLY;
    edit.flushCursorRight(); CHECK(edit.nLeftOffset == 0);
    edit.nLeftOffset = 15;
    edit.makeCursorVisible(); CHECK(edit.nLeftOffset == 0);
    edit.dwStyleFlags = 0;
    edit.makeCursorVisible(); CHECK(edit.nLeftOffset == 24);
}
TEST_CASE("LogisticsUI/import launch publishes completion and errors without blocking") {
    ImportJob job;
    std::promise<ImportLaunchResult> completion;
    auto result = completion.get_future();
    job.start([&] { return result.get(); });
    CHECK(job.valid()); CHECK_FALSE(job.poll()); CHECK_FALSE(job.launchResult());
    ImportLaunchResult failed; failed.errorCode = 37;
    completion.set_value(failed);
    job.launch.wait();
    CHECK(job.launchResult() == 37); CHECK_FALSE(job.valid());
    job.start([]() -> ImportLaunchResult { throw 1; });
    job.launch.wait(); CHECK(job.launchResult() == 1); CHECK_FALSE(job.valid());
    job.start([] { return ImportLaunchResult{}; });
    job.launch.wait(); CHECK(job.launchResult() == 0); CHECK_FALSE(job.valid());
}
TEST_CASE("LogisticsUI/missing panel suppresses only its own composite") {
    SimpleCamera bay, purchase;
    bay.drawPreviewToPanel(0, 0, 1, 1);
    bay.pObject = false;
    bay.drawPreviewToPanel(0, 0, 1, 1);
    purchase.drawPreviewToPanel(0, 0, 1, 1);
    CHECK(bay.composites == 1); CHECK(purchase.composites == 1);
    bay.pObject = true;
    bay.drawPreviewToPanel(0, 0, 1, 1);
    CHECK(bay.composites == 2);
}
#ifdef _WIN32
TEST_CASE("LogisticsUI/launch allocation failure closes acquired handles") {
    const auto name = L"MC2ImportThrow-" + std::to_wstring(GetCurrentProcessId());
    ImportJob job;
    job.start([&]() -> ImportLaunchResult {
        const ImportLaunchResult::Handle log(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
        const ImportLaunchResult::Handle input(CreateEventW(nullptr, TRUE, FALSE, (name + L"-input").c_str()));
        const ImportLaunchResult::Handle managed(CreateEventW(nullptr, TRUE, FALSE, (name + L"-job").c_str()));
        if (!log.value || !input.value || !managed.value) return {nullptr, nullptr, 99};
        throw std::bad_alloc(); // Same unwind as the command's wstring allocation.
    });
    job.launch.wait();
    CHECK(job.launchResult() == 1);
    CHECK_FALSE(job.valid());
    for (const auto& objectName : {name, name + L"-input", name + L"-job"}) {
        const auto remaining = OpenEventW(SYNCHRONIZE, FALSE, objectName.c_str());
        CHECK(remaining == nullptr);
        if (remaining) CloseHandle(remaining);
    }
}
TEST_CASE("LogisticsUI/pending launch destruction retains state and closes handles") {
    struct State {
        std::promise<void> acquired, destroying;
        std::wstring name = L"MC2ImportPending-" + std::to_wstring(GetCurrentProcessId());
        bool created = false;
    };
    auto state = std::make_shared<State>();
    const std::weak_ptr<State> captured = state;
    {
        struct PendingJob : ImportJob {
            State& state;
            explicit PendingJob(State& value) : state(value) {}
            ~PendingJob() { state.destroying.set_value(); }
        } job(*state);
        job.start([state]() -> ImportLaunchResult {
            const auto process = CreateEventW(nullptr, TRUE, FALSE, state->name.c_str());
            const auto managed = CreateEventW(nullptr, TRUE, FALSE, (state->name + L"-job").c_str());
            state->created = process && managed;
            state->acquired.set_value();
            state->destroying.get_future().wait();
            return {process, managed, 0};
        });
        state->acquired.get_future().wait();
        CHECK(state->created);
        CHECK(job.launch.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    } // Derived destructor releases launch; base destructor collects and closes its handles.
    CHECK_FALSE(captured.expired());
    for (const auto& objectName : {state->name, state->name + L"-job"}) {
        const auto remaining = OpenEventW(SYNCHRONIZE, FALSE, objectName.c_str());
        CHECK(remaining == nullptr);
        if (remaining) CloseHandle(remaining);
    }
    state.reset();
    CHECK(captured.expired());
}
TEST_CASE("LogisticsUI/future transfers handles for poll and exit cleanup") {
    for (bool poll : {false, true}) {
        const auto name = L"MC2ImportOwnership-" + std::to_wstring(GetCurrentProcessId()) + (poll ? L"-poll" : L"-exit");
        const auto jobName = name + L"-job";
        {
            ImportJob job;
            std::promise<void> release;
            auto ready = release.get_future();
            job.start([&]() -> ImportLaunchResult {
                ready.wait();
                return {CreateEventW(nullptr, TRUE, FALSE, name.c_str()),
                    CreateEventW(nullptr, TRUE, FALSE, jobName.c_str()), 0};
            });
            CHECK(job.valid()); CHECK_FALSE(job.poll()); CHECK_FALSE(job.launchResult());
            CHECK(job.process == nullptr); CHECK(job.job == nullptr);
            release.set_value();
            job.launch.wait();
            for (const auto& objectName : {name, jobName}) {
                const auto live = OpenEventW(SYNCHRONIZE, FALSE, objectName.c_str());
                CHECK(live != nullptr);
                if (live) CloseHandle(live);
            }
            if (poll) {
                CHECK(job.process == nullptr); CHECK(job.job == nullptr);
                CHECK(job.launchResult() == 0);
                REQUIRE(job.process != nullptr); REQUIRE(job.job != nullptr);
                CHECK(job.valid());
                CHECK_FALSE(job.poll()); CHECK(job.valid());
            }
        }
        // Named objects cease to exist after the last owner closes them. No stale HANDLE is used.
        for (const auto& objectName : {name, jobName}) {
            const auto remaining = OpenEventW(SYNCHRONIZE, FALSE, objectName.c_str());
            CHECK(remaining == nullptr);
            if (remaining) CloseHandle(remaining);
        }
    }
}
#endif
