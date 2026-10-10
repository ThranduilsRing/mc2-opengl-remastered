// Game main: debug windows, dialog setup/teardown and their rendering.
// Declared in the class headers; shared names in mechcmd2_internal.h.
#include "mechcmd2_internal.h"



void DEBUGWINS_init (void) {

	sprintf(DebugStatusBarString, "DEBUG Status Bar: GLENNBA");

	DebugWindow[0] = new GameDebugWindow;
	DebugWindow[0]->init();
	DebugWindow[0]->setPos(10, 200);
	if (DebugWindowOpen[0])
		DebugWindow[0]->open();
	else
		DebugWindow[0]->close();

	DebugWindow[1] = new GameDebugWindow;
	DebugWindow[1]->init();
	DebugWindow[1]->setPos(10, 20);
	if (DebugWindowOpen[1])
		DebugWindow[1]->open();
	else
		DebugWindow[1]->close();

	DebugWindow[2] = new GameDebugWindow;
	DebugWindow[2]->init();
	DebugWindow[2]->setPos(220, 20);
	if (DebugWindowOpen[2])
		DebugWindow[2]->open();
	else
		DebugWindow[2]->close();

	DebugWindow[3] = new GameDebugWindow;
	DebugWindow[3]->init();
	DebugWindow[3]->setPos(430, 20);
	if (DebugWindowOpen[3])
		DebugWindow[3]->open();
	else
		DebugWindow[3]->close();
}


void DEBUGWINS_destroy (void) {

	for (long i = 0; i < NUM_DEBUG_WINDOWS; i++)
		if (DebugWindow[i]) {
			DebugWindow[i]->destroy();
			delete DebugWindow[i];
			DebugWindow[i] = nullptr;
		}
}	

void initDialogs()
{
	ZoneScopedN("initDialogs");
	FullPathFileName path;
	path.init( artPath, "mcl_dialog", ".fit" );

	FitIniFile file;
	{
		ZoneScopedN("initDialogs mcl_dialog open");
		if ( NO_ERR != file.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (const char*)path );
			Assert( 0, 0, error );
			return;
		}
	}
	{
		ZoneScopedN("initDialogs LogisticsOKDialog::init");
		LogisticsOKDialog::init( file );
	}
	file.close();

	path.init( artPath, "mcl_sm", ".fit" );

	{
		ZoneScopedN("initDialogs mcl_sm open");
		if ( NO_ERR != file.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (const char*)path );
			Assert( 0, 0, error );
			return;
		}
	}

	{
		ZoneScopedN("initDialogs LogisticsSaveDialog::init");
		LogisticsSaveDialog::init( file );
	}
	file.close();

	path.init( artPath, "mcl_dialog_onebutton", ".fit" );
	{
		ZoneScopedN("initDialogs mcl_dialog_onebutton open");
		if ( NO_ERR != file.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (const char*)path );
			Assert( 0, 0, error );
			return;
		}
	}

	{
		ZoneScopedN("initDialogs LogisticsOneButtonDialog::init");
		LogisticsOneButtonDialog::init( file );
	}
}

void endDialogs()
{
	if (LogisticsOKDialog::s_instance)
	{
		delete LogisticsOKDialog::s_instance;
		LogisticsOKDialog::s_instance = nullptr;
	}

	if (LogisticsSaveDialog::s_instance)
	{
		delete LogisticsSaveDialog::s_instance;
		LogisticsSaveDialog::s_instance = nullptr;
	}

	if (LogisticsOneButtonDialog::s_instance)
	{
		delete LogisticsOneButtonDialog::s_instance;
		LogisticsOneButtonDialog::s_instance = nullptr;
	}

	if (LogisticsLegalDialog::s_instance)
	{
		delete LogisticsLegalDialog::s_instance;
		LogisticsLegalDialog::s_instance = nullptr;
	}
}


