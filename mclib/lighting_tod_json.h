#pragma once
// timeOfDay{} sidecar block -> TodCurve. Key names follow the existing sun keys:
//   {"start":0.30,"rate":0.0,"keys":[{"t":0.0,"sunPitch":28,"sunYaw":20,"sunColorR":255,"sunColorG":214,"sunColorB":160,
//     "ambientR":50,"ambientG":50,"ambientB":55,"fogColorR":..,"fogColorG":..,"fogColorB":..,"fogStart":..,"fogFull":..}, ...]}
// A key sets only the channels it names; a colour needs all three components. Anything malformed leaves the mission Fixed
// (null result plus a message): a bad block never changes a stock look. Malformed includes a non-finite or out-of-float-range number
// and a negative rate. Keys past kMaxTodKeys are dropped (TodCurve::droppedKeys); sun pitch is clamped to [-10, 89] (dawn and dusk
// need a low sun; the low-sun shadows are T6).
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lighting_state.h"

namespace mc2::lighting {

namespace detail {

inline std::optional<float> jsonNum(const nlohmann::json& o, std::string_view name)
{
	const auto it = o.find(std::string(name));
	if (it == o.end() || !it->is_number()) return std::nullopt;
	return it->get<float>();
}

// Every number a key or the block carries must fit a finite float (nlohmann reads 1e999 as inf): else the block is rejected whole.
inline bool numbersFitFloat(const nlohmann::json& o)
{
	for (const auto& v : o)
		if (v.is_number()) {
			const double d = v.get<double>();
			if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(FLT_MAX)) return false;
		}
	return true;
}

// Three bytes <base>R/G/B. nullopt + partial=true when only some of the components are present.
inline std::optional<std::array<uint8_t, 3>> jsonBytes(const nlohmann::json& o, std::string_view base, bool& partial)
{
	std::array<uint8_t, 3> out{};
	int have = 0;
	const char* suffix[3] = {"R", "G", "B"};
	for (int i = 0; i < 3; ++i) {
		if (const auto v = jsonNum(o, std::string(base) + suffix[i])) {
			out[i] = static_cast<uint8_t>(std::clamp(*v, 0.0f, 255.0f) + 0.5f);
			++have;
		}
	}
	partial = have != 0 && have != 3;
	if (have != 3) return std::nullopt;
	return out;
}

} // namespace detail

// Empty text: a null curve (no block, Fixed). Malformed: the error message.
using TodParse = std::expected<std::shared_ptr<const TodCurve>, std::string>;
inline TodParse parseTodCurve(std::string_view text)
{
	auto fail = [](std::string_view why) { return TodParse(std::unexpected(std::string(why))); };
	if (text.empty()) return TodParse(std::shared_ptr<const TodCurve>());
	const auto doc = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
	if (!doc.is_object()) return fail("timeOfDay: not an object");
	const auto keys = doc.find("keys");
	if (keys == doc.end() || !keys->is_array() || keys->empty()) return fail("timeOfDay: no keys array");
	if (!detail::numbersFitFloat(doc)) return fail("timeOfDay: non-finite or out-of-range number");
	auto curve = std::make_shared<TodCurve>();
	for (const auto& k : *keys) {
		if (!k.is_object()) return fail("timeOfDay: key is not an object");
		if (!detail::numbersFitFloat(k)) return fail("timeOfDay: non-finite or out-of-range number in a key");
		TodKey key;
		const auto t = detail::jsonNum(k, "t");
		if (!t) return fail("timeOfDay: key without t");
		key.t = *t;
		if (const auto v = detail::jsonNum(k, "sunPitch")) { key.pitch = std::clamp(*v, kMinCurvePitch, kMaxCurvePitch); key.mask |= kPitch; }
		if (const auto v = detail::jsonNum(k, "sunYaw")) { key.yaw = *v; key.mask |= kYaw; }
		bool partial = false;
		if (const auto v = detail::jsonBytes(k, "sunColor", partial)) { std::copy(v->begin(), v->end(), key.sun); key.mask |= kSun; }
		else if (partial) return fail("timeOfDay: partial sunColor");
		if (const auto v = detail::jsonBytes(k, "ambient", partial)) { std::copy(v->begin(), v->end(), key.amb); key.mask |= kAmb; }
		else if (partial) return fail("timeOfDay: partial ambient");
		if (const auto v = detail::jsonBytes(k, "fogColor", partial)) {
			key.fogColor = (static_cast<uint32_t>((*v)[0]) << 16) | (static_cast<uint32_t>((*v)[1]) << 8) | (*v)[2];
			key.mask |= kFogColor;
		} else if (partial) return fail("timeOfDay: partial fogColor");
		if (const auto v = detail::jsonNum(k, "fogStart")) { key.fogStart = *v; key.mask |= kFogStart; }
		if (const auto v = detail::jsonNum(k, "fogFull")) { key.fogFull = *v; key.mask |= kFogFull; }
		curve->keys.push_back(key);
	}
	std::stable_sort(curve->keys.begin(), curve->keys.end(), [](const TodKey& a, const TodKey& b) { return a.t < b.t; });
	for (size_t i = 1; i < curve->keys.size(); ++i)
		if (!(curve->keys[i].t > curve->keys[i - 1].t)) return fail("timeOfDay: key times must be strictly increasing");
	if (curve->keys.size() > kMaxTodKeys) {
		curve->droppedKeys = curve->keys.size() - kMaxTodKeys;
		curve->keys.resize(kMaxTodKeys);
	}
	curve->start = detail::jsonNum(doc, "start").value_or(0.0f);
	curve->rate = detail::jsonNum(doc, "rate").value_or(0.0f);
	if (curve->rate < 0.0f) return fail("timeOfDay: negative rate");
	curve->start = std::clamp(curve->start, curve->keys.front().t, curve->keys.back().t);
	return curve;
}

} // namespace mc2::lighting
