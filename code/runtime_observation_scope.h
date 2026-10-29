#pragma once
#include <cstdint>
#include <string_view>

// Host diagnostics only; simulation sees no GameOS or clock implementation.
namespace mc2::observation {
void observeFrame(bool active);
class PhaseScope {
public:
    PhaseScope(unsigned index, std::string_view name);
    ~PhaseScope();
private:
    unsigned index_;
    std::string_view name_;
    uint64_t startNs_ = 0;
};
}
