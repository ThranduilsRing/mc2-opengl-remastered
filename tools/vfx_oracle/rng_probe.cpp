#include "random.hpp"
#include <cstdint>
#include <cstdio>

// The standalone probe supplies the engine seed source without a game process.
int gos_rand() { return 17; }
void gos_srand(unsigned int) {}

namespace {
// A probe-only candidate sequence; runtime VFX still uses the legacy Stuff table.
class CandidateStream {
public:
    static constexpr std::uint32_t Id = 0x56465852u;
    struct State {
        std::uint32_t seed, value;
        std::uint64_t draws;
        bool operator==(const State&) const = default;
    };
    explicit CandidateStream(std::uint32_t seed) { Reset(seed); }
    void Reset(std::uint32_t seed) { state_ = {seed ^ Id, seed ^ Id, 0}; }
    State GetState() const { return state_; }
    void Restore(State state) { state_ = state; }
    std::uint64_t GetDrawCount() const { return state_.draws; }
    float GetFraction() {
        state_.value = state_.value * 1664525u + 1013904223u;
        ++state_.draws;
        return static_cast<float>((state_.value >> 17) & 0x7fffu) / 32768.0f;
    }
private:
    State state_;
};
}

int main()
{
    Stuff::Random::InitializeClass();
    const auto legacy = Stuff::Random::GetState();
    CandidateStream stream(42);
    const auto initial = stream.GetState();
    const auto first = stream.GetFraction();
    const auto saved = stream.GetState();
    const auto second = stream.GetFraction();
    if (initial.seed != (42u ^ CandidateStream::Id) || initial.value != initial.seed
        || first != 9340.0f / 32768.0f || second != 21221.0f / 32768.0f)
        return 7;
    stream.Restore(saved);
    if (stream.GetFraction() != second || stream.GetDrawCount() != 2)
        return 1;
    stream.Reset(42);
    if (stream.GetState() != initial || stream.GetFraction() != first)
        return 2;
    CandidateStream other(43);
    if (other.GetFraction() == first)
        return 3;
    for (int i = 0; i < 10000; ++i)
    {
        const auto fraction = stream.GetFraction();
        if (fraction < 0.0f || fraction >= 1.0f)
            return 4;
    }
    if (Stuff::Random::GetState() != legacy)
        return 5;
    Stuff::Random::GetFraction();
    if (Stuff::Random::GetDrawCount() != legacy.draws + 1)
        return 6;
    Stuff::Random::TerminateClass();
    std::puts("Probe-only VFX RNG candidate: reset, restore, draw count and range PASS");
}