void DEBUGWINS_toggle (bool* windowsOpen) {

	for (long i = 0; i < NUM_DEBUG_WINDOWS; i++)
		if (windowsOpen[i])
			DebugWindow[i]->toggle();
	DebugHelpOpen = windowsOpen[NUM_DEBUG_WINDOWS];
	DebugStatusBarOpen = windowsOpen[NUM_DEBUG_WINDOWS + 1];
	DebugScoreBoardOpen = windowsOpen[NUM_DEBUG_WINDOWS + 2];
}


void DEBUGWINS_display (bool* windowsOpen) {

	for (long i = 0; i < NUM_DEBUG_WINDOWS; i++) {
		if (windowsOpen[i]) {
			DebugWindowOpen[i] = true;
			DebugWindow[i]->open();
			}
		else {
			DebugWindowOpen[i] = false;
			DebugWindow[i]->close();
		}
	}
	DebugHelpOpen = windowsOpen[NUM_DEBUG_WINDOWS];
	DebugStatusBarOpen = windowsOpen[NUM_DEBUG_WINDOWS + 1];
	DebugScoreBoardOpen = windowsOpen[NUM_DEBUG_WINDOWS + 2];
}


void DEBUGWINS_print (const char* s, long window) {

	DebugWindow[window]->print(s);
}


void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj) {

	if (debugObj == -1) {
		DebugGameObject[2] = DebugGameObject[1];
		DebugGameObject[1] = DebugGameObject[0];
		debugObj = 0;
	}
	DebugGameObject[debugObj] = obj;
}


void DEBUGWINS_viewGameObject (long debugObj) {

	if (DebugGameObject[debugObj]) {
		Stuff::Vector3D newPos = DebugGameObject[debugObj]->getPosition();
		eye->setPosition(newPos,false);
	}
}


void DEBUGWINS_removeGameObject (GameObjectPtr obj) {

	for (long i = 0; i < 3; i++)
		if (DebugGameObject[i] == obj) {
			DebugGameObject[i] = nullptr;
			DebugWindow[1 + i]->clear();
		}

}


void DEBUGWINS_update (void) {

	for (long i = 0; i < 3; i++)
		if (DebugGameObject[i])
			DebugGameObject[i]->updateDebugWindow(DebugWindow[1 + i]);
}


