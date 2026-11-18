// mclib/script_music.h
// The music state an ABL script sees: the tune it last requested and when that tune ends on the scenario clock.
//
// WHY: the sound system's own idea of "playing" comes from the audio device (gosAudio_GetChannelPlayMode), which is
// wall-clock state. Every stock mission script branches on getCurrentMusicId == -1 and then draws from the simulation
// RNG (`if (random(100) < 1) playDigitalMusic(...)`), so a tune that ended at a different wall moment in two runs of the
// same mission forked the simulation (mc2_16). Rule: the builtin
// reports what the SCRIPT asked for, and end-of-track is the scenario clock plus the track length read from the file.
// Pure (no GameOS, no device): the doctest in tests/unit/test_script_music.cpp pins the semantics.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>

struct WaveSampleView
{
	std::span<const unsigned char> format;
	std::span<const unsigned char> samples;
};

// Playback needs complete chunks, unlike the header-only duration query below.
// Bound every read by both the packet and the enclosing RIFF chunk.
inline std::optional<WaveSampleView> parseWaveSamples(std::span<const unsigned char> bytes)
{
	if (bytes.size() < 12)
		return std::nullopt;
	auto u32 = [bytes](std::size_t at) -> std::uint32_t {
		return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8)
			| (std::uint32_t{bytes[at + 2]} << 16) | (std::uint32_t{bytes[at + 3]} << 24);
	};
	if (u32(0) != 0x46464952 || u32(8) != 0x45564157)
		return std::nullopt;
	const std::size_t riffSize = u32(4);
	if (riffSize < 4 || riffSize > bytes.size() - 8)
		return std::nullopt;
	const std::size_t end = 8 + riffSize;
	WaveSampleView found;
	bool haveData = false;
	std::size_t at = 12;
	while (end - at >= 8)
	{
		const auto tag = u32(at);
		const std::size_t length = u32(at + 4);
		at += 8;
		if (length > end - at)
			return std::nullopt;
		if (tag == 0x20746d66 && found.format.empty())
		{
			// The engine consumes the first 14 bytes of WAVEFORMAT.
			if (length < 14)
				return std::nullopt;
			found.format = bytes.subspan(at, length);
		}
		else if (tag == 0x61746164 && !haveData)
		{
			found.samples = bytes.subspan(at, length);
			haveData = true;
		}
		if (!found.format.empty() && haveData)
			return found;
		at += length;
		if (length & 1u)
		{
			if (at == end)
				return std::nullopt;
			++at;
		}
	}
	return std::nullopt;
}

struct ScriptMusic
{
	long   id     = -1;		// the id the script last requested; -1 when it asked for none
	double endsAt = 0.0;		// scenario-clock second at which a one-shot tune ends
	bool   loops  = false;		// a looping tune ends only when the script stops it or asks for another

	void request(long musicId, double now, double seconds, bool loop)
	{
		id = musicId;
		endsAt = now + seconds;
		loops = loop;
	}

	void stop() { id = -1; }

	// What getCurrentMusicId returns at scenario time `now`.
	long currentId(double now) const
	{
		return (id >= 0 && (loops || now < endsAt)) ? id : -1;
	}
};

// Seconds of audio described by a RIFF/WAVE header: the data chunk length over the fmt chunk's average byte rate.
// 0 when the bytes are not a WAVE header whose fmt and data chunk headers both lie within `bytes` (the payload need not).
inline double wavDurationSeconds(std::span<const unsigned char> bytes)
{
	const std::size_t n = bytes.size();
	auto u32 = [bytes](std::size_t at) -> std::uint32_t {
		return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8) | (std::uint32_t{bytes[at + 2]} << 16)
			| (std::uint32_t{bytes[at + 3]} << 24);
	};
	auto tag = [bytes](std::size_t at, std::span<const char, 4> four) {
		for (std::size_t i = 0; i < 4; ++i)
			if (bytes[at + i] != static_cast<unsigned char>(four[i]))
				return false;
		return true;
	};
	using Tag = std::span<const char, 4>;
	if (n < 12 || !tag(0, Tag{"RIFF", 4}) || !tag(8, Tag{"WAVE", 4}))
		return 0.0;
	std::uint32_t byteRate = 0;
	std::size_t at = 12;
	while (at + 8 <= n)
	{
		const std::uint32_t length = u32(at + 4);
		if (tag(at, Tag{"fmt ", 4}))
		{
			if (length < 16 || at + 8 + 16 > n)
				return 0.0;
			byteRate = u32(at + 8 + 8);		// MC2_WAVEFORMATEX::nAvgBytesPerSec
		}
		else if (tag(at, Tag{"data", 4}))
		{
			return byteRate ? static_cast<double>(length) / static_cast<double>(byteRate) : 0.0;
		}
		at += 8 + std::size_t{length} + (length & 1u);	// RIFF chunks are word aligned
	}
	return 0.0;
}
