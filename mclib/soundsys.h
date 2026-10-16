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
#define MAX_DIGITAL_SAMPLES			16

inline constexpr int MAX_SAMPLE_VOLUME = 127;

inline constexpr int RADIO_CHANNEL = 0;
#define FIRE_CHANNEL				(MAX_DIGITAL_SAMPLES-1)
#define BETTY_CHANNEL				(FIRE_CHANNEL-1)
#define SUPPORT_CHANNEL				(BETTY_CHANNEL-1)

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
		
		HGOSAUDIO			stream1Handle;
		bool				stream1Active;
		float				stream1Time;
		long				stream1Id;
		
		HGOSAUDIO			stream2Handle;
		bool				stream2Active;
		float				stream2Time;
		long				stream2Id;
		
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
			
			stream1Handle = 0;
			stream1Active = FALSE;
			stream1Time = 0.0;
			
			stream2Handle = 0;
			stream2Active = FALSE;
			stream2Time = 0.0;
			
			stream3Handle = 0;
			stream3Active = false;
			
			numBettySamples = 0;
			bettyHandle = NULL;

			supportSoundBite.clear();
			lastSupportId = -1;
			
			numSupportSamples = 0;
			supportHandle = NULL;
			
			isRaining = oldRaining = 0;
			sensorBeepUpdateTime = 0.0f;
			gamePaused = false;
		}
		
		long init (const char *soundFileName);
		virtual void destroy (void);
		
		void preloadSoundBite (long sampleId);
		static bool createWaveResource (HGOSAUDIO *handle, const std::vector<uint8_t>& wave);	//false (nothing created) when the wave header is unreadable
		long findOpenChannel (long start, long end);

		long playDigitalSample (unsigned long sampleId, Stuff::Vector3D pos = Stuff::Vector3D(-9999.0f,-9999.0,-9999.0f), bool allowDupes = false);
		
		long playDigitalMusic (long musicId);
		
		long playDigitalStream (const char *streamName);
											 
		bool isDigitalMusicPlaying (void);
		bool isPlayingSample(long sampleId);
		bool isChannelPlaying (long channelId);
		bool isPlayingVoiceOver (void);

		long playBettySample (unsigned long bettySampleId);
		void stopBettySample (void);

		
		long playSupportSample (unsigned long supportSampleId, const char* fileName = NULL);
		
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
