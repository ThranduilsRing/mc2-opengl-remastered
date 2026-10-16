//******************************************************************************************
//
//	Radio.cpp -- File contains the Radio class functions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//------------------------------------------------------------------------------------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef RADIO_H
#include"radio.h"
#endif

#include"../GameOS/gameos/utils/mc2log.h"

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#include"appearance_rng.h"
#endif

//------------------------------------------------------------------------------------------
// Macro Definitions
extern bool useSound;

PacketFilePtr 		Radio::noiseFile = nullptr;
RadioPtr	  		Radio::radioList[MAX_RADIOS];	//Warriors no longer delete their radios.
bool				Radio::radioListGo = false;
bool				Radio::messageInfoLoaded = false;
long				Radio::currentRadio = 0;
UserHeapPtr			Radio::radioHeap = nullptr;
PacketFilePtr		Radio::messagesFile[MAX_RADIOS];

RadioMessageInfo	messageInfo[RADIO_MESSAGE_COUNT];

//------------------------------------------------------------------------------------------
// Class Radio
//------------------------------------------------------------------------------------------
void *Radio::operator new (size_t mySize)
{
	if (!radioListGo)
	{
		radioListGo = true;
		for (long i=0;i<MAX_RADIOS;i++)
		{
			radioList[i] = nullptr;
			messagesFile[i] = nullptr;
		}
		
		currentRadio = 0;
		
		radioHeap = new UserHeap;
		radioHeap->init((4096 * 51)-1,"Radios");
		radioHeap->setMallocFatals(false);
	}

	void *result = radioHeap->Malloc(mySize);
	return result;
}
 
//-------------------------------------------------------------------------------
void Radio::operator delete (void *us)
{
	radioHeap->Free(us);
}

//-------------------------------------------------------------------------------
long Radio::init (char *fileName, unsigned long heapSize, char *movie)
{
	FullPathFileName pilotAudioPath;
	pilotAudioPath.init(CDsoundPath,fileName,".pak");

	FullPathFileName noisePath;
	noisePath.init(CDsoundPath,"noise",".pak");

	//--------------------------------------
	// Startup the packet file.
	radioID = currentRadio;
	// The sound system owns the files (it outlives every radio and frees them at purge); the statics are views for the radios.
	soundSystem->radioMessageFiles[radioID] = std::make_unique<PacketFile>();
	messagesFile[radioID] = soundSystem->radioMessageFiles[radioID].get();
		
	long result = messagesFile[radioID]->open(pilotAudioPath);
	gosASSERT(result == NO_ERR);
	
	if (!noiseFile)
	{
		//--------------------------------------
		// Startup the Noise packet file.
		soundSystem->radioNoiseFile = std::make_unique<PacketFile>();
		noiseFile = soundSystem->radioNoiseFile.get();
			
		result = noiseFile->open(noisePath);
		gosASSERT(result == NO_ERR);
	}
		
	// load message parameters
	if (!messageInfoLoaded)
	{
		if (loadMessageInfo() == NO_ERR)
			messageInfoLoaded = TRUE;
		else
			Fatal(0, "Unable to load message info.");
	}
	
	radioList[currentRadio] = this;
	currentRadio++;
	
	return(NO_ERR);
}
		
