#include "doctest.h"
#include "../../mclib/lighting_legacy.h"
#include "../../mclib/lighting_state.h"
#include "../../mclib/lighting_tod_json.h"
#include <map>
#include "../../code/sim_tick_clock.h"
#include <string>
#include <cstring>

// Trigger paths: mclib/lighting_state.h, mclib/lighting_legacy.h, mclib/lighting_tod_json.h, mclib/camera.cpp (sun keys),
// mclib/camera_daylight.cpp, code/gamecam.cpp (renderTerrainSceneState).
using namespace mc2::lighting;

TEST_CASE("LightingState/clock: sim ticks, pause, fixed path and member publication") {
	TodCurve curve; curve.keys = {{0.0f}, {1.0f}};
	double t = 0.5;
	int writes = 0;
	float memberPitch = 0.0f;
	curve.keys[0].mask = curve.keys[1].mask = kPitch;
	curve.keys[0].pitch = 20.0f; curve.keys[1].pitch = 80.0f;
	auto publish = [&] { ++writes; memberPitch = compose(&curve, t, {}, {}).pitch; };
	mc2::sim::TickClock clock;
	for (int frame = 0; frame < 300; ++frame)
		for (int i = clock.advance(1.0 / 30.0, 1.0); i > 0; --i) tickTod(&curve, t, curve.rate, publish);
	CHECK(t == doctest::Approx(0.5 + 10.0 / 86400.0).epsilon(1e-9));
	CHECK(writes == 300);
	CHECK(memberPitch == compose(&curve, t, {}, {}).pitch);
	CHECK(memberPitch > 50.0f);
	const double paused = t;
	for (int i = clock.advance(1.0 / 30.0, 0.0); i > 0; --i) tickTod(&curve, t, curve.rate, publish);
	CHECK(t == paused);
	tickTod(nullptr, t, 1.0f, publish);
	tickTod(&curve, t, 0.0f, publish);
	CHECK(t == paused); CHECK(writes == 300);
	tickTod(&curve, t, 60.0f, publish);
	CHECK(t == 1.0); // curve endpoints hold, never jump back to dawn
}

TEST_CASE("LightingState/save: matching hash, mismatch, old save and stock bytes") {
	const auto parsed = parseTodCurve(R"({"start":0.28,"keys":[{"t":0},{"t":1}]})");
	REQUIRE((parsed && *parsed));
	const auto& c = **parsed;
	CHECK(c.rate == 1.0f / 86400.0f);
	struct Save {
		std::map<std::string, double> keys;
		std::string bytes = "[Cameras]\n";
		void writeIdFloat(const char* k, float v) { keys[k] = v; bytes += std::string(k) + "=" + std::to_string(v) + "\n"; }
		void writeIdULong(const char* k, uint32_t v) { keys[k] = v; bytes += std::string(k) + "=" + std::to_string(v) + "\n"; }
	} stock, live;
	const std::string before = stock.bytes;
	saveTod(stock, nullptr, 0.45, c.rate);
	CHECK(stock.bytes == before); CHECK(stock.keys.empty());
	saveTod(live, &c, 0.45f, c.rate);
	CHECK(live.keys.size() == 3);
	CHECK(restoreTod(c, uint32_t(live.keys.at("TodCurveHash")), float(live.keys.at("TimeOfDay"))) == 0.45f);
	CHECK(live.keys.at("TimeOfDayRate") == c.rate);
	CHECK(restoreTod(c, c.hash ^ 1u, 0.45f) == c.start);
	CHECK(restoreTod(c, 0, 0.0f) == c.start);
	const auto changed = parseTodCurve(R"({"start":0.29,"keys":[{"t":0},{"t":1}]})");
	REQUIRE((changed && *changed)); CHECK((*changed)->hash != c.hash);
}

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

// ---- Follow-ups (T1b) ----

