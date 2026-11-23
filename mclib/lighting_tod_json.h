#pragma once
// timeOfDay{} sidecar block -> TodCurve. Key names follow the existing sun keys:
//   {"start":0.30,"rate":0.0,"keys":[{"t":0.0,"sunPitch":28,"sunYaw":20,"sunColorR":255,"sunColorG":214,"sunColorB":160,
//     "ambientR":50,"ambientG":50,"ambientB":55,"fogColorR":..,"fogColorG":..,"fogColorB":..,"fogStart":..,"fogFull":..}, ...]}
// A key sets only the channels it names; a colour needs all three components. Anything malformed leaves the mission Fixed
// (null result plus a message): a bad block never changes a stock look.
#include <algorithm>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "lighting_state.h"

namespace mc2::lighting {

inline std::shared_ptr<const TodCurve> parseTodCurve(const std::string& text, std::string* error = nullptr)
{
	auto fail = [&](const char* why) -> std::shared_ptr<const TodCurve> { if (error) *error = why; return nullptr; };
	if (text.empty()) return nullptr;
	const auto doc = nlohmann::json::parse(text, nullptr, false);
	if (!doc.is_object()) return fail("timeOfDay: not an object");
	const auto keys = doc.find("keys");
	if (keys == doc.end() || !keys->is_array() || keys->empty()) return fail("timeOfDay: no keys array");
	auto num = [](const nlohmann::json& o, const char* n, float& out) {
		const auto it = o.find(n);
		if (it == o.end() || !it->is_number()) return false;
		out = it->get<float>();
		return true;
	};
	auto byte3 = [&](const nlohmann::json& o, const std::string& base, uint8_t out[3]) {
		float v[3];
		const char* suffix[3] = {"R", "G", "B"};
		int have = 0;
		for (int i = 0; i < 3; ++i) have += num(o, (base + suffix[i]).c_str(), v[i]) ? 1 : 0;
		if (have != 3) return have == 0 ? 0 : -1;
		for (int i = 0; i < 3; ++i) out[i] = static_cast<uint8_t>(std::clamp(v[i], 0.0f, 255.0f) + 0.5f);
		return 1;
	};
	auto curve = std::make_shared<TodCurve>();
	for (const auto& k : *keys) {
		if (!k.is_object()) return fail("timeOfDay: key is not an object");
		TodKey key;
		if (!num(k, "t", key.t)) return fail("timeOfDay: key without t");
		if (num(k, "sunPitch", key.pitch)) key.mask |= kPitch;
		if (num(k, "sunYaw", key.yaw)) key.mask |= kYaw;
		if (const int r = byte3(k, "sunColor", key.sun); r < 0) return fail("timeOfDay: partial sunColor"); else if (r) key.mask |= kSun;
		if (const int r = byte3(k, "ambient", key.amb); r < 0) return fail("timeOfDay: partial ambient"); else if (r) key.mask |= kAmb;
		uint8_t fog[3];
		if (const int r = byte3(k, "fogColor", fog); r < 0) return fail("timeOfDay: partial fogColor");
		else if (r) { key.mask |= kFogColor; key.fogColor = (uint32_t(fog[0]) << 16) | (uint32_t(fog[1]) << 8) | fog[2]; }
		if (num(k, "fogStart", key.fogStart)) key.mask |= kFogStart;
		if (num(k, "fogFull", key.fogFull)) key.mask |= kFogFull;
		curve->keys.push_back(key);
	}
	std::stable_sort(curve->keys.begin(), curve->keys.end(), [](const TodKey& a, const TodKey& b) { return a.t < b.t; });
	for (size_t i = 1; i < curve->keys.size(); ++i)
		if (!(curve->keys[i].t > curve->keys[i - 1].t)) return fail("timeOfDay: key times must be strictly increasing");
	num(doc, "start", curve->start);
	num(doc, "rate", curve->rate);
	curve->start = std::clamp(curve->start, curve->keys.front().t, curve->keys.back().t);
	return curve;
}

} // namespace mc2::lighting