constexpr int NO_PLAY = -1;
// Reads packet `id` of `file` into `clip`. The clip counts as requested either way; it stays empty when the packet is
// oversized or unreadable, and the message then plays without that sample.
static bool loadRadioClip (PacketFilePtr file, long id, RadioClip& clip)
{
	clip.requested = true;
	unsigned long size = file->getPacketSize();
	if (size == 0 || size > MAX_RADIO_CLIP_BYTES)
	{
		MC2_LOG(Audio, Warn, "radio: packet {} has a bad size ({} bytes); the message plays without it", id, size);
		return false;
	}

	clip.bytes.resize(size);
	if (!file->readPacket(id,clip.bytes.data()))
	{
		MC2_LOG(Audio, Warn, "radio: packet {} could not be read; the message plays without it", id);
		clip.bytes.clear();
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------------------
long Radio::playMessage (RadioMessageType msgType)
{
long i, roll, callsign, fragmentNum, dropOut = 0;

	if (!useSound)
		return(NO_PLAY);
	
	if (!enabled)
		return(NO_PLAY);

	if (!owner)
		return(NO_PLAY);

	if (!soundSystem->checkMessage(owner, messageInfo[msgType].priority, msgType))
		return(NO_PLAY);

	if ((msgType == RADIO_AMMO_OUT) && ammoOutPlayed)
		return(NO_PLAY);
	
	if (messageInfo[msgType].styleCount > 1)
	{
		roll = mc2_appearance_rng::randomNumber(100);	// which style of message to play: presentation, so the appearance stream
		for (i=0; i<messageInfo[msgType].styleCount; i++)
		{
			dropOut += messageInfo[msgType].styleChance[i];
			if (roll < dropOut)
				break;
		}
		
		if (i != 0 && i == messageInfo[msgType].styleCount)
			return NO_PLAY;
			
		if (messageInfo[msgType].messageMapping + i == owner->getLastMessage())
			i++;
			
		if (i >= messageInfo[msgType].styleCount)
			i = 0;
	}
	else
		i = 0;

	auto msgData = std::make_unique<RadioData>();

	msgData->noiseId = SHORT_STATIC;
	msgData->msgType = msgType;
	msgData->msgId = messageInfo[msgType].messageMapping + i;
	msgData->movieCode = messageInfo[msgType].movieCode;
	msgData->turnQueued = turn;
			
	msgData->priority = messageInfo[msgType].priority;
	msgData->pilot = owner;
	msgData->expirationDate = scenarioTime + messageInfo[msgType].shelfLife;
	
	//-----------------------------------------------------------------------
	// Load the pieces need for playback.
	callsign = 0;
	fragmentNum = 0;
	if (messageInfo[msgType].pilotIdentifiesSelf)
	{
		if (messageInfo[RADIO_CALLSIGN].styleCount > 1)
		{
			roll = mc2_appearance_rng::randomNumber(100);	// which callsign style: presentation, so the appearance stream
			for (i=0; i<messageInfo[RADIO_CALLSIGN].styleCount; i++)
			{
				dropOut += messageInfo[RADIO_CALLSIGN].styleChance[i];
				if (roll < dropOut)
					break;
			}
			
			if (i != 0 && i == messageInfo[RADIO_CALLSIGN].styleCount)
				callsign = 0;
				
			if (i >= messageInfo[RADIO_CALLSIGN].styleCount)
				callsign = 0;
		}
		else
			callsign = 0;

		callsign += messageInfo[RADIO_CALLSIGN].messageMapping;
	}
	
	if (callsign)
	{
		if (messagesFile[radioID]->seekPacket(callsign) == NO_ERR)
		{
			loadRadioClip(messagesFile[radioID],callsign,msgData->data[fragmentNum]);
			fragmentNum++;
		}
	}

	if (messagesFile[radioID]->seekPacket(msgData->msgId) == NO_ERR)
	{
		loadRadioClip(messagesFile[radioID],msgData->msgId,msgData->data[fragmentNum]);

		if (noiseFile->seekPacket(msgData->noiseId) == NO_ERR)
			loadRadioClip(noiseFile,msgData->noiseId,msgData->noise[0]);
	}
	
	// A full queue refuses the message; it is dropped here instead of playing late.
	const long playedMsgId = msgData->msgId;
	if (soundSystem->queueRadioMessage(std::move(msgData)) != NO_ERR)
		return (NO_PLAY);
	
	if (msgType == RADIO_AMMO_OUT)
		ammoOutPlayed = true;

	return(playedMsgId);
}

//------------------------------------------------------------------------------------------
long Radio::loadMessageInfo(void)
{
FullPathFileName	messageInfoPath;
File				messageInfoFile;
long				result;
char				dataLine[512];
char*				field;

	messageInfoPath.init(soundPath,"radio",".csv");
	result = messageInfoFile.open(messageInfoPath);
	if (result != NO_ERR)
		return result;

	messageInfoFile.readLine((MemoryPtr)dataLine, 511);	// skip title line

	for (long i=0; i<RADIO_MESSAGE_COUNT; i++)
	{
		result = messageInfoFile.readLine((MemoryPtr)dataLine, 511);
		if (!result)
			Fatal(0, "Bad Message Info File");

		field = strtok(dataLine, ",");	// get past command name
		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].priority = atoi(field);
		else
			messageInfo[i].priority = 4;

		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].shelfLife = atoi(field);
		else
			messageInfo[i].shelfLife = 0;

		field = strtok(nullptr, ",");
		if (field && *field != 'x')
			messageInfo[i].movieCode = *field;
		else
			messageInfo[i].movieCode = '\0';

		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].styleCount = atoi(field);
		else
			messageInfo[i].styleCount = 1;

		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].styleChance[0] = atoi(field);
		else
			messageInfo[i].styleChance[0] = 0;
		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].styleChance[1] = atoi(field);
		else
			messageInfo[i].styleChance[1] = 0;
		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].styleChance[2] = atoi(field);
		else
			messageInfo[i].styleChance[2] = 0;

		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].pilotIdentifiesSelf = (*field == 'y');
		else
			messageInfo[i].pilotIdentifiesSelf = FALSE;

		field = strtok(nullptr, ",");
		if (field)
			messageInfo[i].messageMapping = atoi(field);
		else
			messageInfo[i].messageMapping = 0;
		
		field = strtok(nullptr, ",");
		if (field)
		{
			field = strtok(nullptr, ",");
			if (field)
			{
				messageInfo[i].kludgeStyle = (*field == 'x');
			}
		}
		if (!field)
			messageInfo[i].kludgeStyle = FALSE;
	}
	messageInfoFile.close();
	return NO_ERR;
}
