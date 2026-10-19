//---------------------------------------------------------------------------
//
// ApprType.h -- File contains the Basic Game Appearance Type Declaration
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef APPRTYPE_H
#define APPRTYPE_H
//---------------------------------------------------------------------------
// Include Files
#ifndef DAPRTYPE_H
#include"daprtype.h"
#endif

#ifndef DAPPEAR_H
#include"dappear.h"
#endif

#ifndef DSTD_h
#include"dstd.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#include<stuff/stuffheaders.hpp>
#include<memory>
#include<string>
#include<vector>

//---------------------------------------------------------------------------
// Macro definitions
#ifndef NO_ERR
#define NO_ERR					0
#endif

#define MAX_LODS				3
//---------------------------------------------------------------------------
// Class definitions
class AppearanceType
{
	//Data Members
	//-------------
	public:
	
		unsigned long 		numUsers;			//Number of users using this appearanceType.
		unsigned long		appearanceNum;		//What kind am I.

		char 				*name;				//Appearance Base FileName.  A view of nameStore.
		std::string			nameStore;			//Owns the name text; a type is never copied or moved, so the view stays valid.
	
		glm::vec3		typeUpperLeft;		//For Designer defined extents of objects
		glm::vec3		typeLowerRight;		//For Designer defined extents of objects

		float				boundsUpperLeftX;
		float				boundsUpperLeftY;
		
		float				boundsLowerRightX;
		float				boundsLowerRightY;
		
		bool				designerTypeBounds;	//So I know not to change them if the designer typed them in.

		// META-FIX design B PART 1/2: set by tglVerifyShapeLoaded when a
		// REQUIRED base-shape load for this type failed (placeholder may be
		// standing in). Read by the
		// AppearanceTypeList::getAppearance backstop (one appr_type_fail
		// line per type) and by Mech3DAppearance::resetPaintScheme (keeps
		// the placeholder's texture handle from being clobbered by the
		// name-based instance rebind). Stock types never set this.
		bool				baseShapeLoadFailed;

	//Member Functions
	//-----------------
	public:

		void init (void)
		{
			numUsers = 0;
			appearanceNum = 0xffffffff;

			name = nullptr;

			typeUpperLeft = glm::vec3(0.0f);
			typeLowerRight = glm::vec3(0.0f);
			designerTypeBounds = false;
			baseShapeLoadFailed = false;
 		}

		AppearanceType (void)
		{
			init();
		}

		virtual void init (const char *fileName) = 0;
		
		virtual void destroy (void);

		void reinit (void);

		bool typeBoundExists (void)
		{
			return true;		//Always exists now
		}

		bool getDesignerTypeBounds (void)
		{
			return designerTypeBounds;
		}

		unsigned long getAppearanceClass (void)
		{
			return(appearanceNum >> 24);
		}
		
		virtual ~AppearanceType (void)
		{
			destroy();
		}
};

//---------------------------------------------------------------------------
class AppearanceTypeList
{
	//Data Members
	//-------------
	protected:

		// The registry owns every type, in load order. Users hold non-owning pointers that stay valid until removeAppearance drops
		// the last user or the registry is destroyed. destroy() releases the types before anything else they depend on.
		std::vector<std::unique_ptr<AppearanceType>>	types;

	//Member Functions
	//----------------
	public:

		AppearanceTypePtr getAppearance (unsigned long apprNum, const char * apprFile);
		
		long removeAppearance (AppearanceTypePtr which);
		
		void destroy (void);
		
		~AppearanceTypeList (void)
		{
			destroy();
		}

		// The one place the shared Sensor shapes are deleted: the registry's destroy() and the logistics SimpleCamera, which loads them
		// without a mission.
		static void destroySensorShapes (void);
};

extern AppearanceTypeListPtr appearanceTypeList;
//---------------------------------------------------------------------------
#endif
