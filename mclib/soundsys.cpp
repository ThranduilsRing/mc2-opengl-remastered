#include "Gates/preferences_legacy.h"
#include "gameos_music_cache.h"
//---------------------------------------------------------------------------
// soundsys.cpp - This file is the sound system code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files
#ifndef SOUNDSYS_H
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include"soundsys.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#include "platform_stdlib.h" // _splitpath

#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_atomic.h>
#include <algorithm>
#include <string>
#include <vector>
#include <format>
#include "../GameOS/gameos/utils/mc2log.h"

bool objectiveObservationEnabled();
void objectiveObservationWav(const char*, const char*, int, const char*, bool);

//---------------------------------------------------------------------------
// static globals
bool useSound = TRUE;
bool useMusic = TRUE;


constexpr float MAX_SENSOR_UPDATE_TIME = 5.0f;	//Seconds

float		SoundSystem::digitalMasterVolume = 0.99f;
float		SoundSystem::SFXVolume = 0.6f;
float		SoundSystem::musicVolume = 0.4f;
float		SoundSystem::radioVolume = 0.6f;
float		SoundSystem::bettyVolume = 0.7f;
long		SoundSystem::largestSensorContact = -1;

constexpr float FALLOFF_DISTANCE = 1500.0f;

//---------------------------------------------------------------------------
// Video PCM push adapter — file-scope state + Mix_HookMusic callback
//---------------------------------------------------------------------------
namespace {
    struct VideoAudioState {
        std::vector<int16_t>  ring;
        size_t                cap = 0;
        SDL_SpinLock          lock = 0;
        size_t                head = 0;   // producer index
        size_t                tail = 0;   // consumer index
        SDL_atomic_t          consumedFrames;
        int                   rate = 0;
        int                   channels = 0;
        bool                  active = false;
    };
    VideoAudioState g_va;

    void videoMixHook(void* /*udata*/, Uint8* stream, int len)
    {
        int16_t* out = reinterpret_cast<int16_t*>(stream);
        int outSamples = len / static_cast<int>(sizeof(int16_t));
        SDL_AtomicLock(&g_va.lock);
        for (int i = 0; i < outSamples; ++i) {
            if (g_va.tail == g_va.head) {
                out[i] = 0;   // underrun = silence
            } else {
                out[i] = g_va.ring[g_va.tail];
                g_va.tail = (g_va.tail + 1) % g_va.cap;
            }
        }
        SDL_AtomicUnlock(&g_va.lock);
        int framesWritten = (g_va.channels > 0)
                             ? (outSamples / g_va.channels)
                             : outSamples;
        SDL_AtomicAdd(&g_va.consumedFrames, framesWritten);
    }
} // namespace

// Limits on counts and packet sizes read from the sound setup and packet files; a larger value means a damaged file.
constexpr unsigned long MAX_SOUND_BITES = 4096;
constexpr long MAX_DIGITAL_MUSIC = 1024;
constexpr long MAX_SOUND_PACKET_BYTES = 64l * 1024l * 1024l;

// The channel a music crossfade slot plays on: slot 0 is STREAM1CHANNEL, slot 1 is STREAM2CHANNEL.
static int musicSlotChannel (int slot)
{
	return (slot == 0) ? STREAM1CHANNEL : STREAM2CHANNEL;
}

//---------------------------------------------------------------------------
// class SoundSystem
//---------------------------------------------------------------------------
void SoundSystem::destroy (void)
{
    for (auto& sound : sounds) gosAudio_DestroyResource(&sound.resourceHandle);
    for (MusicSlot& slot : musicSlots) gosAudio_DestroyResource(&slot.handle);
    for (HGOSAUDIO* handle : {&stream3Handle, &bettyHandle, &supportHandle})
        gosAudio_DestroyResource(handle);
	gosAudio_ClearMusicCache();
	sounds.clear();
	digitalMusic.clear();
	bettySoundBite.clear();
	supportSoundBite.clear();

    soundDataFile.reset();
    bettyDataFile.reset();
    supportDataFile.reset();

}