// Reference values captured by running the OLD Camera::updateDaylight (805c7e2c1, verbatim, in a scratch program) on Day 255,255,225 /
// Sunset 205,84,31 / Night 64,64,64 / DayAmbient 71,71,78 / NightAmbient 32,32,32 / dayFog 0xa0a0a5 / fogTransparency 0.5, 3600 s day.
struct OldRef { float pitchIn, yaw, camRot, pitch; uint8_t sun[3], amb[3]; uint32_t fog; int night; float nf; };
constexpr OldRef kOld[] = {
	{5,11,0,5,{223,141,66},{35,35,36},0x262626,1,1},
	{5,11,100,5,{223,141,66},{35,35,36},0x665543,1,1},
	{5,-206,100,5,{223,141,66},{35,35,36},0x191b1b,1,1},
	{12,11,0,12,{227,159,80},{40,40,41},0x404141,0,0.866667},
	{12,11,100,12,{227,159,80},{40,40,41},0x6f6c5f,0,0.866667},
	{20,-206,100,20,{235,192,112},{45,45,47},0x545e5e,0,0.333333},
	{30,11,0,30,{247,238,180},{51,51,55},0x7e8a8a,0,0},
	{30,11,100,30,{247,238,180},{51,51,55},0x888e8c,0,0},
	{60,11,0,60,{255,255,225},{65,65,71},0xa5b8be,0,0},
	{60,11,100,60,{255,255,225},{65,65,71},0xa5b8be,0,0},
	{160,-206,100,160,{235,192,112},{45,45,47},0x858783,0,0.333333},
	{175,11,0,175,{223,141,66},{35,35,36},0x7b5f47,1,1},
	{175,11,100,175,{223,141,66},{35,35,36},0x2d2d2d,1,1},
	{200,11,0,160,{64,64,64},{32,32,32},0x000000,1,1},
	{200,11,100,160,{64,64,64},{32,32,32},0x000000,1,1},
	{270,-206,100,90,{64,64,64},{32,32,32},0x000000,1,1},
};

TEST_CASE("LightingState/legacy::sample equals the old updateDaylight on captured reference values") {
	legacy::Params P;
	const uint8_t day[3] = {255, 255, 225}, sunset[3] = {205, 84, 31}, night[3] = {64, 64, 64}, dayAmb[3] = {71, 71, 78}, nightAmb[3] = {32, 32, 32};
	for (int i = 0; i < 3; ++i) { P.day[i] = day[i]; P.sunset[i] = sunset[i]; P.night[i] = night[i]; P.dayAmb[i] = dayAmb[i]; P.nightAmb[i] = nightAmb[i]; }
	P.dayFogColor = 0xa0a0a5u;
	P.fogTransparency = 0.5f;
	for (const auto& r : kOld) {
		const float time = r.pitchIn / 360.0f * 3600.0f;   // the old code derived the pitch from dayLightTime this way
		const auto g = legacy::sample(P, time * (1.0f / 3600.0f) * 360.0f, r.yaw, r.camRot);
		CHECK_MESSAGE(g.pitch == doctest::Approx(r.pitch).epsilon(1e-5), r.pitchIn);
		CHECK_MESSAGE(g.isNight == (r.night != 0), r.pitchIn);
		CHECK_MESSAGE(g.nightFactor == doctest::Approx(r.nf).epsilon(1e-4), r.pitchIn);
		CHECK_MESSAGE(g.fogColor == r.fog, r.pitchIn);
		for (int i = 0; i < 3; ++i) { CHECK_MESSAGE(g.sun[i] == r.sun[i], r.pitchIn); CHECK_MESSAGE(g.amb[i] == r.amb[i], r.pitchIn); }
	}
}

TEST_CASE("LightingState/camera wiring: sun-key pitch, ambient scaling, curve start write-back, repeat evaluation") {
	const Stock& m = kStock[0];
	const SunKeys keys{28.0f, std::nullopt, 255.0f, 214.0f, 160.0f, 0.70f};   // the look-6 keys
	const LightingState keyed = applySunKeys(authoredOf(m), keys, false);
	CHECK(keyed.pitch == 28.0f);
	CHECK(keyed.amb[0] == scaleAmbientByte(m.amb[0], 0.70f));
	CHECK(applySunKeys(authoredOf(m), keys, true).amb[0] == m.amb[0]);   // a save holds scaled bytes: no second scale
	// a curve over the keyed base: an unkeyed channel keeps the keyed (authored) value, a keyed one follows the curve
	TodCurve c = sunCurve();
	for (auto& k : c.keys) k.mask = kPitch;
	c.start = 0.5f;
	const LightingState start = compose(&c, c.start, keyed, {});
	CHECK(start.pitch == 60.0f);
	for (int i = 0; i < 3; ++i) { CHECK(start.sun[i] == keyed.sun[i]); CHECK(start.amb[i] == keyed.amb[i]); }
	// the camera writes the start state back into day* and keeps evaluating from the base: the same state, every time
	CHECK(sameBytes(start, compose(&c, c.start, keyed, {})));
	CHECK(sameBytes(compose(&c, c.start, start, {}), start));   // base == the written-back state is a fixed point
	CHECK(&bakeReference(keyed) == &keyed);
}

