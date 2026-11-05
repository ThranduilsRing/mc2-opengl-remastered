//---------------------------------------------------------------------------
// GameSound.h - This file is the sound system header for the GAME
//
// MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef GAMESOUND_H
#define GAMESOUND_H
//---------------------------------------------------------------------------
// Include files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef DWARRIOR_H
#include"dwarrior.h"
#endif

#ifndef RADIO_H
#include"radio.h"
#endif

#include<memory>

//---------------------------------------------------------------------------
// Macro Defintiions

//---------------------------------------------------------------------------
class GameSoundSystem : public SoundSystem
{
	//Data Members
	//-------------
	protected:
		std::unique_ptr<RadioData>	currentMessage;						//Radio message playing.
		unsigned long		messagesInQueue;						//Radio messages waiting to play.
		std::unique_ptr<RadioData>	queue[MAX_QUEUED_MESSAGES];			//Radio message queue.
		unsigned long		currentFragment;						//Which piece are we playing.
		unsigned long		playingNoise;							//are we playing noise right now?
		bool 				wholeMsgDone;							//Are all fragments played?
		HGOSAUDIO			radioHandle;

		float				generalAlarmTimer;						//How long do we play the alarm buzzer?

	public:
		// The radio packet files. Radio::init fills them; purgeSoundSystem resets them (after the radios release their packet references).
		std::unique_ptr<PacketFile>	radioNoiseFile;
		std::unique_ptr<PacketFile>	radioMessageFiles[MAX_RADIOS];
	protected:
			
	//Member Functions
	//----------------
	public:
	
		GameSoundSystem (void)
		{
			init();
		}
		
		~GameSoundSystem (void)
		{
			destroy();
		}
		
        void destroy() override {
            gosAudio_DestroyResource(&radioHandle);
            SoundSystem::destroy();
        }

		void init (void)
		{
			SoundSystem::init();

			wholeMsgDone = true;
			currentMessage = nullptr;
			messagesInQueue = 0;
			currentFragment = 0;
			playingNoise = false;
			radioHandle = nullptr;

			//------------------------------------------------------------
			// Startup the Radio Message Queue.
			messagesInQueue = 0;
			wholeMsgDone = true;
			for (auto & i : queue)
				i = nullptr;

			generalAlarmTimer = 0.0f;
		}
		
		virtual void update (void);

		void purgeSoundSystem (void);					//This will shutdown all active sound.
		void removeQueuedMessage (long msgNumber);
		bool checkMessage (MechWarriorPtr pilot, byte priority, unsigned long messageType);
		long queueRadioMessage (std::unique_ptr<RadioData> msgData);
		void moveFromQueueToPlaying (void);
		void removeCurrentMessage (void);

	protected:
		bool playClip (const RadioClip& clip);			//Starts one clip on the radio channel; false when its wave header is unreadable.
		void playNoiseClip (void);						//The static before the current fragment.
		void playNextDataClip (void);					//The speech of the current fragment, or the end of the message.
};

//---------------------------------------------------------------------------
extern GameSoundSystem *soundSystem;

extern bool useSound;
extern bool useMusic;

//---------------------------------------------------------------------------
#endif
