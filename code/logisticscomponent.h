#ifndef LOGISTICSCOMPONENT_H
#define LOGISTICSCOMPONENT_H
/*************************************************************************************************\
LogisticsComponent.h			: Interface for the LogisticsComponent component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <memory>
#include "mc2_czstring.h"

// forward declarations
class File;


class LogisticsComponent
{
	public:

		static long XICON_FACTOR; // multiplier for width and height of icons
		static long YICON_FACTOR;

		static float MAX_HEAT;
		static float MAX_DAMAGE;
		static float MAX_RECYCLE;
		static float MAX_RANGE;

		LogisticsComponent( );
		~LogisticsComponent();
		int init( char* dataLine );

		inline int getID() const { return ID; }
		inline int getType() const { return Type; }
		inline int getRange() const{ return range; }
		inline float getDamage() const { return damage; }
		inline float getRecycleTime() const { return recycleTime; }
		inline int getAmmo() const { return Ammo; }
		void		setAmmo(int newAmmo) { Ammo = newAmmo; }
		inline float getWeight() const { return weight; }
		inline int getCost() const { return cost; }
		inline float getHeat() const { return heat; }
		inline const char* getName() const { return name.get(); }
		inline const char* getFlavorText() const { return flavorText; }
		inline int	getHelpID() const { return helpStringID; }
		inline const char* getIconFileName() const { return iconFileName.get(); }
		inline const char* getPictureFileName() const { return pictureFileName.get(); }
		inline int		getComponentWidth() const { return iconX; }
		inline int		getComponentHeight() const { return iconY; }
		// MC2_ENGINE_XL: XL engines get a weight-class-dependent footprint + heat,
		// applied per current mech on mech-bay entry (mechlabscreen). Mutating the
		// shared component is safe — only one mech is edited at a time.
		void			setEngineProfile( int w, int h, float heatVal )
							{ iconX = w; iconY = h; heat = heatVal; }
		// EQUIP-ADD-FILL-1: init() sources `name` from the string table via the
		// CSV stringID column; a NEW modded id has no string, so the gated ADD
		// append overwrites it with the def's DisplayName. `name` and
		// `flavorText` alias ONE allocation in init() -- keep that invariant.
		// Null names are copied as empty text.
		void			setDisplayName( mc2::czstring n );
		bool			compare( LogisticsComponent* second, int type );
		bool			isAvailable(){ return bAvailable; }
		void			setAvailable(bool avail){ bAvailable = avail; }
		bool			isWeapon();

		 enum SORT_ORDERS
		 {
			 DAMAGE = 0,
			 HEAT = 1,
			 NAME = 2,
			 RANGE = 3,
			 WEIGHT = 4
		 };

		 enum WEAPON_RANGE
		 {
			 SHORT = 0,
			 MEDIUM = 1,
			 LONG = 2,
			 NO_RANGE = 3 // not a weapon
		 };

 		WEAPON_RANGE getRangeType() const { return rangeType; }


	private:

		int ID; // index into csv
		int Type; // defined in csv
		int	stringID;
		int helpStringID;

		int iconX;
		int iconY;

		std::unique_ptr<char[]> iconFileName;
		std::unique_ptr<char[]> pictureFileName;
		int range;
		WEAPON_RANGE rangeType;

		float damage;
		float recycleTime;
		int Ammo;
		float weight;
		int cost;
		float heat;
		std::unique_ptr<char[]> name;
		char* flavorText;

		bool	bHead;
		bool	bTorso;
		bool	bLegs;
		bool	bAvailable;

		static	char* s_typeString[];

		// HELPERS
		int extractString( char*& pFileLine, char* pBuffer, int bufferLength );
		int extractInt( char*& pFileLine );
		float extractFloat( char*& pFileLine );







};


//*************************************************************************************************
#endif  // end of file ( LogisticsComponent.h )