//---------------------------------------------------------------------------
long SoundSystem::init (const char *soundFileName)
{
	gosAudio_ClearMusicCache();
	if (useSound)
	{
		FullPathFileName soundName;
		soundName.init(soundPath,soundFileName,".snd");
		
		FitIniFile soundFile;
		long result = soundFile.open(soundName);
		gosASSERT(result == NO_ERR);

		result = soundFile.seekBlock("SoundSetup");
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdFloat("MaxSoundDistance",maxSoundDistance);
		gosASSERT(result == NO_ERR);

		//-----------------------------------------------------------------------
		// Startup the Sound packet File with the sound Blocks in it.
		// This works by sound ID.  The sound ID is the packet number.
		// When ordered to play a sample, the sound system check to see if that
		// sound ID is in the cache.  If not, it is loaded.  If there is no more
		// room, any idle sounds are flushed in order of priority.
		soundDataFile = std::make_unique<PacketFile>();
		
		FullPathFileName soundDataPath;
		soundDataPath.init(CDsoundPath,soundFileName,".pak");
		
		result = soundDataFile->open(soundDataPath);
		gosASSERT(result == NO_ERR);
		
		bettyDataFile = std::make_unique<PacketFile>();
		
		FullPathFileName bettyDataPath;
		bettyDataPath.init(CDsoundPath,"Betty",".pak");
		
		result = bettyDataFile->open(bettyDataPath);
		gosASSERT(result == NO_ERR);

		numBettySamples = bettyDataFile->getNumPackets();
		
		supportDataFile = std::make_unique<PacketFile>();
		
		FullPathFileName supportDataPath;
		supportDataPath.init(CDsoundPath,"support",".pak");
		
		result = supportDataFile->open(supportDataPath);
		gosASSERT(result == NO_ERR);

		numSupportSamples = supportDataFile->getNumPackets();
 		//-----------------------------------------------------------------------
		// Load all of the sound Bite data.  Do not load actual packet unless
		// preload field is TRUE.
		result = soundFile.seekBlock("SoundBites");
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdULong("numBites",numSoundBites);
		gosASSERT(result == NO_ERR);

		//-----------------------------------------------------------------------
		// Preallocate SoundBites
		if (numSoundBites > MAX_SOUND_BITES)
		{
			MC2_LOG(Audio, Error, "sound setup: {} sound bites is over the limit of {}; none are loaded", numSoundBites, MAX_SOUND_BITES);
			numSoundBites = 0;
		}
		sounds.assign(numSoundBites,SoundBite());
		
		for (long i=0;i<static_cast<long>(numSoundBites);i++)
		{
			char biteBlock[20];
			auto biteBlockEnd = std::format_to_n(biteBlock, sizeof(biteBlock) - 1, "SoundBite{}", i).out;
			*biteBlockEnd = '\0';
			
			result = soundFile.seekBlock(biteBlock);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdULong("priority",sounds[i].priority);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdULong("cache",sounds[i].cacheStatus);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdULong("soundId",sounds[i].soundId);
			gosASSERT(result == NO_ERR);
			
			preloadSoundBite(i);		//ALWAYS Preload!!!!!!!!!!!!!

			result = soundFile.readIdFloat("volume",sounds[i].volume);
			gosASSERT(result == NO_ERR);
		}		

		//---------------------------------------------------------------
		// Load the digital Music Data Strings
		result = soundFile.seekBlock("DigitalMusic");
		gosASSERT(result == NO_ERR);
			
		result = soundFile.readIdLong("NumDMS",numDMS);
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdFloat("StreamFadeDownTime",streamFadeDownTime);
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdULong("StreamBitDepth",digitalStreamBitDepth);
		gosASSERT(result == NO_ERR);
			
		result = soundFile.readIdULong("StreamChannels",digitalStreamChannels);
		gosASSERT(result == NO_ERR);

		if (numDMS < 0 || numDMS > MAX_DIGITAL_MUSIC)
		{
			MC2_LOG(Audio, Error, "sound setup: {} digital music entries is outside 0..{}; none are loaded", numDMS, MAX_DIGITAL_MUSIC);
			numDMS = 0;
		}
		digitalMusic.assign(numDMS,DigitalMusicEntry());

		for (int i=0;i<numDMS;i++)
		{
			char digitalMSId[20];
			auto digitalMSIdEnd = std::format_to_n(digitalMSId, sizeof(digitalMSId) - 1, "DMS{}", i).out;
			*digitalMSIdEnd = '\0';

			char digitalMSBId[20];
			auto digitalMSBIdEnd = std::format_to_n(digitalMSBId, sizeof(digitalMSBId) - 1, "DMSLoop{}", i).out;
			*digitalMSBIdEnd = '\0';

			char digitalMSVId[25];
			auto digitalMSVIdEnd = std::format_to_n(digitalMSVId, sizeof(digitalMSVId) - 1, "DMSVolume{}", i).out;
			*digitalMSVIdEnd = '\0';

			char musicName[30];
			result = soundFile.readIdString(digitalMSId,musicName,29);
			gosASSERT(result == NO_ERR);
			digitalMusic[i].id = musicName;

			result = soundFile.readIdBoolean(digitalMSBId,digitalMusic[i].loop);
			gosASSERT(result == NO_ERR);

			result = soundFile.readIdFloat(digitalMSVId,digitalMusic[i].volume);
			if (result != NO_ERR)
				digitalMusic[i].volume = 1.0f;

			// Script music requests must not synchronously decode ADPCM inside the mission brain.
			if (useMusic && strncmp(musicName, "NONE", 4) != 0) {
				FullPathFileName path;
				path.init(soundPath, musicName, ".wav");
				gosAudio_PreloadMusic(static_cast<const char*>(path));
			}
		}

		soundFile.close();
		
		for (int i=0;i<MAX_DIGITAL_SAMPLES+4;i++)
		{
			gosAudio_AllocateChannelSliders(i,gosAudio_Volume | gosAudio_Panning);
		}
	}
	
	digitalMasterVolume = float(DigitalMasterVolume) / 256.0f;
	SFXVolume = float(sfxVolume) / 256.0f;
	musicVolume = float(MusicVolume) / 256.0f;
	radioVolume = float(RadioVolume) / 256.0f;
	bettyVolume = float(BettyVolume) / 256.0f;

	musicSlots[0].id = musicSlots[1].id = 0;

	return(NO_ERR);
}

