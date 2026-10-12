// mclib/script_music.h
// The music state an ABL script sees: the tune it last requested and when that tune ends on the scenario clock.
//
// WHY: the sound system's own idea of "playing" comes from the audio device (gosAudio_GetChannelPlayMode), which is
// wall-clock state. Every stock mission script branches on getCurrentMusicId == -1 and then draws from the simulation
// RNG (`if (random(100) < 1) playDigitalMusic(...)`), so a tune that ended at a different wall moment in two runs of the
// same mission forked the simulation (mc2_16, 2026-10-12). Ruling (orchestrator default, owner to confirm): the builtin
// reports what the SCRIPT asked for, and end-of-track is the scenario clock plus the track length read from the file.
// Pure (no GameOS, no device): the doctest in tests/unit/test_script_music.cpp pins the semantics.
#pragma once

#include <cstddef>
#include <cstdint>

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
// 0 when the bytes are not a WAVE header whose fmt and data chunks both lie within `n` (the data payload need not).
inline double wavDurationSeconds(const unsigned char* bytes, std::size_t n)
{
	auto u32 = [bytes](std::size_t at) -> std::uint32_t {
		return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8) | (std::uint32_t(bytes[at + 2]) << 16)
			| (std::uint32_t(bytes[at + 3]) << 24);
	};
	auto tag = [bytes](std::size_t at, const char* four) {
		return bytes[at] == (unsigned char)four[0] && bytes[at + 1] == (unsigned char)four[1]
			&& bytes[at + 2] == (unsigned char)four[2] && bytes[at + 3] == (unsigned char)four[3];
	};
	if (!bytes || n < 12 || !tag(0, "RIFF") || !tag(8, "WAVE"))
		return 0.0;
	std::uint32_t byteRate = 0;
	std::size_t at = 12;
	while (at + 8 <= n)
	{
		const std::uint32_t length = u32(at + 4);
		if (tag(at, "fmt "))
		{
			if (length < 16 || at + 8 + 16 > n)
				return 0.0;
			byteRate = u32(at + 8 + 8);		// MC2_WAVEFORMATEX::nAvgBytesPerSec
		}
		else if (tag(at, "data"))
		{
			return byteRate ? double(length) / double(byteRate) : 0.0;
		}
		at += 8 + std::size_t(length) + (length & 1u);	// RIFF chunks are word aligned
	}
	return 0.0;
}
