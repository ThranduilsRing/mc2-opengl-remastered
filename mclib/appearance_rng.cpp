// The appearance-side random stream; see appearance_rng.h for what it is for.
#include "appearance_rng.h"
#include "det_rng.h"

namespace mc2_appearance_rng {

namespace {
constexpr uint32_t kSalt = 0x41505052u;   // keeps the appearance sequence apart from the simulation's even with the same seed
uint32_t s_state = kSalt;
uint32_t s_draws = 0;
}

void reseed(uint32_t seed)
{
	s_state = seed ^ kSalt;
	s_draws = 0;
}

long randomNumber(long range)
{
	if (range <= 0)
		return 0;
	++s_draws;
	return static_cast<long>(mc2_det_rng::next15(s_state) % static_cast<uint32_t>(range));
}

bool rollDice(long percent)
{
	++s_draws;
	return ((static_cast<long>(mc2_det_rng::next15(s_state)) * 100) >> 15) < percent;
}

uint32_t drawCount()
{
	return s_draws;
}

} // namespace mc2_appearance_rng
