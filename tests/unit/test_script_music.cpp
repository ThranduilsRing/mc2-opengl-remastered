// tests/unit/test_script_music.cpp
// The music state an ABL script sees (mclib/script_music.h): the script's own request on the scenario clock, never the device.
#include "doctest.h"
#include "mclib/script_music.h"

#include <cstring>
#include <algorithm>
#include <span>
#include <vector>

namespace {

// A RIFF/WAVE header with the given byte rate and data length; `before` is appended as an extra chunk ahead of `data`.
std::vector<unsigned char> wavHeader(unsigned byteRate, unsigned dataLength, unsigned extraChunkLength = 0)
{
	std::vector<unsigned char> b;
	auto u32 = [&b](unsigned v) { for (int i = 0; i < 4; ++i) b.push_back((unsigned char)((v >> (8 * i)) & 0xff)); };
	auto u16 = [&b](unsigned v) { for (int i = 0; i < 2; ++i) b.push_back((unsigned char)((v >> (8 * i)) & 0xff)); };
	auto tag = [&b](const char* t) { b.insert(b.end(), t, t + 4); };
	tag("RIFF"); u32(36 + dataLength); tag("WAVE");
	tag("fmt "); u32(16); u16(1); u16(2); u32(44100); u32(byteRate); u16(4); u16(16);
	if (extraChunkLength)
	{
		tag("LIST"); u32(extraChunkLength);
		b.insert(b.end(), extraChunkLength + (extraChunkLength & 1u), (unsigned char)0);
	}
	tag("data"); u32(dataLength);
	return b;
}

}  // namespace

TEST_CASE("ScriptMusic: a one-shot tune is current until its length has elapsed on the scenario clock")
{
	ScriptMusic m;
	CHECK(m.currentId(0.0) == -1);
	m.request(7, 100.0, 12.5, false);
	CHECK(m.currentId(100.0) == 7);
	CHECK(m.currentId(112.4) == 7);
	CHECK(m.currentId(112.5) == -1);
	CHECK(m.currentId(500.0) == -1);
}

TEST_CASE("ScriptMusic: a looping tune ends only by stop or by another request")
{
	ScriptMusic m;
	m.request(3, 0.0, 10.0, true);
	CHECK(m.currentId(1e6) == 3);
	m.request(4, 1e6, 1.0, false);
	CHECK(m.currentId(1e6) == 4);
	CHECK(m.currentId(1e6 + 1.0) == -1);
	m.request(3, 0.0, 10.0, true);
	m.stop();
	CHECK(m.currentId(0.0) == -1);
}

TEST_CASE("ScriptMusic: a crossfade request replaces the view at once, the old tune's length no longer matters")
{
	ScriptMusic m;
	m.request(7, 0.0, 100.0, false);
	CHECK(m.currentId(50.0) == 7);
	m.request(9, 50.0, 10.0, false);		// the second request starts while the first still plays
	CHECK(m.currentId(50.0) == 9);
	CHECK(m.currentId(59.9) == 9);
	CHECK(m.currentId(60.0) == -1);
}

TEST_CASE("wavDurationSeconds: data length over the fmt byte rate, chunks before data are skipped")
{
	const auto plain = wavHeader(176400, 1764000);
	CHECK(wavDurationSeconds(plain) == doctest::Approx(10.0));
	const auto list = wavHeader(176400, 882000, 33);			// odd-length chunk: word aligned
	CHECK(wavDurationSeconds(list) == doctest::Approx(5.0));
	// The payload need not be present; only the headers are read.
	CHECK(wavDurationSeconds(plain) == wavDurationSeconds(std::span<const unsigned char>(plain).first(44)));
}

TEST_CASE("wavDurationSeconds: anything that is not a readable WAVE header is 0")
{
	const auto plain = wavHeader(176400, 1764000);
	const std::span<const unsigned char> all(plain);
	CHECK(wavDurationSeconds({}) == 0.0);
	CHECK(wavDurationSeconds(all.first(11)) == 0.0);
	CHECK(wavDurationSeconds(all.first(30)) == 0.0);		// fmt present, data chunk header cut off
	std::vector<unsigned char> notRiff(plain);
	std::memcpy(notRiff.data(), "RIFX", 4);
	CHECK(wavDurationSeconds(notRiff) == 0.0);
	const auto zeroRate = wavHeader(0, 1764000);
	CHECK(wavDurationSeconds(zeroRate) == 0.0);
}

TEST_CASE("WaveSamples: bounded playback chunks preserve sample offsets")
{
	for (unsigned extra : {0u, 1u, 32u, 33u})
	{
		auto bytes = wavHeader(176400, 4, extra);
		bytes.insert(bytes.end(), {1, 2, 3, 4});
		const unsigned riffSize = static_cast<unsigned>(bytes.size() - 8);
		for (int i = 0; i < 4; ++i)
			bytes[4 + i] = static_cast<unsigned char>(riffSize >> (8 * i));
		const auto view = parseWaveSamples(bytes);
		REQUIRE(view.has_value());
		CHECK(view->format.data() == bytes.data() + 20);
		CHECK(view->samples.data() == bytes.data() + bytes.size() - 4);
		CHECK(view->samples.size() == 4);
		for (std::size_t n = 0; n < bytes.size(); ++n)
		{
			CHECK_FALSE(parseWaveSamples(std::span<const unsigned char>(bytes).first(n)).has_value());
		}
		// The packet may have bytes following the RIFF container.
		bytes.push_back(99);
		CHECK(parseWaveSamples(bytes).has_value());
	}
}

TEST_CASE("WaveSamples: reject hostile chunk sizes and missing format")
{
	auto bytes = wavHeader(176400, 4);
	bytes.insert(bytes.end(), {1, 2, 3, 4});
	bytes[4] = 40;
	REQUIRE(parseWaveSamples(bytes).has_value());
	for (std::size_t field : {std::size_t{4}, std::size_t{16}, std::size_t{40}})
	{
		auto bad = bytes;
		std::memset(bad.data() + field, 0xff, 4);
		CHECK_FALSE(parseWaveSamples(bad).has_value());
	}
	bytes[4] = 3;
	CHECK_FALSE(parseWaveSamples(bytes).has_value());
	bytes[4] = 40;
	bytes[16] = 13;
	CHECK_FALSE(parseWaveSamples(bytes).has_value());
	bytes[16] = 16;
	std::memcpy(bytes.data() + 12, "JUNK", 4);
	CHECK_FALSE(parseWaveSamples(bytes).has_value());
	std::memcpy(bytes.data() + 12, "fmt ", 4);
	// RIFF permits data before fmt; return only after finding both.
	std::rotate(bytes.begin() + 12, bytes.begin() + 36, bytes.end());
	const auto view = parseWaveSamples(bytes);
	REQUIRE(view.has_value());
	CHECK(view->samples.data() == bytes.data() + 20);
	CHECK(view->format.data() == bytes.data() + 32);
}
