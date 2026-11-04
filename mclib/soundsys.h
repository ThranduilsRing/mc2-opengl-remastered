//---------------------------------------------------------------------------
// soundsys.h - This file is the sound system header
//
// MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef SOUNDSYS_H
#define SOUNDSYS_H
#include <optional>
//---------------------------------------------------------------------------
// Include files
#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#ifndef PACKET_H
#include"packet.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#include<string>
#include<vector>

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#include<math.h>
#include<cstdint>
#include<string_view>
#include"script_music.h"
#include<gameos.hpp>
#include<stuff/stuff.hpp>
//---------------------------------------------------------------------------
// Macro Defintiions
// Channels 0 through 10 are Main Sound Effect Channels
// Channels 11 through 12 are ambient sound channels
// Channel 13 is Support Officer
// Channel 14 is Betty
// Channel 15 is Fire Burning SFX
inline constexpr int MAX_DIGITAL_SAMPLES = 16;

inline constexpr int MAX_SAMPLE_VOLUME = 127;

inline constexpr int RADIO_CHANNEL = 0;
inline constexpr int FIRE_CHANNEL = (MAX_DIGITAL_SAMPLES-1);
inline constexpr int BETTY_CHANNEL = (FIRE_CHANNEL-1);
inline constexpr int SUPPORT_CHANNEL = (BETTY_CHANNEL-1);

inline constexpr int AMBIENT_CHANNEL_START = 11;

inline constexpr int STREAM1CHANNEL = 16;
inline constexpr int STREAM2CHANNEL = 17;
inline constexpr int STREAM3CHANNEL = 18;		//Used for in-Mission voiceovers

inline constexpr int MAX_QUEUED_MESSAGES = 8;
//---------------------------------------------------------------------------
// Classes
#pragma pack(1)
struct MC2_WAVEFORMATEX {     
	WORD  wFormatTag;     
	WORD  nChannels; 
    DWORD nSamplesPerSec;     
	DWORD nAvgBytesPerSec;     
	WORD  nBlockAlign; 
};
#pragma pack()

//---------------------------------------------------------------------------
struct _SoundBite {
	unsigned long		priority = 0;
	unsigned long		cacheStatus = 0;
	unsigned long		soundId = 0;
	std::vector<uint8_t>	biteData;
	float				volume = 0.0f;
	HGOSAUDIO			resourceHandle = nullptr;
};
using SoundBite = _SoundBite;

// One digital music entry of the sound setup file.
struct DigitalMusicEntry {
	std::string			id;
	bool				loop = false;
	float				volume = 1.0f;
};

//---------------------------------------------------------------------------
// The fade state of one music crossfade slot, and the per-frame rule that advances it. Pure: no device and no engine state, so
// a crossfade can be replayed frame by frame in a unit test (tests/unit/test_music_crossfade.cpp).
struct MusicFade {
	float	time = 0.0f;		// signed: > 0 fading up, < 0 fading down, 0 steady
	bool	active = false;
};

enum class MusicFadeStep {
	Steady,			// nothing to do this frame
	FadingUp,		// set the channel volume to `level`
	FadingDown,		// set the channel volume to `level`
	Ended			// the fade-down finished: destroy the stream and free the slot
};

// Advances one slot's fade by one frame. `level` is the volume multiplier to apply for FadingUp/FadingDown and is untouched
// otherwise.
inline MusicFadeStep advanceMusicFade (MusicFade& fade, float frameLength, float fadeDownTime, float& level)
{
	if (!fade.active || (fade.time == 0.0f))
		return MusicFadeStep::Steady;

	if (fade.time < 0.0f)
	{
		//------------------------------------------
		// We are fading Down.
		fade.time += frameLength;
		if (fade.time >= 0.0f)
		{
			fade.time = 0.0f;
			return MusicFadeStep::Ended;
		}

		level = fabs(fade.time) / fadeDownTime;
		if (level < 0.0f)
			level = 0.0f;

		if (level > 1.0f)
			level = 1.0f;

		return MusicFadeStep::FadingDown;
	}

	//------------------------------------------
	// We are fading Up.
	fade.time -= frameLength;
	if (fade.time <= 0.0f)
	{
		fade.time = 0.0f;
		level = 1.0f;
		return MusicFadeStep::FadingUp;
	}

	level = (fadeDownTime - fabs(fade.time)) / fadeDownTime;
	if (level < 0.0f)
		level = 0.0f;

	if (level > 1.0f)
		level = 1.0f;

	return MusicFadeStep::FadingUp;
}

