//---------------------------------------------------------------------
//
// crater.h -- Crater/footprint manager for MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef CRATER_H
#define CRATER_H
//---------------------------------------------------------------------
// Include Files
#ifndef HEAP_H
#include"heap.h"
#endif

#ifndef PACKET_H
#include"packet.h"
#endif

#include<vector>
#include<stuff/stuff.hpp>

//---------------------------------------------------------------------
// Macro Definitions
enum craterTypes
{
	INVALID_CRATER = -1,
	SML_FOOTPRINT = 0,
	AVG_FOOTPRINT,
	BIG_FOOTPRINT,
	ULLER_FOOTPRINT,
	MADCAT_FOOTPRINT,
	LEFT_MASAKARI_FOOTPRINT,
	RIGHT_MASAKARI_FOOTPRINT,
	BUSH_FOOTPRINT,
	NOVACAT_FOOTPRINT,
	TURKINA_FOOTPRINT = 63,
	CRATER_1 = 64,
	CRATER_2,
	CRATER_3,
	CRATER_4,
	MAX_CRATER_SHAPES
};

inline constexpr int FOOTPRINT_ROTATIONS = 16;
inline constexpr int BIG_CRATER_OFFSET = 0;
inline constexpr int SMALL_CRATER_OFFSET = 1;
//---------------------------------------------------------------------
// struct CraterData
struct _CraterData
{
	int 			craterShapeId;
	glm::vec3 position[4];
	Stuff::Vector4D screenPos[4];
}; using CraterData = _CraterData;

using CraterDataPtr = CraterData*;
//---------------------------------------------------------------------
// class CraterManager
class CraterManager
{
	//Data Members
	//-------------
	protected:
		
		HeapManagerPtr		craterPosHeap;
		UserHeapPtr			craterShpHeap;
		
		unsigned long		craterPosHeapSize;
		unsigned long		craterShpHeapSize;
		
		PacketFilePtr		craterFile;
		
		unsigned long		maxCraters;
		unsigned long		currentCrater;
		CraterDataPtr		craterList;
		long				numCraterTextures;

		// Texture node per crater texture, and the GOS handle cached for it this frame; sized by init(), empty until then.
		std::vector<DWORD>	craterTextureIndices;
		std::vector<DWORD>	craterTextureHandles;
				
	//Member Functions
	//-----------------
	protected:
		
	public:
	
		void init (void)
		{
			craterPosHeap = nullptr;
			craterShpHeap = nullptr;

			craterPosHeapSize = craterShpHeapSize = 0;
			
			currentCrater = maxCraters = 0;
			craterList = nullptr;

			numCraterTextures = 0;
			
			craterTextureHandles.clear();
			craterTextureIndices.clear();
			
			craterFile = nullptr;
		}
		
		CraterManager (void)
		{
			init();
		}
		
		long init (long numCraters, unsigned long craterTypeSize, const char *craterFileName);
		
		~CraterManager (void)
		{
			destroy();
		}
		
		void destroy (void);
		
		long addCrater (long craterType, glm::vec3 &position, float rotation);
		
		long update (void);
		void render (void);
};

//---------------------------------------------------------------------
using CraterManagerPtr = CraterManager*;
extern CraterManagerPtr craterManager;

//---------------------------------------------------------------------
#endif