void DEBUGWINS_renderSpecialWindows (void) {

	gos_TextSetAttributes(DebugWindow[0]->font, 0xffffffff, 1.0, true, true, false, false);
	gos_TextSetRegion(0, 0, Environment.screenWidth, Environment.screenHeight );
	gos_TextSetPosition(15, 10);
	if (DebugStatusBarOpen && DebugStatusBarString[0])
		gos_TextDraw(DebugStatusBarString);
	if (DebugScoreBoardOpen) {
		if (MPlayer) {
			long curY = Environment.screenHeight - 390;
			for (int i = 0; i < MPlayer->numTeams; i++) {
				char s[256];
				sprintf(s, "Team %d score = %d", i, MPlayer->teamScore[i]);
				gos_TextSetPosition(Environment.screenWidth - 380, curY);
				gos_TextDraw(s);
				curY += 10;
			}
			curY += 10;
			for (int i = 0; i < MAX_MC_PLAYERS; i++)
				if (MPlayer->playerInfo[i].commanderID > -1) {
					char s[256];
					sprintf(s, "Player %d (%s) score = %d, %d kills, %d losses",
						i,
						MPlayer->playerInfo[i].name,
						MPlayer->playerInfo[i].score,
						MPlayer->playerInfo[i].kills,
						MPlayer->playerInfo[i].losses);
					gos_TextSetPosition(Environment.screenWidth - 380, curY);
					gos_TextDraw(s);
					curY += 10;
				}
			}
		else {
			gos_TextSetPosition(Environment.screenWidth - 275, Environment.screenHeight - 120);
			gos_TextDraw("Single-player Mission");
		}
	}
	if (DebugHelpOpen) {
		gos_TextSetPosition(15, 10);
		gos_TextDraw("DEBUGGING COMMANDS");
		gos_TextSetPosition(15, 20);
		gos_TextDraw("----------------------------------------------------------------------------");

		gos_TextSetPosition(15, 30);
		gos_TextDraw("ALT-W");
		gos_TextSetPosition(140, 30);
		gos_TextDraw("Toggle Debug Windows");

		gos_TextSetPosition(15, 40);
		gos_TextDraw("ALT-T");
		gos_TextSetPosition(140, 40);
		gos_TextDraw("Teleport to mouse");

		gos_TextSetPosition(15, 50);
		gos_TextDraw("ALT-/");
		gos_TextSetPosition(140, 50);
		gos_TextDraw("Quick Info");

		gos_TextSetPosition(15, 60);
		gos_TextDraw("ALT-SHFT-/");
		gos_TextSetPosition(140, 60);
		gos_TextDraw("Select Debug Object");

		gos_TextSetPosition(15, 70);
		gos_TextDraw("ALT-#");
		gos_TextSetPosition(140, 70);
		gos_TextDraw("Center on Debug Object # (1 thru 3)");

		gos_TextSetPosition(15, 80);
		gos_TextDraw("ALT-CTRL-#");
		gos_TextSetPosition(140, 80);
		gos_TextDraw("Toggle Info for Debug Object (1 thru 3)");

		gos_TextSetPosition(15, 90);
		gos_TextDraw("ALT-M");
		gos_TextSetPosition(140, 90);
		gos_TextDraw("Show All Movers (toggle)");

		gos_TextSetPosition(15, 100);
		gos_TextDraw("ALT-G");
		gos_TextSetPosition(140, 100);
		gos_TextDraw("GlobalMap Move Log (toggle)");

		gos_TextSetPosition(15, 110);
		gos_TextDraw("ALT-B");
		gos_TextSetPosition(140, 110);
		gos_TextDraw("Enemies Brain Dead (toggle)");

		gos_TextSetPosition(15, 120);
		gos_TextDraw("ALT-P");
		gos_TextSetPosition(140, 120);
		gos_TextDraw("Use Goal Planning (toggle on selected movers)");

		gos_TextSetPosition(15, 130);
		gos_TextDraw("ALT-CTRL-P");
		gos_TextSetPosition(140, 130);
		gos_TextDraw("Computer AI Use Goal Planning (toggle)");

		gos_TextSetPosition(15, 140);
		gos_TextDraw("ALT-4");
		gos_TextSetPosition(140, 140);
		gos_TextDraw("Damage Object for 1 point");

		gos_TextSetPosition(15, 150);
		gos_TextDraw("ALT-5");
		gos_TextSetPosition(140, 150);
		gos_TextDraw("Damage Object for 4 points");

		gos_TextSetPosition(15, 160);
		gos_TextDraw("ALT-6");
		gos_TextSetPosition(140, 160);
		gos_TextDraw("Damage Object for 9 points");

		gos_TextSetPosition(15, 170);
		gos_TextDraw("ALT-7");
		gos_TextSetPosition(140, 170);
		gos_TextDraw("Damage Object for 16 points");

		gos_TextSetPosition(15, 180);
		gos_TextDraw("ALT-8");
		gos_TextSetPosition(140, 180);
		gos_TextDraw("Damage Object for 25 points");

		gos_TextSetPosition(15, 190);
		gos_TextDraw("ALT-9");
		gos_TextSetPosition(140, 190);
		gos_TextDraw("Damage Object for 36 points");

		gos_TextSetPosition(15, 200);
		gos_TextDraw("ALT-0");
		gos_TextSetPosition(140, 200);
		gos_TextDraw("Disable Object");

		gos_TextSetPosition(15, 210);
		gos_TextDraw("ALT-V");
		gos_TextSetPosition(140, 210);
		gos_TextDraw("Display TacOrder Target in Debug Window");
	}
}


void DEBUGWINS_render (void) {

	for (long i = 0; i < NUM_DEBUG_WINDOWS; i++)
		DebugWindow[i]->render();
	DEBUGWINS_renderSpecialWindows();
}
