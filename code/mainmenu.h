#ifndef MAINMENU_H
#define MAINMENU_H
#include "mc2_czstring.h"
#include <memory>
#include <string_view>
/*************************************************************************************************\
MainMenu.h			: Interface for the MainMenu component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef LOGISTICSSCREEN_H
#include"logisticsscreen.h"
#endif

#ifndef AANIM_H
#include"aanim.h"
#endif

#ifndef MPLOADMAP_H
#include"mploadmap.h"
#endif

#ifndef MC2movie_H
#include"mc2movie.h"
#endif


class OptionsScreenWrapper;
class Mechlopedia;
//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MainMenu:
**************************************************************************************************/
class SplashIntro : public LogisticsScreen
{
public:

	SplashIntro(){}
	virtual ~SplashIntro(){}

	void init();

};


class MainMenu: public LogisticsScreen
{
	public:

		MainMenu();
		virtual ~MainMenu();


		int init( FitIniFile& file );

		virtual void begin();
		virtual void end();
		virtual void update();
		virtual void render();

		void setHostLeftDlg( mc2::czstring playerName );

		void setDrawBackground( bool bDrawBackground );
		void skipIntro();

		// True while this menu fully covers whatever logistics screen sits
		// behind it (intro movie playing, or steady-state splash menu with
		// its opaque background up).  Legacy code relied on painter's-order
		// GameOS draws to hide the screen behind; the data/defs UI path
		// composites through ImGui AFTER all GameOS draws, so callers must
		// skip rendering occluded screens instead.
		bool occludesLogisticsScreens() const;

		virtual int	handleMessage( unsigned long, unsigned long );
		

		static	bool	bDrawMechlopedia;
		static	bool	bLoadShown;   // capture harness: the Load Game dialog is fully shown

	private:

		MainMenu& operator=( const MainMenu& ainMenu );
		MainMenu( const MainMenu& src );



		bool	bDrawBackground;

		LogisticsScreen	background;

		bool	promptToQuit;
		bool	bOptions;
		bool	bSave;
		bool	bLoad;
		bool	bLoadSingle;
		bool	bLoadCampaign;
		bool	bRulesDlg;				// the Game Rules dialog is up between the campaign picker and startNewCampaign
		std::string rulesCampaign;		// the campaign picked in the picker
		bool	beginCampaignRules( std::string_view campaignFile );	// true = dialog shown; false = caller starts the campaign now
		bool	promptToDisconnect;
		bool	bLegal;

		long	tuneId;			//What music should I play here!
		bool	musicStarted;	//Should I restart the tune?

		long	endResult;

		aAnimation	beginAnim;
		aAnimation	endAnim;

		std::unique_ptr<OptionsScreenWrapper>	optionsScreenWrapper;
		Mechlopedia*			mechlopedia;
		SplashIntro				intro;
		bool					introOver;
		MPLoadMap				singleLoadDlg;
		bool					bHostLeftDlg;

		MC2MoviePtr			introMovie;
			
		

};


//*************************************************************************************************
#endif  // end of file ( MainMenu.h )