TEST_CASE("LightingState/modulators compose once: rain, KillAmbientLight and the sun override, with and without a curve") {
	const LightingState base = authoredOf(kStock[0]);
	LightMods rain; rain.rainDivisor = 1.7f;
	LightMods kill; kill.killAmbient = true;
	LightMods sun; sun.sunPitch = 40.0f; sun.sunYaw = 5.0f;
	// no curve: the old weather expression (uchar / float, truncated), kill = white ambient, override = the new angles
	const LightingState r0 = modulate(base, rain);
	for (int i = 0; i < 3; ++i) { CHECK(r0.sun[i] == static_cast<uint8_t>(base.sun[i] / 1.7f)); CHECK(r0.amb[i] == static_cast<uint8_t>(base.amb[i] / 1.7f)); }
	CHECK(modulate(base, {}) == base);
	CHECK(modulate(base, kill).amb[1] == 255);
	CHECK(modulate(base, kill).sun[1] == base.sun[1]);
	CHECK(modulate(base, sun).pitch == 40.0f);
	// a curve: the modulators sit ON the curve's base; the curve cannot override them
	const TodCurve c = sunCurve();
	const LightingState live = evaluate(&c, 0.25f, base);
	const LightingState r1 = compose(&c, 0.25f, base, rain);
	for (int i = 0; i < 3; ++i) { CHECK(r1.sun[i] == static_cast<uint8_t>(live.sun[i] / 1.7f)); CHECK(r1.amb[i] == static_cast<uint8_t>(live.amb[i] / 1.7f)); }
	CHECK(compose(&c, 0.25f, base, kill).amb[0] == 255);
	CHECK(compose(&c, 0.25f, base, sun).pitch == 40.0f);   // the curve says about 44 here; the override wins
	CHECK(compose(&c, 0.25f, base, rain) == r1);           // pure: evaluating again gives the same state
	// the flat light of a state is the camera's getLightRed arithmetic on its bytes
	CHECK(flatLightByte(255, 71, 0.5f, 0) == 198);
	CHECK(flatLightByte(255, 71, 0.0f, 0) == 71);
	CHECK(flatLightByte(255, 71, 0.0f, 90) == 90);
	CHECK(flatLightByte(255, 200, 1.0f, 0) == 255);
}

TEST_CASE("LightingState/parser hardening: key cap, non-finite, float range, negative rate, pitch clamp") {
	std::string many = R"({"keys":[)";
	for (int i = 0; i < 70; ++i) many += std::string(i ? "," : "") + R"({"t":)" + std::to_string(i) + R"(,"sunPitch":30})";
	many += "]}";
	const auto big = parseTodCurve(many);
	REQUIRE((big && *big));
	CHECK((*big)->keys.size() == kMaxTodKeys);
	CHECK((*big)->droppedKeys == 70 - kMaxTodKeys);
	CHECK_FALSE(parseTodCurve(R"({"keys":[{"t":0,"sunPitch":1e300}]})"));    // outside float range
	CHECK_FALSE(parseTodCurve(R"({"keys":[{"t":0,"fogStart":1e999}]})"));    // not finite / not parseable
	CHECK_FALSE(parseTodCurve(R"({"start":1e50,"keys":[{"t":0}]})"));
	CHECK_FALSE(parseTodCurve(R"({"rate":-0.5,"keys":[{"t":0}]})"));         // negative rate
	const auto ok = parseTodCurve(R"({"rate":0,"keys":[{"t":0,"sunPitch":-40},{"t":1,"sunPitch":120}]})");
	REQUIRE((ok && *ok));
	CHECK((*ok)->keys[0].pitch == kMinCurvePitch);   // clamped to [-10, 89]: dawn and dusk need a low sun
	CHECK((*ok)->keys[1].pitch == kMaxCurvePitch);
	CHECK((*ok)->droppedKeys == 0);
}

