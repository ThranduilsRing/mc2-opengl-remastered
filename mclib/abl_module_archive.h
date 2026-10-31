// Module control state; code, symbol tables and allocator addresses are derived.
#pragma once
#include "../code/save_sections.h"

namespace mc2::save {
inline constexpr auto tagAblModule = fourcc('A', 'M', 'O', 'D');
struct AblModuleRecord {
    std::string name;
    std::int32_t handle = 0;
    std::string previousState, state;
    bool initCalled = false;
    std::uint32_t statics = 0;
    std::vector<std::uint32_t> orderFlags;
    void serialize(Archive& a) {
        Text savedName{1, name, 4}, previous{1, previousState, 255}, current{1, state, 255};
        a(savedName, handle, previous, current, initCalled, statics, orderFlags);
        if (a.reading() && a.good()) {
            name = std::move(savedName.bytes);
            previousState = std::move(previous.bytes);
            state = std::move(current.bytes);
        }
        if (name.size() >= 5 || previousState.size() >= 256 || state.size() >= 256 ||
            name.find('\0') != std::string::npos || previousState.find('\0') != std::string::npos ||
            state.find('\0') != std::string::npos || handle < 0 || statics > maxSequenceLength)
            a.reject("abl_module");
    }
};
// Native ABLModule fields omitted deliberately:
// id: assigned in saved instance order; staticData/array addresses: ABL pool
// allocations from recompiled size tables; orderCallFlags address: allocated
// from numOrderCalls (words are values above); prevState/state: findState by
// saved names; returnVal: next execute computes it; debugger managers and
// trace/step/traceEntry/traceExit: fresh init/debugger setup, not simulation.
}
