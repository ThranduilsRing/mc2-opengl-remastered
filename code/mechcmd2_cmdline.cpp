// Game main: command-line parsing and the GameOS environment setup.
// Declared in the class headers; shared names in mechcmd2_internal.h.
#include "mechcmd2_internal.h"


static long textToLong (char *num)
{
	long result = 0;
	
	char *hexOffset = strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = strlen(hexOffset)-1;
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = strlen(hexOffset)-1;
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}

	
	return(result);
}

//----------------------------------------------------------------------------
// Same command line Parser as MechCommander
void ParseCommandLine(const char *command_line)
{
	int i;
	int n_args = 0;
	int index = 0;
	char *argv[30];
	
	char tempCommandLine[4096];
	memset(tempCommandLine,0,4096);
	strncpy(tempCommandLine,command_line,4095);

	while (tempCommandLine[index] != '\0')  // until we null out
	{
		argv[n_args] = tempCommandLine + index;
		n_args++;
		while (tempCommandLine[index] != ' ' && tempCommandLine[index] != '\0')
		{
			index++;
		}
		while (tempCommandLine[index] == ' ')
		{
			tempCommandLine[index] = '\0';
			index++;
		}
	}

	i=0;
	while (i<n_args)
	{
		if (S_stricmp(argv[i],"-mission") == 0)
		{
			i++;
			if (i < n_args)
			{
				justStartMission = true;
				inViewMode = false;
				if (argv[i][0] == '"')
				{
					strcpy(missionName,&(argv[i][1]));
					bool scanName = true;
					while (scanName && (i < n_args))
					{
						i++;
						if (i < n_args)
						{
							strcat(missionName," ");
							strcat(missionName,argv[i]);

							if (strstr(argv[i],"\"") != nullptr)
							{
								scanName = false;
								missionName[strlen(missionName)-1] = 0;
							}
						}
						else
						{
							//
							scanName = false;
							missionName[strlen(missionName)-1] = 0;
						}
					}
				}
				else
					strcpy(missionName,argv[i]);
			}
		}
		else if (S_stricmp(argv[i],"-viewer") == 0)
		{
			i++;
			if (i < n_args)
			{
				inViewMode = true;
				justStartMission = true;
				viewObject = textToLong(argv[i]);
				strcpy(missionName,"mis0101");
			}
		}
		else if (S_stricmp(argv[i],"-nodialog") == 0)
		{
			gNoDialogs = true;
		}
		else if (S_stricmp(argv[i],"-sniffer") == 0)
		{
			SnifferMode = true;
		}
		else if (S_stricmp(argv[i], "-braindead") == 0) {
			i++;
			if (i < n_args) {
				long teamID = textToLong(argv[i]);
				MechWarrior::brainsEnabled[teamID] = false;
			}
		}
		else if (S_stricmp(argv[i], "-turrets_off") == 0) {
			i++;
			if (i < n_args) {
				long teamID = textToLong(argv[i]);
				Turret::turretsEnabled[teamID] = false;
			}
		}
		else if (S_stricmp(argv[i], "-debugwins") == 0) {
			i++;
			if (i < n_args) {
				long winState = textToLong(argv[i]);
				if (winState == 1)
					DebugWindowOpen[0] = true;
				else if (winState == 2) {
					DebugWindowOpen[1] = true;
					DebugWindowOpen[2] = true;
					DebugWindowOpen[3] = true;
					}
				else if (winState == 3) {
					DebugWindowOpen[0] = true;
					DebugWindowOpen[1] = true;
					DebugWindowOpen[2] = true;
					DebugWindowOpen[3] = true;
				}
			}
		}
		else if (S_stricmp(argv[i], "-objectwins") == 0) {
			i++;
			if (i < n_args) {
				long partNumber = textToLong(argv[i]);
				if (NumGameObjectsToDisplay < 3)
					GameObjectWindowList[NumGameObjectsToDisplay++] = partNumber;
			}
		}
		else if (S_stricmp(argv[i], "-debugcells") == 0) {
			i++;
			if (i < n_args) {
				long setting = textToLong(argv[i]);
				if ((setting > 0) && (setting < 5))
					DrawDebugCells = setting;
			}
		}
		else if (S_stricmp(argv[i], "-nopain") == 0) {
			i++;
			if (i < n_args) {
				long teamID = textToLong(argv[i]);
				Team::noPain[teamID] = true;
			}
		}
		else if (S_stricmp(argv[i], "-disable") == 0) {
			i++;
			if (i < n_args) {
				long partID = textToLong(argv[i]);
				DisableAtStart[NumDisableAtStart++] = partID;
			}
		}
		else if (S_stricmp(argv[i], "-log") == 0) {
			i++;
			initGameLogs = true;
			if (i < n_args) {
				if (S_stricmp(argv[i], "net") == 0)
					initNetLog = true;
				if (S_stricmp(argv[i], "weaponfire") == 0)
					initCombatLog = true;
				if (S_stricmp(argv[i], "bugs") == 0)
					initBugLog = true;
				if (S_stricmp(argv[i], "lrmove") == 0)
					initLRMoveLog = true;
			}
		}
		else if (S_stricmp(argv[i], "-show") == 0) {
			i++;
			if (i < n_args) {
				if (S_stricmp(argv[i], "movers") == 0)
					ShowMovers = true;
			}
		}
		else if (S_stricmp(argv[i], "-goalplan") == 0) {
			i++;
			if (i < n_args) {
				if (S_stricmp(argv[i], "enemies") == 0)
					EnemiesGoalPlan = true;
			}
		}
		else if (S_stricmp(argv[i], "-killambient") == 0) {
			KillAmbientLight = true;
		}
		else if (S_stricmp(argv[i], "-movegoals") == 0) {
			i++;
			if (i < n_args)
				MaxMoveGoalChecks = textToLong(argv[i]);
		}
		else if (S_stricmp(argv[i], "-rps") == 0) {
			i++;
			if (i < n_args)
				MaxResourcePoints = textToLong(argv[i]);
		}
		else if (S_stricmp(argv[i], "-registerzone") == 0) {
			MultiPlayer::registerZone = true;
		}
		else if (S_stricmp(argv[i], "-dropzones") == 0) {
			i++;
			if (i < n_args) {
				long numPlayers = strlen(argv[i]);
				for (long j = 0; j < numPlayers; j++)
					MultiPlayer::presetDropZones[j] = (argv[i][j] - '0');
			}
		}

		i++;
	}
}