// Execute the production Camera methods through the existing source-slice seam;
// only the world-light sink and FIT storage are device-free substitutes.
#include <glm/glm.hpp>
#include <algorithm>
#include <sstream>
namespace camera_lighting_test {
constexpr int NO_ERR = 0;
float frameLength=1.0f/30.0f;
using MemoryPtr = unsigned char*;
struct FitIniFile {
    std::map<std::string, double> values;
    long readIdFloat(const char* key, float& v) { auto i=values.find(key); if(i==values.end()) return 1; v=float(i->second); return 0; }
    long readIdULong(const char* key, unsigned long& v) { auto i=values.find(key); if(i==values.end()) return 1; v=static_cast<unsigned long>(i->second); return 0; }
    long readIdUCharArray(const char* key, uint8_t* v, int n) { for(int i=0;i<n;++i) v[i]=uint8_t(values.at(std::string(key)+char('0'+i))); return 0; }
    void writeIdUCharArray(const char* key, uint8_t* v, int n) { for(int i=0;i<n;++i) values[std::string(key)+char('0'+i)]=v[i]; }
    void writeIdULong(const char* key, uint32_t v) { values[key]=v; }
    void writeIdUChar(const char* key, uint8_t v) { values[key]=v; }
    void write(MemoryPtr p, size_t n) { std::istringstream in(std::string(reinterpret_cast<char*>(p),n)); std::string type,key,eq; double v; in>>type>>key>>eq>>v; values[key]=v; }
};
namespace mc2 { namespace lighting = ::mc2::lighting;
namespace fmt { template<class... A> void to_buf(char (&line)[128], const char*, const char* key, float v) { std::snprintf(line,128,"f %s = %.9g\r\n",key,double(v)); } }
namespace options { struct Options { bool sunElevationFix=true; }; inline const Options& current() { static Options o; return o; } }
namespace math { struct Euler { Euler(float,float,float) {} }; inline void buildTranslation(glm::mat4&,glm::vec3) {} inline void buildRotation(glm::mat4&,Euler) {} }
}
struct Light { void SetLightToWorld(glm::mat4*) {} };
struct Camera {
    glm::vec3 lightDirection{};
    float lightPitch=50,lightYaw=11,fogStart=-1,fogFull=-2;
    uint8_t lightRed=240,lightGreen=200,lightBlue=160,ambientRed=80,ambientGreen=60,ambientBlue=40;
    uint8_t dayLightRed=240,dayLightGreen=200,dayLightBlue=160,dayAmbientRed=80,dayAmbientGreen=60,dayAmbientBlue=40;
    uint32_t fogColor=0xffe1ded4;
    uint32_t dayFogColor=0xffe1ded4;
    uint8_t sunsetLightRed=120,sunsetLightGreen=80,sunsetLightBlue=40,nightLightRed=64,nightLightGreen=64,nightLightBlue=64;
    uint8_t nightAmbientRed=32,nightAmbientGreen=32,nightAmbientBlue=32;
    float day2NightTransitionTime=-1,dayLightTime=0,dayLightPitch=50,nightFactor=0,fogTransparency=0;
    bool isNight=false;
    Light** worldLights=nullptr;
    uint8_t killSavedAmbient[3]{};
    std::shared_ptr<const TodCurve> todCurve;
    LightingState todBase,bakeSnapshot;
    LightMods lightMods;
    double todT=0; float todRate=0;
    LightingState lightMembers() const; LightingState currentLighting() const; LightingState bakeReferenceLighting() const;
    void captureBakeReference(FitIniFile*); void saveBakeReference(FitIniFile*) const;
    void restoreSunOverride(FitIniFile*); void saveSunOverride(FitIniFile*) const;
    void saveColors(FitIniFile* file) const {
#include "camera_save_colors.inc"
    }
    void publishLight(); void setRainLight(float); void clearRainLight(); void setKillAmbient(bool); void advanceTimeOfDay();
    static glm::vec3 sunDirectionFor(float,float);
    void updateDaylight(bool); float setDaylightFraction(float); float setSunPitch(float,bool=false,float=0);
    float getCameraRotation() const { return 0; }
};
#include "camera_sun_direction.inc"
#include "camera_lighting.inc"
#include "camera_daylight.inc"
}