//////////////////////////////////////////////////////////////////
//
// wave_ParseWaveMemory
//   Parses a chunk of memory into the header and samples.
//   This is done by looking for the "fmt " and "data"
//   fields in the memory.
//
//////////////////////////////////////////////////////////////////
bool wave_ParseWaveMemory(MemoryPtr lpChunkOfMemory, MC2_WAVEFORMATEX** lplpWaveHeader, MemoryPtr* lplpWaveSamples,DWORD *lpcbWaveSize)
{
    DWORD 	*pdw;
    DWORD 	*pdwEnd;
    DWORD   dwRiff;
    DWORD   dwType;
    DWORD   dwLength;

    // Set defaults to NULL or zero
    if (*lplpWaveHeader)
        *lplpWaveHeader = nullptr;

    if (*lplpWaveSamples)
        *lplpWaveSamples = nullptr;

    if (lpcbWaveSize)
        *lpcbWaveSize = 0;

    // Set up DWORD pointers to the start of the chunk
    // of memory.
    pdw = reinterpret_cast<DWORD *>(lpChunkOfMemory);

    // Get the type and length of the chunk of memory
    dwRiff = *pdw++;
    dwLength = *pdw++;
    dwType = *pdw++;

    // Using the mmioFOURCC macro (part of Windows SDK), ensure
    // that this is a RIFF WAVE chunk of memory
    if (dwRiff != 0x46464952)
      return FALSE;      // not even RIFF

    if (dwType != 0x45564157)
      return FALSE;      // not a WAV

    // Find the pointer to the end of the chunk of memory
    pdwEnd = reinterpret_cast<DWORD *>((reinterpret_cast<BYTE *>(pdw) + dwLength-4));

    // Run through the bytes looking for the tags
    while (pdw < pdwEnd)
      {
      dwType   = *pdw++;
      dwLength = *pdw++;

      switch (dwType)
        {
        // Found the format part
        case 0x20746d66:

          if (*lplpWaveHeader == nullptr)
            {
            if (dwLength < sizeof(MC2_WAVEFORMATEX))
              return FALSE; // something's wrong! Not a WAV

            // Set the lplpWaveHeader to point to this part of
            // the memory chunk
            *lplpWaveHeader = reinterpret_cast<MC2_WAVEFORMATEX*>(pdw);

            // Check to see if the other two items have been
            // filled out yet (the bits and the size of the
            // bits). If so, then this chunk of memory has
            // been parsed out and we can exit
            if ((!lplpWaveSamples || *lplpWaveSamples) &&
                (!lpcbWaveSize || *lpcbWaveSize))
              {
              return TRUE;
              }
            }
          break;

        // Found the samples
        case 0x61746164:

          if ((lplpWaveSamples && !*lplpWaveSamples) ||
              (lpcbWaveSize && !*lpcbWaveSize))
            {
            // Point the samples pointer to this part of the
            // chunk of memory.
            if (*lplpWaveSamples == nullptr)
				*lplpWaveSamples = reinterpret_cast<MemoryPtr>(pdw);

            // Set the size of the wave
            if (lpcbWaveSize)    *lpcbWaveSize = dwLength;

            // Make sure we have our header pointer set up.
            // If we do, we can exit
            if (lplpWaveHeader)
              return TRUE;
            }
          break;

        } // End case

      // Move the pointer through the chunk of memory
      pdw = reinterpret_cast<DWORD *>((reinterpret_cast<BYTE *>(pdw) + ((dwLength+1)&~1)));
      }

  // Failed! If we made it here, we did not get all the peices
  // of the wave
  return FALSE;
}

//---------------------------------------------------------------------------
// Creates the audio resource for a wave held in memory. A wave without a readable header creates nothing and is logged, so a
// damaged sample is silent instead of read past its buffer.
bool SoundSystem::createWaveResource (HGOSAUDIO *handle, const std::vector<uint8_t>& wave)
{
	constexpr size_t MIN_WAVE_BYTES = 44;			// RIFF header plus the format chunk
	MC2_WAVEFORMATEX *waveFormat = nullptr;
	MemoryPtr dataOffset = nullptr;
	DWORD length = 0;

	if (wave.size() < MIN_WAVE_BYTES ||
		!wave_ParseWaveMemory(const_cast<MemoryPtr>(wave.data()),&waveFormat,&dataOffset,&length) ||
		!waveFormat || !dataOffset || waveFormat->nChannels == 0)
	{
		MC2_LOG(Audio, Warn, "sound: a sample of {} bytes has no readable wave header; it is not played", wave.size());
		return false;
	}

	gosAudio_Format soundFormat;
	soundFormat.wFormatTag = 1;				//PCM
	soundFormat.nChannels = waveFormat->nChannels;
	soundFormat.nSamplesPerSec = waveFormat->nSamplesPerSec;
	soundFormat.nAvgBytesPerSec = waveFormat->nAvgBytesPerSec;
	soundFormat.nBlockAlign = waveFormat->nBlockAlign;
	soundFormat.wBitsPerSample = waveFormat->nBlockAlign / waveFormat->nChannels * 8;
	soundFormat.cbSize = 0;

	gosAudio_CreateResource(handle,gosAudio_UserMemory, nullptr, &soundFormat,dataOffset,length);
	return true;
}

