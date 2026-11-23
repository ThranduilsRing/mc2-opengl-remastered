#include "doctest.h"
#include "../../mclib/lighting_legacy.h"
#include "../../mclib/lighting_state.h"
#include "../../mclib/lighting_tod_json.h"
#include <cstring>

// Trigger paths: mclib/lighting_state.h, mclib/lighting_legacy.h, mclib/lighting_tod_json.h, mclib/camera.cpp (sun keys),
// mclib/camera_daylight.cpp, code/gamecam.cpp (renderTerrainSceneState).
using namespace mc2::lighting;

namespace {
// Stock camera blocks (mc2srcdata missions/*.fit: LightDirPitch/Yaw, DayLight*, DayAmbient*; mc2_10 is the night one).
struct Stock { const char* name; float pitch, yaw; uint8_t sun[3], amb[3]; };
constexpr Stock kStock[] = {
	{"mc2_01", 50.0f, 11.0f, {255, 255, 225}, {71, 71, 78}},
	{"mc2_10", 180.0f, 28.75f, {64, 64, 64}, {32, 32, 32}},
	{"mc2_03", 46.0f, -206.0f, {242, 242, 225}, {31, 31, 31}},
};

LightingState authoredOf(const Stock& m)
{
	LightingState s;
	s.pitch = m.pitch; s.yaw = m.yaw;
	sunDirFromAngles(s.pitch, s.yaw, s.sunDir);
	for (int i = 0; i < 3; ++i) { s.sun[i] = m.sun[i]; s.amb[i] = m.amb[i]; }
	s.fogColor = 0xffa0a0a5u; s.fogStart = 800.0f; s.fogFull = 250.0f;
	return s;
}

// The pre-T1 inline key code of Camera::init (camera.cpp, before the LightingState producer), verbatim, on plain variables.
struct OldKeys { float pitch, yaw, dir[3]; uint8_t sun[3], amb[3], dayAmb[3]; };
OldKeys oldInline(const Stock& m, const SunKeys& k, bool scaledInSave)
{
	OldKeys o{};
	o.pitch = m.pitch; o.yaw = m.yaw;
	for (int i = 0; i < 3; ++i) { o.sun[i] = m.sun[i]; o.amb[i] = m.amb[i]; o.dayAmb[i] = m.amb[i]; }
	if (const auto v = k.pitch) o.pitch = std::max(*v, 26.0f);
	if (const auto v = k.yaw) o.yaw = *v;
	o.dir[0] = cos((o.yaw + 45.0f) * DEGREES_TO_RADS);
	o.dir[1] = sin((o.yaw + 45.0f) * DEGREES_TO_RADS);
	o.dir[2] = sin(o.pitch * DEGREES_TO_RADS);
	const auto r = k.colorR, g = k.colorG, b = k.colorB;
	if (r && g && b) {
		o.sun[0] = static_cast<unsigned char>(*r + 0.5f);
		o.sun[1] = static_cast<unsigned char>(*g + 0.5f);
		o.sun[2] = static_cast<unsigned char>(*b + 0.5f);
	}
	if (const auto kk = (scaledInSave ? std::optional<float>() : k.ambientScale)) {
		const auto sc = [kk](unsigned char& v) { v = static_cast<unsigned char>(std::min(255.0f, v * *kk + 0.5f)); };
		for (auto& v : o.amb) sc(v);
		for (auto& v : o.dayAmb) sc(v);
	}
	return o;
}

bool sameBytes(const LightingState& a, const LightingState& b) { return a == b; }   // member-wise (the struct has padding, so no memcmp)

TodCurve sunCurve()
{
	TodCurve c;
	TodKey a; a.t = 0.0f; a.mask = kPitch | kYaw | kSun | kAmb | kFogStart;
	a.pitch = 28.0f; a.yaw = 10.0f; a.sun[0] = 255; a.sun[1] = 214; a.sun[2] = 160; a.amb[0] = 50; a.amb[1] = 50; a.amb[2] = 55; a.fogStart = 100.0f;
	TodKey b = a; b.t = 0.5f; b.pitch = 60.0f; b.yaw = 40.0f; b.sun[0] = 255; b.sun[1] = 255; b.sun[2] = 225; b.amb[0] = 71; b.amb[1] = 71; b.amb[2] = 78; b.fogStart = 300.0f;
	TodKey d = a; d.t = 1.0f; d.pitch = 30.0f; d.yaw = 70.0f; d.sun[0] = 205; d.sun[1] = 84; d.sun[2] = 31; d.amb[0] = 32; d.amb[1] = 32; d.amb[2] = 32; d.fogStart = 50.0f;
	c.keys = {a, b, d};
	return c;
}
} // namespace

