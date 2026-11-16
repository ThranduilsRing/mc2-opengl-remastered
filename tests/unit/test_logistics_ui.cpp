#include "doctest.h"
#include "../../code/preview_choice.h"
#include <future>
#include <chrono>
#include <optional>
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
    std::promise<int> completion;
    auto result = completion.get_future();
    job.start([&] { return result.get(); });
    CHECK(job.valid()); CHECK_FALSE(job.poll()); CHECK_FALSE(job.launchResult());
    completion.set_value(37);
    job.launch.wait();
    CHECK(job.launchResult() == 37); CHECK_FALSE(job.valid());
    job.start([]() -> int { throw 1; });
    job.launch.wait(); CHECK(job.launchResult() == 1); CHECK_FALSE(job.valid());
    job.start([] { return 0; });
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
TEST_CASE("LogisticsUI/exit waits for launch before closing owned handles") {
    HANDLE handle = nullptr;
    {
        ImportJob job;
        job.start([&] { handle = job.process = CreateEventW(nullptr, TRUE, FALSE, nullptr); return 0; });
    }
    REQUIRE(handle != nullptr);
    DWORD flags = 0;
    CHECK_FALSE(GetHandleInformation(handle, &flags));
}
#endif
