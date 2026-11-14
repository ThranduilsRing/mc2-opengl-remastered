#ifndef PILOTREVIEWAREA_H
#define PILOTREVIEWAREA_H
/*************************************************************************************************\
PilotReviewARea.h			: Interface for the PilotReviewARea component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************
#include <memory>
#include"logisticsscreen.h"
#include"alistbox.h"
#include"attributemeter.h"
#include"aanim.h"
#include"logisticspilot.h"

class aButton;
class PilotIcon;
class PilotPromotionArea;

/**************************************************************************************************
CLASS DESCRIPTION
PilotReviewARea:
**************************************************************************************************/


// holds the dead and active pilots
class PilotListBox : public aListBox
{
public:

	PilotListBox();
	long AddItem( std::unique_ptr<aListItem> item ) override;
	virtual void update();

	bool isDone( ){ return bDone; }

private:
 
	float		timeSinceStart;
	int			curItem;
	long		oldScroll;
	long		newScroll;
	float		scrollTime;
	bool		bDone;
};

// this screen shows up after the salvage screen
class PilotReviewScreen : public LogisticsScreen
{
public:
	
	PilotReviewScreen();
	virtual ~PilotReviewScreen();
	
	void init(FitIniFile* file);
	bool isDone();
	bool listFinished();	// the per-pilot scroll (promotion panels included) has run out (capture harness)
	virtual void render();
	virtual void update();
	virtual int			handleMessage( unsigned long, unsigned long );

	void				updatePilots(); // put into inventory, save??

	bool				bDone;

	static PilotPromotionArea*	s_curPromotion;
	static PilotReviewScreen*	instance;

private:

	PilotListBox			pilotListBox;
	aAnimation				entryAnim;
	aAnimation				exitAnim;
};


// base class for dead, and active pilot list box items
class PilotListItem : public aListItem
{
public:

	PilotListItem()
	{
		currentTime = -1.f;
		bDone = 0;
	}

	virtual void begin();	// for animation purposes...
	virtual bool isDone() = 0; // implement this yourself
	virtual void update();

protected:
	float	currentTime;
	bool	bDone;

};


// list box item for dead pilots
class DeadPilotListItem : public PilotListItem
{
public:
	virtual void		render();
	virtual void		update();
	virtual bool		isDone();

	virtual ~DeadPilotListItem();

	static void			init( FitIniFile* file );

	LogisticsPilot*		getPilot(){ return pPilot; }

	DeadPilotListItem( LogisticsPilot* pUnit );

private:
	friend class PilotReviewScreen;

	LogisticsPilot*		pPilot;
	
	struct Templates;
	static std::unique_ptr<Templates>	s_tpl;
	static long			s_itemCount;
	
	aText				nameText;
	aText				rankText;
	aText				missionText;
	aText				killsText;

	std::unique_ptr<PilotIcon>			liveIcon;
	std::unique_ptr<PilotIcon>			deadIcon;


	
};

// pilots that weren't killed
class ActivePilotListItem : public PilotListItem
{
public:

	virtual void		render();
	virtual void		update();
	virtual bool		isDone();

	virtual ~ActivePilotListItem();

	static void			init( FitIniFile* file );
	ActivePilotListItem( LogisticsPilot* pUnit );
	static long			s_totalWidth;

	float				flashTime();


protected:

	AttributeMeter		attributeMeters[2];
	struct Templates;
	static std::unique_ptr<Templates>	s_tpl;
	static long			s_itemCount;

	aObject		icons[4];
	aText		nameText;
	aText		missionText;
	aText		killsText;
	long			itemCount;

	aText		rankText;
	aText		gunneryText;
	aText		pilotingText;
	aRect		area;

	std::unique_ptr<aObject>	medalIcons[MAX_MEDAL];
	std::unique_ptr<aText>	medalTexts[MAX_MEDAL];
	aText		promotionText;
	aText		medalAwardedText;

	
	std::unique_ptr<PilotIcon>	pilotIcon;
	LogisticsPilot* pilot;

	bool		showingPromotion;
	bool		promotionShown;

	long		medalCount;

};

class PilotPromotionArea : public LogisticsScreen
{
public:

	bool isDone();
	void init( FitIniFile& file );
	virtual void render();
	virtual void update();
	void setPilot(LogisticsPilot* pPilot, PilotIcon* pIcon);
	virtual int			handleMessage( unsigned long, unsigned long );
	virtual ~PilotPromotionArea();

	aListBox*	getSkillListBox() { return &skillListBox; }

private:

	AttributeMeter	attributeMeters[2];

	LogisticsScreen	areaLeft;
	LogisticsScreen	areaRight;
	LogisticsPilot* pilot;
	aAnimation		leftInfo;
	aAnimation		rightInfo;
	aAnimation		leftExitInfo;
	aAnimation		rightExitInfo;
	bool			bDone;

	aListBox		skillListBox;
	PilotIcon*		pilotIcon;
	aAnimation		selSkillAnim;
	long			lastCheck;

};



// specialty skills each show up in here
class SpecialtyListItem : public aListItem
{
public:
	virtual void		render();
	virtual void		update();

	virtual ~SpecialtyListItem();

	static void			init( FitIniFile* file );
	virtual int			handleMessage( unsigned long message, unsigned long who );

	virtual bool		isChecked();
	virtual void		setCheck( bool );
	int					getID();
	SpecialtyListItem( int ID );

private:
	struct Templates;
	static std::unique_ptr<Templates>	s_tpl;

	static void			deleteStatics();

	aAnimation			pressedAnim;
	aAnimation			highlightAnim;
	aAnimation			normalAnim;
	aObject				icon;
	aText				description;
	aButton			radioButton;
	aRect				outline;

	int					ID;

	friend class PilotPromotionArea;

	
};




//*************************************************************************************************
#endif  // end of file ( PilotReview.h )