TEST_CASE("LightingState/no curve returns the authored state byte for byte for stock mission keys") {
	for (const auto& m : kStock) {
		const LightingState a = authoredOf(m);
		const TodCurve empty;
		for (float t : {-1.0f, 0.0f, 0.3f, 1.0f, 5.0f}) {
			CHECK_MESSAGE(sameBytes(evaluate(nullptr, t, a), a), m.name);
			CHECK_MESSAGE(sameBytes(evaluate(&empty, t, a), a), m.name);
		}
		CHECK(evaluate(nullptr, 0.0f, a).mode == Mode::Fixed);
		CHECK(evaluate(nullptr, 0.0f, a).todWeight == 0.0f);
	}
}

TEST_CASE("LightingState/no sun keys and the look-6 keys equal the pre-T1 inline key code exactly") {
	const SunKeys none{};
	const SunKeys look6{28.0f, std::nullopt, 255.0f, 214.0f, 160.0f, 0.70f};
	const SunKeys yawOnly{std::nullopt, -30.5f, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
	const SunKeys low{10.0f, 100.0f, 17.4f, 18.6f, 19.5f, 1.3f};   // pitch clamp, colour rounding, ambient above 255 scale
	for (const auto& m : kStock) {
		for (const SunKeys* k : {&none, &look6, &yawOnly, &low}) {
			for (bool scaled : {false, true}) {
				const OldKeys o = oldInline(m, *k, scaled);
				const LightingState n = applySunKeys(authoredOf(m), *k, scaled);
				CHECK_MESSAGE(n.pitch == o.pitch, m.name);
				CHECK_MESSAGE(n.yaw == o.yaw, m.name);
				for (int i = 0; i < 3; ++i) {
					CHECK_MESSAGE(n.sunDir[i] == o.dir[i], m.name);
					CHECK_MESSAGE(n.sun[i] == o.sun[i], m.name);
					CHECK_MESSAGE(n.amb[i] == o.amb[i], m.name);
					if (k->ambientScale && !scaled) CHECK_MESSAGE(scaleAmbientByte(m.amb[i], *k->ambientScale) == o.dayAmb[i], m.name);
				}
			}
		}
		CHECK(sameBytes(applySunKeys(authoredOf(m), none, false), authoredOf(m)));   // no keys: the recomputed direction is the authored one
	}
}

TEST_CASE("LightingState/a curve: pitch is monotone between keys and hits the keys, colours lerp in bytes") {
	const TodCurve c = sunCurve();
	const LightingState a = authoredOf(kStock[0]);
	// at the keys
	CHECK(evaluate(&c, 0.0f, a).pitch == 28.0f);
	CHECK(evaluate(&c, 0.5f, a).pitch == 60.0f);
	CHECK(evaluate(&c, 1.0f, a).pitch == 30.0f);
	// no overshoot and monotone inside each segment (rising 28 -> 60, then falling 60 -> 30)
	float prev = 28.0f;
	for (int i = 1; i <= 100; ++i) {
		const float p = evaluate(&c, i * 0.005f, a).pitch;
		CHECK(p >= prev - 1e-4f);
		CHECK(p <= 60.0f + 1e-4f);
		prev = p;
	}
	for (int i = 101; i <= 200; ++i) {
		const float p = evaluate(&c, i * 0.005f, a).pitch;
		CHECK(p <= prev + 1e-4f);
		CHECK(p >= 30.0f - 1e-4f);
		prev = p;
	}
	// colours: linear in the byte domain, rounded to nearest
	const LightingState mid = evaluate(&c, 0.25f, a);
	CHECK(mid.sun[0] == 255);
	CHECK(mid.sun[1] == static_cast<uint8_t>(214 + (255 - 214) * 0.5f + 0.5f));
	CHECK(mid.sun[2] == static_cast<uint8_t>(160 + (225 - 160) * 0.5f + 0.5f));
	CHECK(mid.amb[0] == static_cast<uint8_t>(50 + (71 - 50) * 0.5f + 0.5f));
	CHECK(evaluate(&c, 0.5f, a).sun[1] == 255);
	CHECK(evaluate(&c, 1.0f, a).amb[2] == 32);
	// live mode, the direction follows the evaluated angles, un-keyed channels (fog colour, fog full) stay authored
	CHECK(mid.mode == Mode::Live);
	CHECK(mid.todWeight == 1.0f);
	float dir[3];
	sunDirFromAngles(mid.pitch, mid.yaw, dir);
	for (int i = 0; i < 3; ++i) CHECK(mid.sunDir[i] == dir[i]);
	CHECK(mid.fogColor == a.fogColor);
	CHECK(mid.fogFull == a.fogFull);
	CHECK(mid.fogStart == doctest::Approx(200.0f).epsilon(0.2));   // keyed channel moves
}

TEST_CASE("LightingState/t outside the keyed range clamps to the first and last key") {
	const TodCurve c = sunCurve();
	const LightingState a = authoredOf(kStock[0]);
	CHECK(sameBytes(evaluate(&c, -3.0f, a), evaluate(&c, 0.0f, a)));
	CHECK(sameBytes(evaluate(&c, 7.0f, a), evaluate(&c, 1.0f, a)));
}

TEST_CASE("LightingState/a key without a channel leaves it to the authored value; one key is constant") {
	TodCurve c;
	TodKey k; k.t = 0.4f; k.mask = kSun; k.sun[0] = 10; k.sun[1] = 20; k.sun[2] = 30;
	c.keys = {k};
	const LightingState a = authoredOf(kStock[2]);
	const LightingState s = evaluate(&c, 0.9f, a);
	CHECK(s.sun[0] == 10);
	CHECK(s.pitch == a.pitch);
	CHECK(s.yaw == a.yaw);
	for (int i = 0; i < 3; ++i) { CHECK(s.sunDir[i] == a.sunDir[i]); CHECK(s.amb[i] == a.amb[i]); }
}

TEST_CASE("LightingState/timeOfDay sidecar block parses to a curve and a malformed one leaves the mission Fixed") {
	const auto parsed = parseTodCurve(R"({"start":0.5,"rate":0.01,"keys":[{"t":1.0,"sunPitch":30,"fogColorR":10,"fogColorG":20,"fogColorB":30},
		{"t":0.0,"sunPitch":28,"sunYaw":10,"sunColorR":255,"sunColorG":214,"sunColorB":160,"ambientR":50,"ambientG":50,"ambientB":55}]})");
	REQUIRE(parsed);
	const auto c = *parsed;
	REQUIRE(c);
	CHECK(c->keys.size() == 2);
	CHECK(c->keys[0].t == 0.0f);   // sorted
	CHECK(c->start == 0.5f);
	CHECK(c->rate == 0.01f);
	CHECK(c->keys[0].mask == (kPitch | kYaw | kSun | kAmb));
	CHECK(c->keys[1].fogColor == 0x0a141eu);
	CHECK((parseTodCurve("") && *parseTodCurve("") == nullptr));   // no block: a null curve, not an error
	CHECK_FALSE(parseTodCurve(R"({"keys":[]})"));
	CHECK_FALSE(parseTodCurve(R"({"keys":[{"t":0,"sunColorR":1}]})"));   // partial colour
	CHECK_FALSE(parseTodCurve(R"({"keys":[{"t":0},{"t":0}]})"));          // duplicate time
	CHECK_FALSE(parseTodCurve("not json"));
}

