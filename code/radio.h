//******************************************************************************************
//
//	Radio.h -- File contains the Radio Definition
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef RADIO_H
#define RADIO_H

//------------------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#include<cstdint>
#include<memory>
#include<vector>

#ifndef DRADIO_H
#include"dradio.h"
#endif

#ifndef DWARRIOR_H
#include"dwarrior.h"
#endif

#ifndef DGAMEOBJ_H
#include"dgameobj.h"
#endif

//------------------------------------------------------------------------------------------
inline constexpr int MAX_FRAGMENTS = 16;
inline constexpr int MAX_RADIOS = 256;
// One decoded wave clip of a radio message. `requested` separates "this slot was asked for but its packet would not
// load" (the pilot still keys the mike and the message goes on) from "this slot is unused".
struct RadioClip
{
	std::vector<uint8_t>	bytes;
	bool					requested = false;

	bool playable (void) const {return !bytes.empty();}
};

// One queued radio message; the queue and the player own it, and its clips die with it.
struct RadioData
{
	uint32_t        msgId = 0;
	uint32_t        msgType = 0;
	uint32_t        noiseId = 0;
	int32_t         numFragments = 0;
	RadioClip		data[MAX_FRAGMENTS];
	RadioClip		noise[MAX_FRAGMENTS];
	uint32_t        turnQueued = 0;
	byte			priority = 0;
	byte			movieCode = 0;
	float			expirationDate = 0.0f;
	MechWarriorPtr	pilot = nullptr;
};

// Radio packets are a few hundred KB at most; a larger packet size means a damaged file.
inline constexpr unsigned long MAX_RADIO_CLIP_BYTES = 16ul * 1024ul * 1024ul;

struct RadioMessageInfo
{
	byte			priority;
	float			shelfLife;
	char			movieCode;
	byte			styleCount;
	byte			styleChance[3];
	DWORD			messageMapping;
	bool			pilotIdentifiesSelf;
	bool			kludgeStyle;
};

//------------------------------------------------------------------------------------------
class Radio
{
	//Data Members
	//-------------
	protected:
	
		MechWarriorPtr	owner;
		bool			enabled;
		bool			ammoOutPlayed;
		long			radioID;

	public:		
		// Views of the radio packet files; GameSoundSystem owns them (radioNoiseFile / radioMessageFiles) and nulls these at purge.
		static PacketFilePtr 	noiseFile;
		static RadioPtr			radioList[MAX_RADIOS];		//Warriors no longer delete their radios.  We do when the
															//SoundSystem Shutdown occurs.

		static PacketFilePtr	messagesFile[MAX_RADIOS];	//Do these when the shutdown occurs too to avoid leaks
														
		static bool				messageInfoLoaded;
		static bool				radioListGo;
		static long				currentRadio;
	
	//Member Functions
	//-----------------
	public:
	

		void init (void)
		{
			enabled = TRUE;
			ammoOutPlayed = false;
			radioID = -1;
		}

		void destroy (void)
		{
		}

		Radio();
	
		~Radio (void)
		{
			destroy();
		}
	
		long init (char *fileName, unsigned long heapSize, char *movie);

		void setOwner (MechWarriorPtr _owner)
		{
			owner = _owner;
		}
				
		long playMessage (RadioMessageType msgId);

		void cancelMessage (RadioMessageType msgId);
		
		void turnOn (void) {
			enabled = TRUE;
		}

		void turnOff (void) {
			enabled = FALSE;
		}
		
		long loadMessageInfo(void);

		void resetAmmoMessage (void)
		{
			ammoOutPlayed = false;
		}
};

//------------------------------------------------------------------------------------------
#endif