//---------------------------------------------------------------------------
void SoundSystem::preloadSoundBite (long soundId)
{
	long result = soundDataFile->seekPacket(soundId);
	if (result != NO_ERR)
		return;
		
	//-------------------------------------------------------------
	// If there is a packet in this spot, load it.
	// First, check if there is enough room in the sound heap
	// and if not, free up any samples whose cache status says dump.
	unsigned long packetSize = soundDataFile->getPacketSize();
	if (packetSize > 0)
	{
		SoundBite *thisSoundBite = (&(sounds[soundId]));
		gosAudio_DestroyResource(&thisSoundBite->resourceHandle);
			
		if (packetSize > MAX_SOUND_PACKET_BYTES)
		{
			MC2_LOG(Audio, Warn, "sound: bite {} has a bad size ({} bytes); it is silent", soundId, packetSize);
			return;
		}
		if (thisSoundBite->biteData.empty())
			thisSoundBite->biteData.resize(packetSize);

		if (!soundDataFile->readPacket(soundId,thisSoundBite->biteData.data()))
		{
			MC2_LOG(Audio, Warn, "sound: bite {} could not be read; it is silent", soundId);
			thisSoundBite->biteData.clear();
			return;
		}
		
		if (!createWaveResource(&thisSoundBite->resourceHandle,thisSoundBite->biteData))
			thisSoundBite->resourceHandle = nullptr;
	}
}

//---------------------------------------------------------------------------
// One music slot's frame: the fade rule, then the end-of-stream decision. Both slots run through here, slot 0 first.
void SoundSystem::updateMusicSlot (int slot)
{
	MusicSlot& s = musicSlots[slot];
	const int channel = musicSlotChannel(slot);

	float volLevel = 1.0f;
	const MusicFadeStep step = advanceMusicFade(s.fade, frameLength, streamFadeDownTime, volLevel);

	if ((step == MusicFadeStep::FadingDown) || (step == MusicFadeStep::FadingUp))
	{
		gosAudio_SetChannelSlider(channel, gosAudio_Volume, digitalMasterVolume * volLevel * musicVolume * digitalMusic[s.id].volume);
	}
	else if (step == MusicFadeStep::Ended)
	{
		if (s.handle)
			gosAudio_DestroyResource(&s.handle);
		s.handle = nullptr;
		s.fade.active = FALSE;
	}

	if (s.fade.active && s.handle && (gosAudio_GetChannelPlayMode(channel) == gosAudio_Stop))
	{
		if (s.handle)
			gosAudio_DestroyResource(&s.handle);

		s.handle = nullptr;
		s.fade.time = 0.0;
		s.fade.active = FALSE;
	}
}

//---------------------------------------------------------------------------
void SoundSystem::update (void)
{
	if (useSound && (isRaining != oldRaining))
	{
		//Play the isRaining SoundEffect at the soundEffect level in a loop.
		if (isRaining && (isRaining < numSoundBites))
		{
			long ourChannel = FIRE_CHANNEL;
	
			gosAudio_SetChannelSlider(ourChannel,gosAudio_Panning, 0.0);
			float vol = sounds[isRaining].volume;
			gosAudio_SetChannelSlider(ourChannel,gosAudio_Volume, (digitalMasterVolume * vol * SFXVolume));
			channelSampleId[ourChannel] = isRaining;
			channelInUse[ourChannel] = TRUE;
				
			if (!sounds[isRaining].biteData.empty())
			{
				gosAudio_AssignResourceToChannel( ourChannel, sounds[isRaining].resourceHandle );
				gosAudio_SetChannelPlayMode( ourChannel, gosAudio_Loop );
			}
			
			oldRaining = isRaining;
		}
		else
		{
			//Stop the sound Effect by fading it to zero!
			oldRaining = isRaining;
			gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(FIRE_CHANNEL);
			if ( sampleStatus == gosAudio_PlayOnce || sampleStatus == gosAudio_Loop )
			{
				fadeDown[FIRE_CHANNEL] = TRUE;
			}
		}
 	}
	
	if (useSound && !bettySoundBite.empty())
	{
		//---------------------------------------------------
		// Check if betty is done.  If so, whack the memory
		gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(BETTY_CHANNEL);
		if (sampleStatus != gosAudio_PlayOnce)
		{
			gosAudio_DestroyResource(&bettyHandle);
			bettySoundBite.clear();
			bettyHandle = nullptr;
		}
	}

	if (useSound && !supportSoundBite.empty())
	{
		//---------------------------------------------------
		// Check if betty is done.  If so, whack the memory
		gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(SUPPORT_CHANNEL);
		if (sampleStatus != gosAudio_PlayOnce)
		{
			gosAudio_DestroyResource(&supportHandle);
			supportSoundBite.clear();
		}
	}
	
	if (useSound && stream3Handle)
	{
		gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(STREAM3CHANNEL);
		if (!stream3Active && (sampleStatus != gosAudio_PlayOnce))
		{
			stream3Active = true;
			gosAudio_SetChannelPlayMode( STREAM3CHANNEL, gosAudio_PlayOnce );
		}
				
		sampleStatus = gosAudio_GetChannelPlayMode(STREAM3CHANNEL);
		if (stream3Active && sampleStatus != gosAudio_PlayOnce)
		{
			gosAudio_DestroyResource(&stream3Handle);
			stream3Handle = nullptr;
			stream3Active = false;
		}
	}
	
	if ((largestSensorContact > -1) && (sensorBeepUpdateTime > MAX_SENSOR_UPDATE_TIME))
	{
		playDigitalSample(SENSOR2);
		sensorBeepUpdateTime = 0.0f;
	}

	//Start the music change we requested playing!!
	// This avoids the stupid sound bug of starting more then one stream per frame!!
	for (int slot = 0; slot < 2; ++slot)
	{
		if (musicSlots[slot].handle && !musicSlots[slot].fade.active)
		{
			musicSlots[slot].fade.active = true;
			gosAudio_SetChannelPlayMode( musicSlotChannel(slot), digitalMusic[musicSlots[slot].id].loop ? gosAudio_Loop : gosAudio_PlayOnce);
		}
	}

 	if (useMusic && (musicSlots[0].fade.active || musicSlots[1].fade.active))
	{
		updateMusicSlot(0);
		updateMusicSlot(1);

		if (!musicSlots[0].fade.active && !musicSlots[1].fade.active)
			currentMusicId = -1;
	}

	if (useSound)
	{
		//-----------------------------------------------
		// Check all samples to see if one should end.
		for (long i=0;i<MAX_DIGITAL_SAMPLES;i++)
		{
			gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(i);
			if (sampleStatus == gosAudio_PlayOnce)
			{
				if (fadeDown[i])
				{
					float vol;
					gosAudio_GetChannelSlider(i, gosAudio_Volume, &vol);
					if (vol <= 0.1f)
						vol = 0.1f;

					gosAudio_SetChannelSlider(i, gosAudio_Volume, vol-0.1f);
					gosAudio_GetChannelSlider(i, gosAudio_Volume, &vol);
					if (vol == 0.0f)
					{
						fadeDown[i] = FALSE;
						gosAudio_SetChannelPlayMode(i, gosAudio_Stop);
					}
				}
			}
			else
			{
				fadeDown[i] = FALSE;
				channelSampleId[i] = -1;
				channelInUse[i] = FALSE;
			}
		}
	}

	if (!gamePaused)
		sensorBeepUpdateTime += frameLength;
}

