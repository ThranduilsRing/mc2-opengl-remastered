#ifndef COMPONENTLISTBOX_H
#define COMPONENTLISTBOX_H
#include <memory>
/*************************************************************************************************\
ComponentListBox.h			: Interface for the ComponentListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef ALISTBOX_H
#include"alistbox.h"
#endif

#ifndef AANIM_H
#include"aanim.h"
#include <vector>
#include <array>
#include "logisticscomponent.h"
#endif

class LogisticsVariant;
class LogisticsComponent;
class LogisticsVehicle;


inline constexpr int COMP_ANIMATION_COUNT = 5;

class ComponentIcon : public aObject
{
public:
	using aObject::render;
	void setComponent(const LogisticsComponent& c) { tileX = c.hasLauncherIcon() ? c.getComponentWidth() : 1; tileY = c.hasLauncherIcon() ? c.getComponentHeight() : 1; }
	void render() override
	{
		if (tileX <= 1 && tileY <= 1) { aObject::render(); return; }
		const std::array saved{location[0], location[1], location[2], location[3]};
		const float cellW = width() / tileX, cellH = height() / tileY;
		setUVs(0, 0, 48, 32);
		for (int y = 0; y < tileY; ++y) for (int x = 0; x < tileX; ++x)
		{
			for (int i = 0; i < 4; ++i) { location[i].x = saved[0].x + (x + (i >= 2)) * cellW; location[i].y = saved[0].y + (y + (i == 1 || i == 2)) * cellH; }
			aObject::render();
		}
		for (int i = 0; i < 4; ++i) location[i] = saved[i];
	}
private:
	int tileX = 1, tileY = 1;
};


//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
ComponentListBox:
**************************************************************************************************/
// complex list box items, icons and all
class ComponentListItem : public aListItem
{
	public:

		ComponentListItem( LogisticsComponent* pComp );
		virtual ~ComponentListItem();

		static int init( FitIniFile& file );

		LogisticsComponent* getComponent() { return pComponent; }

		virtual void render();

		void	update();

	private:

		friend class ComponentIconListBox;

		static std::unique_ptr<ComponentListItem> s_templateItem;

		aText		name;
		aText		costText;
		aText		heatText;
		aObject		costIcon;
		aObject		heatIcon;
		ComponentIcon icon;
		aRect		iconOutline;
		aRect		outline;
		aText		disabledText;

		aAnimGroup	animations[COMP_ANIMATION_COUNT];

		aAnimGroup*	pChildAnims[6];

		LogisticsComponent* pComponent;

		static void assignAnimation( FitIniFile& file,
			int whichChild, char animNames[COMP_ANIMATION_COUNT][32], aObject* pObject );

		void doAdd();
		void setComponent();
		void startDrag();



};

class ComponentIconListBox : public aListBox
{

public:
	ComponentIconListBox();
	virtual ~ComponentIconListBox();
	void destroy() override;
	void setType( int Type, int orThisType, int orThis, int orThis2 = -1 );
	virtual void update();

	LogisticsComponent* getComponent();
	int selectFirstAvailableComponent();

	static ComponentIconListBox* s_instance;

	

	private:

		ComponentIconListBox( const ComponentIconListBox& src );
		ComponentIconListBox& operator=( const ComponentIconListBox& src );

		std::vector<std::unique_ptr<ComponentListItem>> masterComponentList;

		int type;

		void addSortedItem( ComponentListItem* pItem );

};


//*************************************************************************************************
#endif  // end of file ( ComponentListBox.h )
