//---------------------------------------------------------------------------
// GameSound.cpp - This file is the sound system code for the GAME
//
// MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files
#ifndef GAMESOUND_H
#include"gamesound.h"
#include "../gui/sound.h"

#include"../GameOS/gameos/utils/mc2log.h"
#endif

#ifndef WARRIOR_H
#include"warrior.h"
#endif

#ifndef VIEWER
#ifndef CONTROLGUI_H
#include"controlgui.h"
#endif 
#endif

//---------------------------------------------------------------------------
GameSoundSystem *soundSystem = nullptr;
namespace mc2::gui {
void playSound(long sampleId) {
    if (soundSystem) soundSystem->playDigitalSample(sampleId);
}
}

bool friendlyDestroyed = false;
bool enemyDestroyed = false;
extern bool GeneralAlarm;

constexpr float ALARM_TIME = 5.0f;

//---------------------------------------------------------------------------
void GameSoundSystem::purgeSoundSystem (void)
{
	wholeMsgDone = true;
	currentMessage = nullptr;
	messagesInQueue = 0;
	currentFragment = 0;
	playingNoise = false;
	gosAudio_DestroyResource(&radioHandle);
	//------------------------------------------------------------
	// dump the Radio Message Queue.
	messagesInQueue = 0;
	wholeMsgDone = true;
	for (auto & queueElem : queue)
		queueElem = nullptr;

	generalAlarmTimer = 0.0f;
			
	//---------------------------------------------------------
	// Toss everybody's radio.  This MUST happen AFTER
	// all of the currently queued radio messages are dumped.
	// The warriors are already gone (Mission::destroy shuts them down first), so free each Radio here,
	// exactly once, before releasing the packet files they referenced.
	for (long i=0;i<MAX_RADIOS;i++)
	{
		delete Radio::radioList[i];
		Radio::radioList[i] = nullptr;
		radioMessageFiles[i].reset();
		Radio::messagesFile[i] = nullptr; 
	}

		
	radioNoiseFile.reset();
	Radio::noiseFile = nullptr;
	// Support archive views die with the radios, before their preloaded owners.
	extern void releaseSupportRadioPreloads();
	releaseSupportRadioPreloads();

	Radio::currentRadio = 0;
	Radio::radioListGo = FALSE;
	Radio::messageInfoLoaded = FALSE;

	isRaining = oldRaining = false;

	largestSensorContact = -1;
	//-----------------------------------------------
	// Check all samples to see if one should end.
	for (long i=0;i<MAX_DIGITAL_SAMPLES;i++)
	{
		if (i != BETTY_CHANNEL)
		{
			gosAudio_SetChannelPlayMode(i, gosAudio_Stop);
			fadeDown[i] = FALSE;
			channelSampleId[i] = -1;
			channelInUse[i] = FALSE;
		}
	}
}	

//---------------------------------------------------------------------------
void GameSoundSystem::removeQueuedMessage (long msgNumber)
{
long i;

	if (msgNumber < 0 || msgNumber >= MAX_QUEUED_MESSAGES)
		return;

	if (queue[msgNumber])
	{
		if (queue[msgNumber]->pilot)
			queue[msgNumber]->pilot->clearMessagePlaying();

		queue[msgNumber].reset();
		if (messagesInQueue > 0)
			messagesInQueue--;

#ifdef _DEBUG
		for (long test=0;test<static_cast<long>(messagesInQueue);test++)
		{
			if (queue[test])
				continue;
			else
				Fatal(-1," Bad Message in Queue -- RmoveQMsg");
		}
#endif

		for (i=msgNumber;i<MAX_QUEUED_MESSAGES-1;i++)
			queue[i] = std::move(queue[i+1]);
		queue[MAX_QUEUED_MESSAGES-1] = nullptr;

#ifdef _DEBUG
		for (long test=0;test<static_cast<long>(messagesInQueue);test++)
		{
			if (queue[test])
				continue;
			else
				Fatal(-1," Bad Message in Queue -- RmoveQMsg");
		}
#endif

	}
}