TEST_CASE("LightingState/Camera bake snapshot survives rain KillAmbient moved sun and save restore") {
    using namespace camera_lighting_test;
    Camera c;
    c.lightDirection=c.sunDirectionFor(c.lightPitch,c.lightYaw);
    c.captureBakeReference(nullptr);
    const auto loaded=c.lightMembers();
    c.setRainLight(2); c.setKillAmbient(true); c.lightPitch=15; c.lightYaw=90;
    c.lightDirection=c.sunDirectionFor(15,90);
    CHECK(c.currentLighting()!=loaded);
    CHECK(c.bakeReferenceLighting()==loaded);
    FitIniFile save; c.saveBakeReference(&save);
    CHECK(save.values.count("BakePitch")==1);
    Camera restored; restored.lightPitch=15; restored.captureBakeReference(&save);
    CHECK(restored.bakeReferenceLighting()==loaded);
    Camera stock; stock.captureBakeReference(nullptr); FitIniFile unchanged; stock.saveBakeReference(&unchanged);
    CHECK(unchanged.values.empty());
}

TEST_CASE("LightingState/Camera curve save keeps the undimmed base and optional sun override") {
    using namespace camera_lighting_test;
    Camera c; c.todBase=c.lightMembers();
    c.todCurve=std::make_shared<TodCurve>(TodCurve{{{0}, {1}}});
    c.lightMods.sunPitch=31.234567f; c.lightMods.sunYaw=-62.345678f;
    c.setRainLight(2);
    const auto before=c.currentLighting();
    FitIniFile save; c.saveColors(&save); c.saveSunOverride(&save);
    CHECK(save.values.at("LightRed")==240); CHECK(save.values.at("AmbientRed")==80);
    Camera restored; restored.todCurve=c.todCurve; restored.todBase=c.todBase;
    const char* sun[]={"LightRed","LightGreen","LightBlue"};
    const char* amb[]={"AmbientRed","AmbientGreen","AmbientBlue"};
    for(int i=0;i<3;++i) { restored.todBase.sun[i]=uint8_t(save.values.at(sun[i])); restored.todBase.amb[i]=uint8_t(save.values.at(amb[i])); }
    restored.restoreSunOverride(&save); restored.setRainLight(2);
    CHECK(restored.currentLighting()==before);
    CHECK(restored.lightMods.sunPitch==c.lightMods.sunPitch); CHECK(restored.lightMods.sunYaw==c.lightMods.sunYaw);
    Camera stock; FitIniFile unchanged; stock.saveSunOverride(&unchanged); CHECK(unchanged.values.empty());
}

TEST_CASE("LightingState/Camera readers use published curve members until the next publication") {
    using namespace camera_lighting_test;
    Camera c; c.todBase=c.lightMembers();
    const auto curve=parseTodCurve(R"({"keys":[{"t":0,"sunPitch":20},{"t":1,"sunPitch":80}]})");
    REQUIRE((curve && *curve)); c.todCurve=*curve;
    c.publishLight(); CHECK(c.lightPitch==20);
    const auto cached=c.currentLighting();
    c.todT=1;
    CHECK(c.currentLighting()==cached); CHECK(c.lightPitch==20);
    c.publishLight(); CHECK(c.currentLighting().pitch==80); CHECK(c.lightPitch==80);
    c.setKillAmbient(true); c.setRainLight(2);
    CHECK(c.currentLighting().amb[0]==255);
    c.setKillAmbient(false); CHECK(c.currentLighting().amb[0]==40);
    Camera stock; CHECK(stock.currentLighting()==stock.lightMembers());
}

TEST_CASE("LightingState/Camera curve excludes legacy daylight writes and initialization") {
    using namespace camera_lighting_test;
    Camera c; c.todBase=c.lightMembers(); c.todCurve=std::make_shared<TodCurve>(TodCurve{{{0},{1}}});
    c.day2NightTransitionTime=3600; c.dayLightTime=900; c.publishLight();
    const auto before=c.currentLighting();
    c.updateDaylight(false); c.updateDaylight(true);
    CHECK(c.currentLighting()==before); CHECK(c.dayLightTime==900);
    c.todCurve.reset(); Light light; Light* lights[]={&light}; c.worldLights=lights;
    c.updateDaylight(false);
    CHECK(c.lightPitch==90); CHECK(c.dayLightTime>900);
}