//---------------------------------------------------------------------------
long SoundSystem::playDigitalMusic (long musicId)
{
	//-------------------------------------------------------------------
	// Make sure we have a real music filename.
	if (useMusic)
	{
		if ((musicId >= 0) && (musicId < numDMS) && (strncmp(digitalMusic[musicId].id.c_str(),"NONE",4) != 0))
		{
			if (musicId == currentMusicId)
				return(-1);
				
			if (useSound)
			{
				//---------------------------------------------------------------------------------------------
				// Just start tune.  No fade necessary.  Set fadeTime to 0.0 to tell update to leave it alone.
				// Use Stream1 by default.
				FullPathFileName digitalStream;
				digitalStream.init(soundPath,digitalMusic[musicId].id.c_str(),".wav");

				//---------------------------------------------
				// Gotta check if music file is there
				// new sound system can't handle it if its not.
				if (fileExists(digitalStream))
				{
					int slot;
					if (!musicSlots[0].fade.active && !musicSlots[1].fade.active)
					{
						slot = 0;
					}
					else if (!musicSlots[1].fade.active)
					{
						slot = 1;

						//Need to check if stream1 ever got all the way faded up!
						musicSlots[0].fade.time = (musicSlots[0].fade.time > 0.0f) ? -musicSlots[0].fade.time : -streamFadeDownTime;
					}
					else if (!musicSlots[0].fade.active)
					{
						slot = 0;

						//Need to check if stream2 ever got all the way faded up!
						musicSlots[1].fade.time = (musicSlots[1].fade.time > 0.0f) ? -musicSlots[1].fade.time : -streamFadeDownTime;
					}
					else
					{
						return(-1);
					}

					MusicSlot& s = musicSlots[slot];
					const int channel = musicSlotChannel(slot);

					if (s.handle)
					{
						gosAudio_DestroyResource(&s.handle);
						s.handle = nullptr;
					}

					gosAudio_CreateResource(&s.handle, gosAudio_StreamedFile, static_cast<const char *>(digitalStream));

					s.fade.time = streamFadeDownTime;

					gosAudio_SetChannelSlider( channel, gosAudio_Volume, 0.0f );
					gosAudio_SetChannelSlider( channel, gosAudio_Panning,0.0f );
					gosAudio_AssignResourceToChannel( channel, s.handle );

					currentMusicId = musicId;
					s.id = musicId;
				}
			}
		}
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------
long SoundSystem::playDigitalStream (const char *streamName)
{
	const char* accepted = "disabled";
	std::string resourcePath;
	bool resourceAccepted = false;
	long result = NO_ERR;
	if (useSound)
	{
		char actualName[1024];
		_splitpath(streamName,nullptr,nullptr,actualName,nullptr);

		FullPathFileName digitalStream;
		digitalStream.init(soundPath,actualName,".wav");
		if (objectiveObservationEnabled()) { resourcePath = digitalStream; }

		accepted = "missing";
		if (fileExists(digitalStream))
		{
			if (!stream3Active)
			{
				if (stream3Handle)
				{
					gosAudio_DestroyResource(&stream3Handle);
					stream3Handle = nullptr;
				}
						
				gosAudio_CreateResource(&stream3Handle, gosAudio_StreamedFile, static_cast<const char *>(digitalStream));
				
				stream3Active = false;
				
				gosAudio_AssignResourceToChannel( STREAM3CHANNEL, stream3Handle );
				gosAudio_SetChannelSlider( STREAM3CHANNEL,gosAudio_Volume, digitalMasterVolume * bettyVolume );
				gosAudio_SetChannelSlider( STREAM3CHANNEL, gosAudio_Panning,0.0f );
				accepted = stream3Handle ? "resource_created_channel_assigned" : "resource_rejected";
				resourceAccepted = stream3Handle != nullptr;
			}
			else
			{
				accepted = "busy";
				result = -1;
			}
		}
	}

	objectiveObservationWav(streamName, resourcePath.c_str(), STREAM3CHANNEL, accepted, resourceAccepted);
	return result;
}

//---------------------------------------------------------------------------
long SoundSystem::playBettySample (unsigned long bettySampleId)
{
	if (useSound && bettySoundBite.empty())	//Playing Betty takes precedence
	{

		if (bettySampleId >= numBettySamples)
			return(-1);
			
		long ourChannel = BETTY_CHANNEL;
	
		gosAudio_SetChannelSlider( ourChannel, gosAudio_Panning, 0.0f );
		float vol = 1.0;
		gosAudio_SetChannelSlider( ourChannel, gosAudio_Volume, digitalMasterVolume * vol * bettyVolume );
		gosAudio_SetChannelSlider( RADIO_CHANNEL, gosAudio_Volume, digitalMasterVolume * vol * radioVolume * 0.5f ); 
		lastBettyId = bettySampleId;
		channelInUse[ourChannel] = TRUE;


		long result = bettyDataFile->seekPacket(bettySampleId);
		if (result != NO_ERR)
			return(-1);
			
		long bettySize = bettyDataFile->getPacketSize();
		if (bettySize <= 0 || bettySize > MAX_SOUND_PACKET_BYTES)
		{
			MC2_LOG(Audio, Warn, "sound: betty sample {} has a bad size ({} bytes); it is not played", bettySampleId, bettySize);
			return(-1);
		}
		bettySoundBite.resize(bettySize);
		if (!bettyDataFile->readPacket(bettySampleId,bettySoundBite.data()))
		{
			MC2_LOG(Audio, Warn, "sound: betty sample {} could not be read; it is not played", bettySampleId);
			bettySoundBite.clear();
			return(-1);
		}
		
		if (!createWaveResource(&bettyHandle,bettySoundBite))
		{
			bettySoundBite.clear();
			return(-1);
		}
		gosAudio_AssignResourceToChannel( ourChannel, bettyHandle );
		gosAudio_SetChannelPlayMode(ourChannel, gosAudio_PlayOnce);
			
		return(BETTY_CHANNEL);
	}

	return(-1);
}

//---------------------------------------------------------------------------
long SoundSystem::playSupportSample (unsigned long supportSampleId, const char* fileName)
{
	if (useSound && supportSoundBite.empty())		//Playing Support takes precedence
	{
		if (!fileName)
		{
			if (supportSampleId >= numSupportSamples)
				return(-1);
		}
			
		long ourChannel = SUPPORT_CHANNEL;
		
		if (fileName) 
		{
			playDigitalStream(fileName);
			return (STREAM3CHANNEL);
		}
		else 
		{
	
			gosAudio_SetChannelSlider( ourChannel, gosAudio_Panning, 0.0f );
			float vol = 1.0;
			gosAudio_SetChannelSlider( ourChannel, gosAudio_Volume, digitalMasterVolume * vol * bettyVolume );
			lastSupportId = supportSampleId;
			channelInUse[ourChannel] = TRUE;

			long result = supportDataFile->seekPacket(supportSampleId);
			if (result != NO_ERR)
				return(-1);
			long supportSize = supportDataFile->getPacketSize();
			if (supportSize <= 0 || supportSize > MAX_SOUND_PACKET_BYTES)
			{
				return -1;      //Sound Bite does NOT exist!!
			}
			supportSoundBite.resize(supportSize);

			if (!supportDataFile->readPacket(supportSampleId,supportSoundBite.data()))
			{
				MC2_LOG(Audio, Warn, "sound: support sample {} could not be read; it is not played", supportSampleId);
				supportSoundBite.clear();
				return -1;
			}
		}
		
		if (!createWaveResource(&supportHandle,supportSoundBite))
		{
			supportSoundBite.clear();
			return -1;
		}
		gosAudio_AssignResourceToChannel( ourChannel, supportHandle );
		gosAudio_SetChannelPlayMode(ourChannel, gosAudio_PlayOnce);
			
		return(SUPPORT_CHANNEL);
	}

	return(-1);
}

//---------------------------------------------------------------------------
bool SoundSystem::isPlayingSample (long sampleId)
{
	for (long i=0;i<MAX_DIGITAL_SAMPLES;i++)
	{
		if (sampleId == channelSampleId[i])
			return TRUE;
	}

	return FALSE;
}

//---------------------------------------------------------------------------
bool SoundSystem::isChannelPlaying (long channelId)
{
	if ((channelId < 0) || (channelId > MAX_DIGITAL_SAMPLES))
		return(FALSE);
		
	gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(channelId);
	if (sampleStatus == gosAudio_PlayOnce || sampleStatus == gosAudio_Loop)
		return(TRUE);

	return FALSE;
}

//---------------------------------------------------------------------------
bool SoundSystem::isPlayingVoiceOver (void)
{
	if (stream3Handle)
		return true;

	return false;
}

//---------------------------------------------------------------------------
bool SoundSystem::isDigitalMusicPlaying (void)
{
	if (musicSlots[0].handle || musicSlots[1].handle)
		return true;

	return false;
}

//---------------------------------------------------------------------------
long SoundSystem::findOpenChannel (long start, long end)
{
	long channel = start;
	
	while (channelInUse[channel] && (channel < end))
		channel++;
		
	if (channel == end)
		return(-1);
		
	return(channel);
}	

//---------------------------------------------------------------------------
long SoundSystem::playDigitalSample (unsigned long sampleId, glm::vec3 pos, bool allowDupes)
{
	if ((useSound && allowDupes) || (!isPlayingSample(sampleId) && !allowDupes))
	{
		if (sampleId >= numSoundBites)
			return(-1);
			
		long ourChannel = findOpenChannel(1,SUPPORT_CHANNEL);
	
		if (ourChannel != -1)
		{
			float distanceVolume = 1.0f;
			float panVolume = 0.0f;
			if (eye && (pos.z != -9999.0f))
			{
				glm::vec3 distance;
				distance = eye->getPosition() - pos;
				float dist = glm::length(distance);
				if (dist < FALLOFF_DISTANCE)
					distanceVolume = (FALLOFF_DISTANCE - dist) / FALLOFF_DISTANCE;
				else
					return -1;		//Do not play sound.  TOO far away!!

				//Figure out where in stereo field to play.
				OppRotate(distance,eye->getRotation().y);

				panVolume = distance.x / (FALLOFF_DISTANCE * 0.5f);
				if (panVolume > 1.0f)
					panVolume = 1.0f;
				else if (panVolume < -1.0f)
					panVolume = -1.0f;
			}

			float vol = sounds[sampleId].volume * distanceVolume;
			if (vol > 1.0f)
				vol = 1.0f;
			else if (vol <= 0.0f)		//No VOlume.  DON't PLAY!
				return -1;

			gosAudio_SetChannelSlider(ourChannel,gosAudio_Panning, panVolume);
			gosAudio_SetChannelSlider(ourChannel,gosAudio_Volume, (digitalMasterVolume * vol * SFXVolume)) ;
			channelSampleId[ourChannel] = sampleId;
			channelInUse[ourChannel] = TRUE;
				
			if (!sounds[sampleId].biteData.empty() && sounds[sampleId].resourceHandle)
			{
				gosAudio_AssignResourceToChannel( ourChannel, sounds[sampleId].resourceHandle );
				gosAudio_SetChannelPlayMode(ourChannel, gosAudio_PlayOnce);
			}
			
			return(ourChannel);
		}
	}

	return(-1);
}

//---------------------------------------------------------------------------
void SoundSystem::stopDigitalSample (unsigned long sampleHandleNumber)
{
	if (useSound)
	{
		gosAudio_PlayMode
			sampleStatus = gosAudio_GetChannelPlayMode(sampleHandleNumber);
		if ( sampleStatus == gosAudio_PlayOnce || sampleStatus == gosAudio_Loop )
		{
			fadeDown[sampleHandleNumber] = TRUE;
		}
	}
}

//---------------------------------------------------------------------------
void SoundSystem::stopDigitalMusic (void)
{
	if (useSound && useMusic)
	{
		for (MusicSlot& slot : musicSlots)
		{
			// Cancel music queued before this frame activated its fade (campaign intro).
			if (slot.handle)
			{
				gosAudio_DestroyResource(&slot.handle);

				slot.handle = nullptr;
				slot.fade.time = 0.0;
				slot.fade.active = FALSE;
			}
		}

		currentMusicId = -1;
	}

	scriptMusic.stop();		// a stop is an explicit call from mission code, so the script's view follows it with audio on or off
}	

//---------------------------------------------------------------------------
void SoundSystem::stopSupportSample (void)
{
	if (stream3Handle)
	{
		gosAudio_DestroyResource(&stream3Handle);
		stream3Handle = nullptr;
		stream3Active = false;
	}
}

//---------------------------------------------------------------------------
void SoundSystem::stopBettySample (void)
{
	if (bettyHandle)
	{
			gosAudio_SetChannelPlayMode(BETTY_CHANNEL, gosAudio_Stop);
			gosAudio_DestroyResource(&bettyHandle);
			bettySoundBite.clear();
			bettyHandle = nullptr;


	}
}


//---------------------------------------------------------------------------
void SoundSystem::setDigitalMasterVolume (byte volume)
{
	if (useSound)
	{
		DigitalMasterVolume = volume;
		digitalMasterVolume = float(DigitalMasterVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
long SoundSystem::getDigitalMasterVolume (void)
{
	if (useSound)
	{
		return DigitalMasterVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setSFXVolume(unsigned char volume)
{
	if (useSound)
	{
		sfxVolume = volume;
		SFXVolume = float(sfxVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getSFXVolume(void)
{
	if (useSound)
	{
		return sfxVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setRadioVolume(unsigned char volume)
{
	if (useSound)
	{
		RadioVolume = volume;
		radioVolume = float(RadioVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getRadioVolume(void)
{
	if (useSound)
	{
		return RadioVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setMusicVolume(unsigned char volume)
{
	if (useSound)
	{
		MusicVolume = volume;
		musicVolume = float(MusicVolume) / 256.0f;
		if (useMusic)
		{
			for (int slot = 0; slot < 2; ++slot)
			{
				if (musicSlots[slot].fade.active && musicSlots[slot].handle)
					gosAudio_SetChannelSlider( musicSlotChannel(slot),gosAudio_Volume, (digitalMasterVolume * musicVolume) );
			}
		}
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getMusicVolume(void)
{
	if (useSound)
	{
		return MusicVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setBettyVolume(unsigned char volume)
{
	if (useSound)
	{
		BettyVolume = volume;
		bettyVolume = float(BettyVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getBettyVolume(void)
{
	if (useSound)
	{
		return BettyVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::stopABLMusic (void)
{
	stopDigitalMusic();
}	

//---------------------------------------------------------------------------
void SoundSystem::playABLSFX (long sfxId)
{
	playDigitalSample(sfxId);
}	

//---------------------------------------------------------------------------
void SoundSystem::playABLDigitalMusic (long musicId)
{
	playDigitalMusic(musicId);

	// The script's view of its request is armed from the file's length on the scenario clock, with audio on or off: what a
	// script sees must never depend on the device (script_music.h). A request the stock code ignores is ignored here too.
	if ((musicId < 0) || (musicId >= numDMS) || (strncmp(digitalMusic[musicId].id.c_str(), "NONE", 4) == 0))
		return;

	FullPathFileName digitalStream;
	digitalStream.init(soundPath, digitalMusic[musicId].id.c_str(), ".wav");
	if (!fileExists(digitalStream))
		return;

	scriptMusic.request(musicId, scenarioTime, musicSeconds(static_cast<const char *>(digitalStream)), digitalMusic[musicId].loop);
}

//---------------------------------------------------------------------------
long SoundSystem::getCurrentMusicId (void)
{
	return scriptMusic.currentId(scenarioTime);
}

//---------------------------------------------------------------------------
// Seconds of audio in a music file from its RIFF header: the fmt and data chunk headers sit in the first few KB.
double SoundSystem::musicSeconds (std::string_view wavPath)
{
	unsigned char header[4096];
	File wav;
	if (wav.open(std::string(wavPath).c_str()) != NO_ERR)
		return 0.0;
	const long want = std::min<long>(static_cast<long>(wav.getLength()), static_cast<long>(sizeof(header)));
	const long got = (want > 0) ? wav.read(header, want) : 0;
	wav.close();
	return (got > 0) ? wavDurationSeconds(std::span<const unsigned char>(header, static_cast<std::size_t>(got))) : 0.0;
}

//---------------------------------------------------------------------------
// Video PCM adapter methods
//---------------------------------------------------------------------------
bool SoundSystem::queryNativeFormat(int* rate, int* channels) const
{
    Uint16 fmt; int ch; int r;
    if (Mix_QuerySpec(&r, &fmt, &ch) == 0) return false;
    if (rate)     *rate     = r;
    if (channels) *channels = ch;
    return true;
}

bool SoundSystem::beginVideoPCMStream(int rate, int channels)
{
    if (g_va.active) return false;
    Uint16 mixFmt = 0; int mixRate = 0, mixCh = 0;
    if (Mix_QuerySpec(&mixRate, &mixFmt, &mixCh) == 0) return false;
    if (mixFmt != AUDIO_S16SYS) return false;
    if (rate != mixRate || channels != mixCh) return false;

    g_va.rate = rate; g_va.channels = channels;
    g_va.cap  = static_cast<size_t>(rate) * channels * 2;  // 2 s ring buffer
    g_va.ring.assign(g_va.cap, 0);
    g_va.head = g_va.tail = 0;
    SDL_AtomicSet(&g_va.consumedFrames, 0);
    Mix_HookMusic(videoMixHook, nullptr);
    g_va.active = true;
    return true;
}

void SoundSystem::pushVideoPCMSamples(const int16_t* samples, int frameCount)
{
    if (!g_va.active || !samples || frameCount <= 0) return;
    int interleaved = frameCount * g_va.channels;
    SDL_AtomicLock(&g_va.lock);
    for (int i = 0; i < interleaved; ++i) {
        size_t next = (g_va.head + 1) % g_va.cap;
        if (next == g_va.tail) break;   // full — drop
        g_va.ring[g_va.head] = samples[i];
        g_va.head = next;
    }
    SDL_AtomicUnlock(&g_va.lock);
}

void SoundSystem::endVideoPCMStream()
{
    if (!g_va.active) return;
    Mix_HookMusic(nullptr, nullptr);
    g_va.active = false;
    g_va.ring.clear();
}

int SoundSystem::videoPCMSamplesConsumed() const
{
    return SDL_AtomicGet((&g_va.consumedFrames));
}
