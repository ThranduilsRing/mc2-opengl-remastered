#include "random.hpp"
#include <cstdio>

// The standalone probe supplies the engine seed source without a game process.
int gos_rand() { return 17; }

int main()
{
    Stuff::Random::InitializeClass();
    const auto legacy = Stuff::Random::GetState();
    Stuff::Random::Stream stream(42);
    const auto initial = stream.GetState();
    const auto first = stream.GetFraction();
    const auto saved = stream.GetState();
    const auto second = stream.GetFraction();
    if (initial.seed != (42u ^ Stuff::Random::Stream::Id) || initial.value != initial.seed
        || first != 9340.0f / 32768.0f || second != 21221.0f / 32768.0f)
        return 7;
    stream.Restore(saved);
    if (stream.GetFraction() != second || stream.GetDrawCount() != 2)
        return 1;
    stream.Reset(42);
    if (stream.GetState() != initial || stream.GetFraction() != first)
        return 2;
    Stuff::Random::Stream other(43);
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
    std::puts("VFX RNG: reset, restore, draw count and range PASS");
}
