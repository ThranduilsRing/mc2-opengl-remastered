#include <string>
#ifndef MPPARAMETERSCREEN_H
#define MPPARAMETERSCREEN_H
#include "mc2_czstring.h"
/*************************************************************************************************\
MPParameterScreen.h			: Interface for the MPParameterScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************

#ifndef LOGISTICSSCREEN_H
#include"logisticsscreen.h"
#endif

#include"asystem.h"
#include"alistbox.h"
#include"attributemeter.h"
#include"simplecamera.h"
#include <memory>
#include <vector>
#include"multplyr.h"
#include"mploadmap.h"

#ifndef AANIM_H
#include"aanim.h"
#endif

struct _MC2Player;



class aStyle2TextListItem : public aTextListItem
{
public:
	aStyle2TextListItem() { hasAnimation = false; normalColor = 0xff808080; }
	virtual long	init( FitIniFile* file, mc2::czstring blockName );
	virtual void		render();

protected:
	bool hasAnimation;
	aAnimGroup animGroup;
	int normalColor;
	

};

class aPlayerParams : public aObject
{
public:
	aPlayerParams();
	~aPlayerParams();
	aPlayerParams& operator=( const aPlayerParams& src );

	virtual long		init(long xPos, long yPos, long w, long h);
	void				init( FitIniFile* file, mc2::czstring blockName );

	virtual void		destroy();
	virtual void		render();
	virtual void		update();
	virtual int			handleMessage( unsigned long message, unsigned long who );
	virtual void		move( float offsetX, float offsetY );

	void				setData(const _MC2Player* data);

	bool				hasFocus( );

	bool				isSelected() { return bHasFocus; }

	int					getCommanderID( ) const { return commanderID; }

	void				disableReadyButton();



protected:
	aAnimButton		CBillsSpinnerUpButton;
	aAnimButton		CBillsSpinnerDownButton;
	aAnimButton		ReadyButton;

	std::unique_ptr<aObject[]>	statics;
	std::unique_ptr<aRect[]>	rects;
	long				rectCount;
	long				staticCount;

	std::unique_ptr<aText[]>	textObjects;
	long				textCount;

	aDropList				teamNumberDropList;
	aDropList				factionDropList;

	aStyle2TextListItem		templateItem;

	std::string					insigniaName;
	aEdit					edit;

	bool					bHasFocus;
	long					commanderID;

	friend class MPParameterScreen;
};


class CFocusManager
{
public:
	enum class control_species_data_type
	{
		CS_NONE,
		CS_DROPLIST
	};
	using enum control_species_data_type;

	CFocusManager();
	void clear();
	void *registerDropList(aDropList &DropList);
	void unregisterDropList(aDropList &DropList);
	void update();
	bool somebodyHasTheFocus(); /*this is distinct from the keyboard input focus*/
	aObject *pControlThatHasTheFocus();

private:
	control_species_data_type speciesOfTheControlWhichHasTheFocus;
	aDropList *pDropListThatHasTheFocus;
	std::vector<aDropList*> listOfDropListPointers;
};


class MPParameterScreen : public LogisticsScreen
{
public:
	
	MPParameterScreen();
	virtual ~MPParameterScreen();
	
	void				init(FitIniFile* file);
	virtual void		begin();
	virtual void		end();
 	virtual void		render( int xOffset, int yOffset );
	virtual void		render();
	virtual void		update();
	virtual int			handleMessage( unsigned long, unsigned long );
	
	static void			resetCheckBoxes();

	void				setHostLeftDlg( mc2::czstring playerName);

	static GUID			getGUIDFromFile( mc2::czstring fileName );

	static void			initializeMap( mc2::czstring fileName );

	static MPParameterScreen* s_instance;


private:
	int indexOfButtonWithID(int id);
	
	int					chatToSend;

	void				setMission( mc2::czstring fileName, bool resetData = 1 );
	void				setMissionClientOnly( mc2::czstring pNewMapName );
	void				checkVersionClientOnly( mc2::czstring pNewMapName );




	aPlayerParams	playerParameters[MAX_MC_PLAYERS];
	long			playerCount;

	MPLoadMap		mpLoadMap;
	LogisticsMapInfoDialog mapInfoDlg;
	bool			bLoading;
	bool			bShowNoMapDlg;
	bool			bErrorDlg;
	bool			bBootDlg;
	bool			bDisconnectDlg;
	std::string			mapName;
	long			bootPlayerID;

	bool			bMapInfoDlg;

	float			delayTime;

	bool			bHostLeftDlg;
	bool			bWaitingToStart;

	

};



//*************************************************************************************************
#endif  // end of file ( MPParameterScreen.h )
