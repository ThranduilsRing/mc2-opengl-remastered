#ifndef GAMETACMAP_H
#define GAMETACMAP_H
/*************************************************************************************************\
gameTacMap.h			: Interface for the gameTacMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef TACMAP_H
#include"tacmap.h"
#include <memory>
#endif

class Camera;

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
gameTacMap:
**************************************************************************************************/
class GameTacMap: public TacMap
{
	public:

	GameTacMap();
	~GameTacMap()
	{
		if (buildingPoints)
		{
			buildingPoints.reset();
			buildingPoints = nullptr;
		}
	}

	void init( unsigned char* bitmapData, int dataSize );
	void update(); // do not call these two functions from editor
	void render();
	void extractHudBlips();

	// MC2_HUD_IMGUI_TACMAP (IN-MISSION-HUD-IMGUI-SCOPE-1 slice E):
	// ImGui panel replaces the legacy gos-batch tac-map draw AND its
	// click/hover handling. render()/update() try these first when the
	// gate is on; legacy path is untouched when the gate is off or
	// renderImGui returns false (pin-to-legacy).
	bool usingImGui() const;
	bool renderImGui();

	bool animate (long objectiveId, long nFlashes);

	// if the world coords do not lie on the map, they will be changed.
	void worldToTacMap( glm::vec3& world, gos_VERTEX& tac );	


	bool inRegion( int x, int y ){ return x > left && x < right && y > top && y < bottom; }
	
	void initBuildings( unsigned char* data, int size );
	void setPos( const GUI_RECT& newPos );
	
	protected:

	int		top;	// position
	int		left;
	int		right;
	int		bottom;
	int		bmpWidth;
	int		bmpHeight;
	int		buildingCount;
	std::unique_ptr<gos_VERTEX[]> buildingPoints;

	unsigned long textureHandle;
	unsigned long viewRectHandle;
	unsigned long blipHandle;

	glm::vec3 navMarkers[6];
	unsigned long navMarkerCount;
	unsigned long curNavMarker;

	void drawSensor( const glm::vec3& pos, float radius, long color);
	void drawBlip( const glm::vec3& pos, long color, int shape  );

	// shared blip/sensor collection (sensor towers + movers, colors by
	// team/selection/contact) — factored verbatim from the legacy render()
	// loops so the ImGui path draws from the SAME data (arrays sized
	// MAX_MOVERS by the callers). Returns count.
	int extractBlips( unsigned long* colors, unsigned long* ringColors,
                      glm::vec3* positions, unsigned long* ranges, bool* selected );
	int collectBlips( unsigned long* colors, unsigned long* ringColors,
					  glm::vec3* positions, unsigned long* ranges,
					  bool* selected );

	const static float s_blinkLength;
	static float		s_lastBlinkTime;

	//Tutorial
	long objectiveAnimationId;
	long objectiveNumFlashes;
	float objectiveFlashTime;

};


//*************************************************************************************************
#endif  // end of file ( gameTacMap.h )
