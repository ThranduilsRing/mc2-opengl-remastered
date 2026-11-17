#ifndef OPTIONSAREA_H
#define OPTIONSAREA_H
/*************************************************************************************************\
OptionsArea.h			: Interface for the OptionsArea component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************
#ifndef LOGISTICSSCREEN_H
#include"logisticsscreen.h"
#endif

#ifndef SIMPLECAMERA_H
#include"simplecamera.h"
#endif

#include "mc2_czstring.h"
#include <memory>
#include"alistbox.h"
#include"attributemeter.h"

class aButton;

class CPrefs;


class OptionsXScreen : public LogisticsScreen
{
public:
	
	OptionsXScreen();
	~OptionsXScreen() override;
	
	void init(FitIniFile* file);
	bool isDone();
	void render() override;
	void update() override;
	int			handleMessage( unsigned long, unsigned long ) override;

	void updateOptions(); // put into inventory

	bool				bDone;


private:

	int indexOfButtonWithID(int id);

	std::unique_ptr<LogisticsScreen>		tabAreas[4];
	int						curTab;
	bool			bShowWarning;


};

class ScrollX : public aObject
{
public:

	ScrollX();
	long	init(aButton* pLeft, aButton* pRight, aButton* pTab );
	void	update() override;

	int		handleMessage( unsigned long message, unsigned long fromWho ) override;

	void			SetScrollMax(float newMax);
	void			SetScrollPos(float newPos);
	float			GetScrollMax(void){return scrollMax;};
	float			GetScrollPos(void){return scrollPos;};
    // sebi unused
	//long			SetSrollInc( long newInc ){ scrollInc = (float)newInc; } // amount you move for one arrow click
	//long			SetScrollPage(long newInc){ pageInc = (float)newInc;} // amount you move if you click on the bar itself
	void			ScrollUp(void);
	void			ScrollPageUp(void);
	void			ScrollDown(void);
	void			ScrollPageDown(void);
	void			SetScroll( long newScrollPos );	
	void			Enable( bool enable );

private:

	void ResizeAreas();

	 
	aButton*		buttons[3];
	float			scrollMax;
	float			scrollPos;
	float			scrollInc;
	float			pageInc;

	long			lastX;

};

class OptionsGraphics : public LogisticsScreen
{

public:

    struct ResModes {
    	int xRes;
    	int yRes;
    	int bitDepth;
    };

    OptionsGraphics();

	void init(long xOffset, long yOffset);
	void render() override;
	void update() override;
	void begin() override;
	void end() override;
	void reset( const CPrefs& newPrefs);

	int		handleMessage( unsigned long message, unsigned long fromWho ) override;

private:
	int waterReflectionSelection = -1;
	int waterDetailSelection = -1;
	aDropList		resolutionList;
	aDropList		cardList;
	bool			bExpanded;

    std::unique_ptr<ResModes[]> resolutionModes;
    std::unique_ptr<std::unique_ptr<char[]>[]> resolutionModesStr;
    int             numResolutionModes;

};

class OptionsAudio : public LogisticsScreen
{
	public:
		void init(long xOffset, long yOffset);
		void render() override;
		void update() override;
		void begin() override;
		void end() override;
		void reset(const CPrefs& newPrefs);

		int		handleMessage( unsigned long message, unsigned long fromWho ) override;

	private:

		ScrollX		scrollBars[5];

};

class OptionsHotKeys : public LogisticsScreen
{

public:
		void init(long xOffset, long yOffset);
		void render() override;
		void update() override;
		void begin() override;
		void end() override;
		void reset(bool bUseOld);

		int		handleMessage( unsigned long message, unsigned long fromWho ) override;

private:

	static void makeKeyString( long hotKey, char* buffer );
	static int makeInputKeyString( long& hotKey, char* buffer );

	void refreshHotKeyList();

	aListBox		hotKeyList;
	bool			bShowDlg;
	long			curHotKey;
};

class OptionsGamePlay : public LogisticsScreen
{
public:
	void init(long xOffset, long yOffset);
	void render() override;
	void update() override;
	void begin() override;
	void end() override;
	void reset(const CPrefs& newPrefs);

	int		handleMessage( unsigned long message, unsigned long fromWho ) override;

	void resetCamera();

	private:

	SimpleCamera	camera;
};

class HotKeyListItem : public aListItem
{
public:

	static void init();
	void render() override;
	void update() override;
	
	void setDescription( mc2::czstring pText );
	void setKey( mc2::czstring pText );

	HotKeyListItem( );
	~HotKeyListItem() override;

	void	setHotKey( long lNew ){ hotKey = lNew; }
	void	setCommand( long lCommand ) { command = lCommand; }

	long	getCommand() const { return command; }
	long	getHotKey() const { return hotKey; }

private:

	aText		description;
	aText		text;
	aAnimGroup	animations[3];
	aRect		rects[2];
	long		hotKey;
	long		command;

	static		HotKeyListItem* s_item;
	
};





//*************************************************************************************************
#endif  // end of file ( OptionsArea.h )