//---------------------------------------------------------------------------
bool GameSoundSystem::checkMessage (MechWarriorPtr pilot, byte priority, unsigned long messageType)
{
	for (const auto & queueElem : queue)
	{
		if (queueElem)
			if ((queueElem->pilot == pilot && priority > queueElem->priority) ||			// I'm already saying something more important, or
					(queueElem->priority > 1 && queueElem->msgType == messageType))		// someone else is saying this
				return FALSE;
	}
	return TRUE;
}

//---------------------------------------------------------------------------
void GameSoundSystem::moveFromQueueToPlaying(void)
{
	removeCurrentMessage();

	while (queue[0] && 
			queue[0]->pilot && 
			!(queue[0]->pilot->active()) &&
			(queue[0]->msgType != RADIO_DEATH) &&
			(queue[0]->msgType != RADIO_EJECTING))
	{
		removeQueuedMessage(0);
	}

	currentMessage = std::move(queue[0]);

	for (long i=0;i<MAX_QUEUED_MESSAGES-1;i++)
		queue[i] = std::move(queue[i+1]);
		
	queue[MAX_QUEUED_MESSAGES-1] = nullptr;
	if (messagesInQueue > 0)
		messagesInQueue--;
}

//---------------------------------------------------------------------------
void GameSoundSystem::removeCurrentMessage(void)
{
	if (currentMessage)
	{
		if (currentMessage->pilot)
			currentMessage->pilot->clearMessagePlaying();

		currentMessage = nullptr;
	}

	gosAudio_SetChannelPlayMode(RADIO_CHANNEL, gosAudio_Stop);
	gosAudio_DestroyResource(&radioHandle);
	wholeMsgDone = TRUE;
}

//---------------------------------------------------------------------------
long GameSoundSystem::queueRadioMessage (std::unique_ptr<RadioData> msgData)
{
long i;

	//-------------------------------------------------
	// First, search the Queue and see if this message
	// was already sent this turn.
	if (msgData->msgId >= MSG_TOTAL_MSGS)	// is this a real message? (why are we asking this???)
	{
		for (i=MAX_QUEUED_MESSAGES-1;i>=0;i--)
		{
			if (queue[i])
			{
				if ((msgData->turnQueued == queue[i]->turnQueued) &&
					(msgData->msgId == queue[i]->msgId))
				{
					removeQueuedMessage(i);
				}
			}
		}
	}

	if (msgData->priority == 0)	// top cancels playing message! top also means pilot's gone, so
	{							// remove other messages from that pilot.
		removeCurrentMessage();
		for (i=MAX_QUEUED_MESSAGES-1;i>=0;i--)
		{
			if (queue[i] && queue[i]->pilot == msgData->pilot)
			{
				removeQueuedMessage(i);
			}
		}
	}

	for (i=0; i<MAX_QUEUED_MESSAGES;i++)
	{
		if (!queue[i])
			break;

		if (queue[i]->priority > msgData->priority)	// if this messages priority higher (a lower number: 1 is top priority)
		{											// push things down to make room.
			for (long j=MAX_QUEUED_MESSAGES-1;j>i;j--)
			{
				queue[j] = std::move(queue[j-1]);
			}
			break;
		}
	}
	
	if (i == MAX_QUEUED_MESSAGES)
		return(-1);

	if (queue[MAX_QUEUED_MESSAGES-1])
		removeQueuedMessage(MAX_QUEUED_MESSAGES-1);

	queue[i] = std::move(msgData);

#ifdef _DEBUG
		for (long test=0;test<static_cast<long>(messagesInQueue);test++)
		{
			if (queue[test])
				continue;
			else
				Fatal(-1," Bad Message in Queue -- RmoveQMsg");
		}
#endif
	messagesInQueue++;
#ifdef _DEBUG
		for (long test=0;test<static_cast<long>(messagesInQueue);test++)
		{
			if (queue[test])
				continue;
			else
				Fatal(-1," Bad Message in Queue -- RmoveQMsg");
		}
#endif

	return NO_ERR;
}