//---------------------------------------------------------------------------
class SoundSystem
{
	//Data Members
	//-------------
	protected:
		bool				channelInUse[MAX_DIGITAL_SAMPLES];
		long				channelSampleId[MAX_DIGITAL_SAMPLES];
		bool				fadeDown[MAX_DIGITAL_SAMPLES];
		
		unsigned long		numSoundBites;
		std::vector<SoundBite>	sounds;

		unsigned long		numBettySamples;
		unsigned long		numSupportSamples;
		// The three sound packet files are owned here and closed by destroy(); a sound bite reads through them while the system lives.
		std::unique_ptr<PacketFile>	soundDataFile;
		std::unique_ptr<PacketFile>	bettyDataFile;
		std::unique_ptr<PacketFile>	supportDataFile;
		
		long				numDMS;
		float				streamFadeDownTime;
		unsigned long		digitalStreamBitDepth;
		unsigned long		digitalStreamChannels;
		long				currentMusicId;
		ScriptMusic			scriptMusic;		// what an ABL script sees: its last request, ended by the scenario clock, never by the device
		
		// The two music crossfade slots: slot 0 plays on STREAM1CHANNEL, slot 1 on STREAM2CHANNEL.
		struct MusicSlot {
			HGOSAUDIO	handle = nullptr;
			MusicFade	fade;
			long		id = -1;
		};
		MusicSlot			musicSlots[2];
		
		HGOSAUDIO			stream3Handle;
		bool				stream3Active;

		std::vector<DigitalMusicEntry>	digitalMusic;
				
		float				maxSoundDistance;
		
		std::vector<uint8_t>	bettySoundBite;							//Used to store current bitching betty sample
		long				lastBettyId;
		HGOSAUDIO			bettyHandle;

		std::vector<uint8_t>	supportSoundBite;						//USed to store current support officer sample
		long				lastSupportId;
		HGOSAUDIO			supportHandle;
		
		DWORD				isRaining;								//What sound FX should play as an ambient background loop.  Rain/Fire/Nothing, etc.
		DWORD				oldRaining;								//Check if the old rain doesn't match new.  If so, play new instead!

		float				sensorBeepUpdateTime;
		
		bool				gamePaused;								//Flag to tell me if game is paused

		static float		digitalMasterVolume;
		static float		SFXVolume;
		static float		musicVolume;
		static float		radioVolume;
		static float		bettyVolume;

		// Advances one music crossfade slot's fade and end-of-stream decision for this frame.
		void updateMusicSlot (int slot);

	public:

		static long			largestSensorContact;
		
	//Member Functions
	//----------------
	public:
	
		SoundSystem (void)
		{
			init();
		}
		
		~SoundSystem (void)
		{
			destroy();
		}
		
		void init (void)
		{
			memset(channelInUse,0,sizeof(bool)*MAX_DIGITAL_SAMPLES);
			memset(channelSampleId,-1,sizeof(long)*MAX_DIGITAL_SAMPLES);
			memset(fadeDown,0,sizeof(bool)*MAX_DIGITAL_SAMPLES);
			
			numSoundBites = 0;
			sounds.clear();
			
			
			numDMS = 0;
			streamFadeDownTime = 0.0;
			digitalStreamBitDepth = 0;
			digitalStreamChannels = 0;
			
			digitalMusic.clear();
			
			maxSoundDistance = 0.0;
			
			bettySoundBite.clear();
			lastBettyId = -1;
			
			currentMusicId = -1;
			
			for (MusicSlot& slot : musicSlots)
			{
				slot.handle = nullptr;
				slot.fade.active = FALSE;
				slot.fade.time = 0.0f;
				slot.id = -1;
			}
			
			stream3Handle = 0;
			stream3Active = false;
			
			numBettySamples = 0;
			bettyHandle = nullptr;

			supportSoundBite.clear();
			lastSupportId = -1;
			
			numSupportSamples = 0;
			supportHandle = nullptr;
			
			isRaining = oldRaining = 0;
			sensorBeepUpdateTime = 0.0f;
			gamePaused = false;
		}
		