bool notFirstTime = false;

//
//
void __stdcall GetGameOSEnvironment(const char* CommandLine )
{
	ParseCommandLine(CommandLine);

	if (SmokeMode::state().enabled) {
		if (!justStartMission) {
			justStartMission = true;
			strncpy(missionName, SmokeMode::state().mission.c_str(), sizeof(missionName) - 1);
			missionName[sizeof(missionName) - 1] = '\0';
		}

		// Preserve the smoke runner's existing milestone contract while using
		// the older direct mission-start path that bypasses logistics entirely.
		SmokeMode::emitTiming("profile_ready");
		SmokeMode::emitTiming("logistics_ready");
		SmokeMode::resolveMissionPaths();
		SmokeMode::emitTiming("mission_load_start");
	}

	if (getValidateConfig().enabled) {
		if (!justStartMission) {
			justStartMission = true;
			strcpy(missionName, "mis0101");
		}
		useSound = false;
	}

	Environment.applicationName			= "MechCommander2\\1.0"; // MP012001";

	Environment.debugLog				= "";
	Environment.spew					= "";
	Environment.TimeStampSpew			= 0;

	Environment.GetGameInformation		= GetGameInformation;
	Environment.UpdateRenderers			= UpdateRenderers;
	Environment.InitializeGameEngine	= InitializeGameEngine;
	Environment.DoGameLogic				= DoGameLogic;
	Environment.TerminateGameEngine		= TerminateGameEngine;

	Environment.Renderer				= 0;

	Environment.allowMultipleApps = false;
	Environment.dontClearRegistry = true;

    Environment.checkCDForFiles = false;  // 2001 CD DRM; always disk-installed now
	
	if (useSound)
	{
		Environment.soundDisable			= FALSE;
		Environment.soundHiFi				= TRUE;
		Environment.soundChannels			= 20;
	}
	else
	{
		Environment.soundDisable			= TRUE;
		Environment.soundHiFi				= FALSE;
		Environment.soundChannels			= 0;
	}

	//--------------------------
	// Networking information...
	// MechCommander GUID = {09608800-4815-11d2-92D2-0060973CFB2C}
	// or {0x9608800, 0x4815, 0x11d2, {0x92, 0xd2, 0x0, 0x60, 0x97, 0x3c, 0xfb, 0x2c}}
	Environment.NetworkGame = FALSE;
	Environment.NetworkMaxPlayers = MAX_COMMANDERS;
	Environment.NetworkGUID[0] = 0x09;
	Environment.NetworkGUID[1] = 0x60;
	Environment.NetworkGUID[2] = 0x88;
	Environment.NetworkGUID[3] = 0x00;
	Environment.NetworkGUID[4] = 0x48;
	Environment.NetworkGUID[5] = 0x15;
	Environment.NetworkGUID[6] = 0x11;
	Environment.NetworkGUID[7] = 0xd2;
	Environment.NetworkGUID[8] = 0x92;
	Environment.NetworkGUID[9] = 0xd2;
	Environment.NetworkGUID[10] = 0x00;
	Environment.NetworkGUID[11] = 0x60;
	Environment.NetworkGUID[12] = 0x97;
	Environment.NetworkGUID[13] = 0x3c;
	Environment.NetworkGUID[14] = 0xfb;
	Environment.NetworkGUID[15] = 0x2c;
	
	Environment.Key_FullScreen			= 0;
	Environment.Key_SwitchMonitors		= 0;
	Environment.Key_Exit				= 0;
	Environment.screenWidth				= 800;
	Environment.screenHeight			= 600;
	Environment.bitDepth				= 16;
	Environment.fullScreen				= 0;

#ifndef LINUX_BUILD
	HKEY hKey;
	LONG result;
	char pData[1024];
	DWORD szData = 1023;

	result=RegOpenKey(HKEY_CURRENT_USER,GAME_REG_KEY,&hKey);
	if( ERROR_SUCCESS==result )
	{
		result=RegQueryValueEx(hKey,"FIRSTRUN",nullptr,nullptr,(PBYTE)pData,&szData);
		if ((result == ERROR_SUCCESS) && pData[0])
		{
			if (fileExists("options.cfg") && !SnifferMode)
				Environment.fullScreen				= 1;
			EULAShown = true;
		}

		RegCloseKey(hKey);
	}
#endif

	Environment.version					= versionStamp;
	Environment.FullScreenDevice		= 0;
	Environment.AntiAlias				= 0;
	Environment.disableZBuffer			= 0;

//
//
	Environment.Texture_S_256			= 6;
	Environment.Texture_S_128			= 1;
	Environment.Texture_S_64			= 0;
	Environment.Texture_S_32			= 1;
	Environment.Texture_S_16			= 5;

	Environment.Texture_K_256			= 2;
	Environment.Texture_K_128			= 5;
	Environment.Texture_K_64			= 5;
	Environment.Texture_K_32			= 5;
	Environment.Texture_K_16			= 5;

	Environment.Texture_A_256			= 0;
	Environment.Texture_A_128			= 1;
	Environment.Texture_A_64			= 5;
	Environment.Texture_A_32			= 1;
	Environment.Texture_A_16			= 0;

	Environment.RaidDataSource			= "MechCommander 2:Raid4"; 
	Environment.RaidFilePath			= "\\\\aas1\\MC2\\Test\\GOSRaid";
	Environment.RaidCustomFields		= "Area=GOSRaid"; 	

	Environment.DisableLowEndCard		= 1;

	Environment.Suppress3DFullScreenWarning = 1;

	Environment.RenderToVram			= 1;

	notFirstTime = true;
}
