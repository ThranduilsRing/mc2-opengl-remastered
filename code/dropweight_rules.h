// Drop-weight rules (MC2X-style "buy drop weight"): the price curve and the over-limit gate.
// All balance constants live here; the campaign stores only the purchase count.
#pragma once

#include <algorithm>
#include <cmath>

namespace dropweight {

// The lance weight warning is always active; offer its remedy in single-player.
inline bool purchaseAvailable(bool multiplayer) { return !multiplayer; }

inline constexpr int    kTonsPerPurchase = 5;       // drop weight added per purchase
inline constexpr double kBasePrice       = 25000.0; // C-Bills for the first purchase
inline constexpr double kLogK            = 0.75;    // growth: base * (1 + k * ln(1 + n))

// Price of the next purchase when n purchases were already made, rounded to 100 C-Bills.
inline long priceForNext(int n)
{
	const double p = kBasePrice * (1.0 + kLogK * std::log(1.0 + std::max(n, 0)));
	return static_cast<long>(std::floor(p / 100.0 + 0.5)) * 100L;
}

// The lance is never trimmed for the player: over the limit only blocks the screen buttons.
inline bool overLimit(long currentTons, long maxTons) { return currentTons > maxTons; }

}