TEST_CASE("LightingState/legacy day-night maths: noon, night, dusk and the curve generator") {
	legacy::Params P;
	const uint8_t day[3] = {255, 255, 225}, sunset[3] = {205, 84, 31}, night[3] = {64, 64, 64}, dayAmb[3] = {71, 71, 78}, nightAmb[3] = {32, 32, 32};
	for (int i = 0; i < 3; ++i) { P.day[i] = day[i]; P.sunset[i] = sunset[i]; P.night[i] = night[i]; P.dayAmb[i] = dayAmb[i]; P.nightAmb[i] = nightAmb[i]; }
	P.dayFogColor = 0xa0a0a5u;
	P.fogTransparency = 0.5f;
	const auto noon = legacy::sample(P, 90.0f, 11.0f, 0.0f);
	CHECK_FALSE(noon.isNight);
	CHECK(noon.nightFactor == 0.0f);
	for (int i = 0; i < 3; ++i) { CHECK(noon.sun[i] == day[i]); CHECK(noon.amb[i] == dayAmb[i]); }
	const auto dark = legacy::sample(P, 270.0f, 11.0f, 0.0f);
	CHECK(dark.isNight);
	CHECK(dark.nightFactor == 1.0f);
	CHECK(dark.pitch == 90.0f);   // the moon mirror: 180 - |180 - 270|
	for (int i = 0; i < 3; ++i) { CHECK(dark.sun[i] == night[i]); CHECK(dark.amb[i] == nightAmb[i]); }
	const auto dusk = legacy::sample(P, 20.0f, 11.0f, 0.0f);
	CHECK_FALSE(dusk.isNight);
	CHECK(dusk.nightFactor == doctest::Approx((25.0f - 20.0f) / 15.0f));
	// the generator: one cycle of keys, t = pitch / 360, key at 0.25 is the noon sample, pitch stays inside each segment's endpoints
	const TodCurve c = legacy::curve(P, 11.0f, 0.0f, 24);
	REQUIRE(c.keys.size() == 25);
	CHECK(c.keys[6].t == 0.25f);
	for (int i = 0; i < 3; ++i) CHECK(c.keys[6].sun[i] == day[i]);
	LightingState a;
	for (int i = 1; i < 50; ++i) {
		const float t = i / 50.0f;
		const float p = evaluate(&c, t, a).pitch;
		CHECK(p >= -1e-3f);
		CHECK(p <= 180.0f + 1e-3f);   // day half: the pitch itself (0..180); the night half mirrors back down to <= 90
	}
}
