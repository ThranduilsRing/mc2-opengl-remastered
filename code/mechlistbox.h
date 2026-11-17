#ifndef MECHLISTBOX_H
#define MECHLISTBOX_H
#include <memory>
/*************************************************************************************************\
MechListBox.h			: Interface for the MechListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef ALISTBOX_H
#include"alistbox.h"
#endif

#ifndef AANIM_H
#include"aanim.h"
#endif

class FitIniFile;
class LogisticsMech;


//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MechListBox:
**************************************************************************************************/

inline constexpr int ANIMATION_COUNT = 4;
class MechListBoxItem : public aListItem
{
public:
	
	~MechListBoxItem() override;
	MechListBoxItem( LogisticsMech* pMech, long count );

	static void init( FitIniFile& file );

	void update() override;
	void render() override;

	LogisticsMech* getMech( ){ return pMech; }
	void	resetMech( ) {pMech = nullptr; }
	bool		pointInside(long xPos, long yPos) const override;

	bool		bOrange;
	static bool	bAddCalledThisFrame;



	
private:

	static std::unique_ptr<MechListBoxItem> s_templateItem;
	
	aAnimGroup		animations[2][ANIMATION_COUNT];
	
	aObject		mechIcon;
	bool hudArt = false;
	aText		chassisName;
	aText		variantName;
	aText		countText;
	aText		weightText;
	aObject		weightIcon;
	aText		costText;
	aObject		costIcon;
	aRect		outline;
	aRect		line;

	long		animationIDs[9];

	bool		drawCBills;
	bool		bIncludeForceGroup;

	LogisticsMech* pMech;

	long mechCount;

	MechListBoxItem& operator=( const MechListBoxItem& src );

	friend class MechListBox;

	void setMech();
	void doAdd();
	void startDrag();
	static void assignAnimation( FitIniFile& file, long& curAnimation );


	bool bDim;

	float animTime;


};


class MechListBox: public aListBox
{
public:

	MechListBox(bool deleteIfNoInventory, bool bIncludeForceGroup);

	
	void	setScrollBarOrange();
	void	setScrollBarGreen();

	~MechListBox() override;
	
	static int		init();
	void	drawCBills( bool bDraw );

	void update() override;

	LogisticsMech* getCurrentMech();

	static bool initIcon( LogisticsMech* pMec, aObject& icon );

	long AddItem(std::unique_ptr<aListItem> itemString) override;
	long AddItem(aListItem* itemString) override { return AddItem(std::unique_ptr<aListItem>(itemString)); }

	void	dimItem( LogisticsMech* pMech, bool bDim );
	void	undimAll();
	void	disableItemsThatCostMoreThanRP();
	void	disableItemsThatCanNotGoInFG();

	void	setOrange( bool bOrange );


	

private:

	static bool	s_DrawCBills;
	
	MechListBox( const MechListBox& src );
	MechListBox& operator=( const MechListBox& src );

	friend class MechListBoxItem;

	bool		bDeleteIfNoInventory;
	bool		bIncludeForceGroup;

	bool		bOrange;


	

};


//*************************************************************************************************
#endif  // end of file ( MechListBox.h )