		long init (const char *soundFileName);
		virtual void destroy (void);
		
		void preloadSoundBite (long sampleId);
		static bool createWaveResource (HGOSAUDIO *handle, const std::vector<uint8_t>& wave);	//false (nothing created) when the wave header is unreadable
		long findOpenChannel (long start, long end);

		long playDigitalSample (unsigned long sampleId, glm::vec3 pos = glm::vec3(-9999.0f,-9999.0,-9999.0f), bool allowDupes = false);
		
		std::optional<long> playDigitalMusic (long musicId);
		
		std::optional<long> playDigitalStream (const char *streamName);
											 
		bool isDigitalMusicPlaying (void);
		bool isPlayingSample(long sampleId);
		bool isChannelPlaying (long channelId);
		bool isPlayingVoiceOver (void);

		long playBettySample (unsigned long bettySampleId);
		void stopBettySample (void);

		
		long playSupportSample (unsigned long supportSampleId, const char* fileName = nullptr);
		
 		void stopDigitalSample (unsigned long sampleHandleNumber);
		void stopDigitalMusic (void);
		void stopSupportSample (void);
	
		void setDigitalMasterVolume (byte volume);
		long getDigitalMasterVolume (void);
		
		unsigned char getSFXVolume();
		unsigned char getRadioVolume();
		unsigned char getMusicVolume();
		unsigned char getBettyVolume();

		void setSFXVolume(unsigned char volume);
		void setRadioVolume(unsigned char volume);
		void setMusicVolume(unsigned char volume);
		void setBettyVolume(unsigned char volume);

		virtual void update (void);
		
		void playABLDigitalMusic (long musicId);
		void stopABLMusic (void);
		long getCurrentMusicId (void);
		// Save/load (FIX1): the script's music view as seconds left on the scenario clock, and its restore.
		void getScriptMusicState (long &id, double &remaining, bool &loops);
		void restoreScriptMusic (long id, double remaining, bool loops);
		long musicTrackCount (void) const { return numDMS; }
		static double musicSeconds (std::string_view wavPath);
		
		void playABLSFX (long sfxId);
		void playABLVideo (long videoId);
		void playABLSpeech (long pilotId, long speechId);
		
		long playPilotSpeech (char *pilotSpeechFilename, long msgId);
		
		void setIsRaining (DWORD rainLevel)
		{
			isRaining = rainLevel;
		}

		void setIsPaused (void)
		{
			gamePaused = true;
		}

		void clearIsPaused (void)
		{
			gamePaused = false;
		}

		// Video-cutscene PCM push adapter. Exclusive with background
		// music while active. rate/channels match the mixer's native
		// format (see queryNativeFormat). Only S16 little-endian samples
		// accepted -- if the mixer's native format is not AUDIO_S16SYS,
		// beginVideoPCMStream fails and the caller falls back to silent
		// video.
		bool queryNativeFormat(int* rate, int* channels) const;
		bool beginVideoPCMStream(int rate, int channels);
		void pushVideoPCMSamples(const int16_t* samples, int frameCount);
		void endVideoPCMStream();
		int  videoPCMSamplesConsumed() const;  // monotonic frame-count for A/V sync
};

//---------------------------------------------------------------------------

extern bool useSound;
extern bool useMusic;

//---------------------------------------------------------------------------
#endif