//---------------------------------------------------------------------------
// Hands one decoded clip to the radio channel. A clip whose wave header cannot be read is skipped with a log line, so a
// damaged radio file silences that sample instead of reading past the buffer.
bool GameSoundSystem::playClip (const RadioClip& clip)
{
	gosAudio_DestroyResource(&radioHandle);
	if (!createWaveResource(&radioHandle,clip.bytes))
		return false;
	
	if (isChannelPlaying(BETTY_CHANNEL))
		gosAudio_SetChannelSlider(RADIO_CHANNEL, gosAudio_Volume, SoundSystem::digitalMasterVolume * radioVolume * 0.5f);
	else
		gosAudio_SetChannelSlider(RADIO_CHANNEL, gosAudio_Volume, SoundSystem::digitalMasterVolume * radioVolume);

	gosAudio_AssignResourceToChannel(RADIO_CHANNEL, radioHandle);
	gosAudio_SetChannelPlayMode(RADIO_CHANNEL,gosAudio_PlayOnce);
	return true;
}

//---------------------------------------------------------------------------
// The static before a fragment. A noise sample that failed to load still keys the mike, so the message goes on to its speech.
void GameSoundSystem::playNoiseClip (void)
{
	const RadioClip& noise = currentMessage->noise[currentFragment];
	if (noise.playable())
		playClip(noise);

	if (currentMessage->pilot)
		currentMessage->pilot->setMessagePlaying();
}

//---------------------------------------------------------------------------
// The speech of the current fragment; a fragment without playable speech ends the message.
void GameSoundSystem::playNextDataClip (void)
{
	if (currentMessage->data[currentFragment].playable() && playClip(currentMessage->data[currentFragment]))
	{
		if (currentMessage->pilot)
			currentMessage->pilot->setMessagePlaying();
	}
	else
		wholeMsgDone = TRUE;
}

//---------------------------------------------------------------------------
void GameSoundSystem::update (void)
{
	//---------------------------------------------
	// Dynamic Music Code goes here!!!
	// New System(tm)
	//
	
	SoundSystem::update();

	if (GeneralAlarm && (generalAlarmTimer < ALARM_TIME))
	{
		playDigitalSample(BUZZER1);
		generalAlarmTimer += frameLength;
	}

	const bool speechBusy = isPlayingVoiceOver() || isChannelPlaying(BETTY_CHANNEL) || isChannelPlaying(SUPPORT_CHANNEL);
	if (useSound && currentMessage && !speechBusy && (gosAudio_GetChannelPlayMode(RADIO_CHANNEL) != gosAudio_PlayOnce))
	{
		if (radioHandle)
		{
			gosAudio_DestroyResource(&radioHandle);		//Toss the current radio data which just completed!
			radioHandle = nullptr;
		}

		if (!wholeMsgDone)
		{
			//---------------------------------------------------------------
			// We are finished with a fragment, move on.
			if (playingNoise)
			{
				playingNoise = FALSE;
				playNextDataClip();
				currentFragment++;
			}
			else
			{
				if (currentMessage->noise[currentFragment].requested)
				{
					playNoiseClip();
					playingNoise = TRUE;
				}
				else
				{
					playingNoise = FALSE;
					playNextDataClip();
					currentFragment++;
				}
			}
		}
		else	// wholeMessageDone
		{
			if (currentMessage)
			{
				if (currentMessage->pilot)
				{
					currentMessage->pilot->clearMessagePlaying();
#ifndef VIEWER
					ControlGui::instance->endPilotVideo();
#endif
				}

				removeCurrentMessage();
			}
		}
	}
		
	if (useSound && messagesInQueue && wholeMsgDone && !speechBusy)
	{
		if (radioHandle)
		{
			gosAudio_DestroyResource(&radioHandle);		//Toss the current radio data which just completed!
			radioHandle = nullptr;
		}
		
		//-----------------------------------------------------
		// We are done with the current sample, start the next
		currentFragment = 0;
		moveFromQueueToPlaying();

		if (!currentMessage && !messagesInQueue)		//It is now possible for ALL remaining messages to go away because the pilot/mech died!
			return;

#ifndef VIEWER
		if ( currentMessage->movieCode )
		{
			ControlGui::instance->playPilotVideo( 
				currentMessage->pilot, currentMessage->movieCode );
		}
#endif

		wholeMsgDone = FALSE;
		if (currentMessage->noise[currentFragment].requested)
		{
			playNoiseClip();
			playingNoise = TRUE;
		}
		else
		{
			playingNoise = FALSE;
			// Consume the first speech fragment just as after an opening noise clip.
			playNextDataClip();
			currentFragment++;
		}
	}
}
// End of radio playback


