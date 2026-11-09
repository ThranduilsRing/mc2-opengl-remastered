#include "mc2_czstring.h"
#include "render_view.h"
#include "../RenderCore/TextureService.h"
#include "../GameOS/gameos/ui_texture.h"
#include "../mclib/texture_assets.h"
 #define MECHICON_CPP
/*************************************************************************************************\
MechIcon.cpp			: Implementation of the MechIcon component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"mechicon.h"
#include "icon_registry.h"
#include "mechicon_package_hud.h"
#include <mdspan>
#include "../GameOS/gameos/texture_decode.h"
#include "mc2fmt.h"
#include"mover.h"
#include"utilities.h"
#include"mech.h"
#include"gvehicl.h"
#include"gamecam.h"
#include"controlgui.h"
#include"gamesound.h"
#include"platform_windows.h"
#include "Gates/gate.h"
#include "package_chassis.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include "mc2fmt.h"

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

// Damage images have packed rectangles; their slots are independent of source icon keys.
static TEXTUREPTR lockIconPage(unsigned long handle)
{
    TEXTUREPTR page{};
    gos_LockTexture(handle, 0, 0, &page);
    return page;
}

static const char* hudSheet(bool vehicle)
{
    if (Environment.screenWidth == 640)
        return vehicle ? "data/art/mcui_low5.tga" : "data/art/mcui_low4.tga";
    if (Environment.screenWidth == 800)
        return vehicle ? "data/art/mcui_med5.tga" : "data/art/mcui_med4.tga";
    return vehicle ? "data/art/mcui_high8.tga" : "data/art/mcui_high7.tga";
}

std::unique_ptr<StaticInfo>		ForceGroupIcon::jumpJetIcon = nullptr;

std::unique_ptr<MC2Movie>	ForceGroupIcon::bMovie = nullptr;
DWORD ForceGroupIcon::pilotVideoTexture = 0;
MechWarrior*	ForceGroupIcon::pilotVideoPilot = nullptr;


float			ForceGroupIcon::unitIconX = 32;
float			ForceGroupIcon::unitIconY = 38;
float			PilotIcon::pilotIconX = 25;
float			PilotIcon::pilotIconY = 36;
const long		PilotIcon::DEAD_PILOT_INDEX = 27;

unsigned long	PilotIcon::s_pilotTextureHandle = 0;
unsigned long	PilotIcon::s_pilotTextureWidth = 0;
AssetScale::AssetKey PilotIcon::s_pilotTexturesKey;
long			ForceGroupIcon::pilotTextTop[17] = {0};


DWORD ForceGroupIcon::damageColors[4][3] = 
		{ 194u << 16 | 229u << 8 | 255u | 0xff << 24,	92u << 16 | 150u << 8 | 194u | 0xff << 24, 0u << 16 | 83u << 8 | 146u | 0xff << 24,
		  248u << 16 | 241u << 8 | 193u | 0xff << 24,	248u << 16 | 206u << 8 | 31u | 0xff << 24, 139u << 16 | 114u << 8 | 0u | 0xff << 24,
		  248u << 16 | 193u << 8 | 193u | 0xff << 24,	248u << 16 | 31u << 8 | 31u | 0xff << 24, 139u << 16 | 0u << 8 | 0u | 0xff << 24,
		  94u << 16 | 101u << 8 | 101u | 0xff << 24,	56u << 16 | 64u << 8 | 64u | 0xff << 24, 26u << 16 | 33u << 8 | 33u | 0xff << 24
		};

DWORD ForceGroupIcon::ForceGroupColors[11] = {
		0xff005392,
		0xffC66600,
		0xff6E7C00,
		0xffB70000,
		0xff620053,
		0xffD8AD00,
		0xff8B00BE,
		0xffFF0000,
		0xffFF8A00,
		0xff0091FF,
		0xff505050

 };

ForceGroupIcon::AnimationInfo ForceGroupIcon::animationInfos[NUM_DEATH_INFOS] = 
{
	0.f, 0,
	2.5f, 0, 
	5.0f, 0x7fff0000, 
	5.35f, 0x7fff0000,
	6.0f, 0xff000000,
	6.5f, 0xff000000

};

bool ForceGroupIcon::s_slotUsed[240] = { 0 };

gos_VERTEX ForceGroupIcon::	bmpLocation[17][5]  = {0}; // in screen coords

GUI_RECT ForceGroupIcon:: textArea[17]  = {0};
GUI_RECT ForceGroupIcon:: pilotRect[17]  = {0};
GUI_RECT ForceGroupIcon:: selectionRect[17]  = {0};
GUI_RECT ForceGroupIcon:: healthBar[17]  = {0};

float ForceGroupIcon::	pilotLocation[17][4]  = {0};

std::unique_ptr<aFont> ForceGroupIcon::gosFontHandle = nullptr;

int ForceGroupIcon::curScreenWidth = 800;


ForceGroupIcon::ForceGroupIcon(  )
{
	unit = nullptr;
	bDrawBack = 0;
	backDamageIndex = -1;
	damageIconIndex = 0;

	locationIndex = 0;

	bool bFound = 0;

	for ( int index = 0; index < 240; index++ )
	{
		if ( !s_slotUsed[ index ] )
		{
			damageIconIndex = index;
			s_slotUsed[index] = 1;
			bFound = 1;
			break;
		}
	}

	
	Assert( bFound, bFound, "out of space for icon textures" );

	deathAnimationTime = 0.0;
	msgPlayTime = 0.0f;
	
}

void ForceGroupIcon::resetResolution(bool bForce)
{
	if ( curScreenWidth !=  Environment.screenWidth || bForce )
	{
		setIconVariables();

		PilotIcon::swapResolutions();
	}

	curScreenWidth = Environment.screenWidth;
}

void ForceGroupIcon::swapResolutions( bool bForce )
{
	resetResolution(bForce);
	init( unit );
}

ForceGroupIcon::~ForceGroupIcon()
{

	if (pilotVideoPilot && unit && pilotVideoPilot == unit->getPilot() )
	{
		if ( bMovie )
		{
			bMovie.reset();
			bMovie = nullptr;
			pilotVideoTexture = 0;
		}
		pilotVideoPilot = nullptr;
	}
	s_slotUsed[damageIconIndex] = 0;

	if ( backDamageIndex != -1 )
		s_slotUsed[ backDamageIndex ] = 0;
}

void ForceGroupIcon::init( FitIniFile& file, int which )
{
	char buffer[256];
	(*std::format_to_n( buffer, sizeof(buffer) - 1, "MechIcon{:d}", mc2::fmt::as_i32(which) ).out = '\0');
	if ( NO_ERR != file.seekBlock( buffer ) )
		Assert( 0, 0, "couldn't find the icon" );

	if ( !gosFontHandle )
		gosFontHandle = std::make_unique<aFont>();

	file.readIdLong( "TextAReaLeft", 	textArea[which].left );
	file.readIdLong( "TextAreaRight", 	textArea[which].right );
	file.readIdLong( "TextAReaTop", 	textArea[which].top );
	file.readIdLong( "TextAreaBottom", 	textArea[which].bottom );
	
	textArea[which].left	+= ControlGui::hiResOffsetX;
	textArea[which].right	+= ControlGui::hiResOffsetX; 
	textArea[which].top     += ControlGui::hiResOffsetY; 
	textArea[which].bottom  += ControlGui::hiResOffsetY; 

	file.readIdLong( "PilotTextTop", 	pilotTextTop[which] );
	
	pilotTextTop[which] += ControlGui::hiResOffsetY;  

	long left, right, top, bottom;
	file.readIdLong( "MechIconAreaLeft", 	left );
	file.readIdLong( "MechIconArearight", 	right );
	file.readIdLong( "MechIconAreaTop", 	top );
	file.readIdLong( "MechIconAreaBottom", 	bottom );
	
	left	+= ControlGui::hiResOffsetX;
	right   += ControlGui::hiResOffsetX;
	top     += ControlGui::hiResOffsetY;
	bottom  += ControlGui::hiResOffsetY;

	for ( int i = 0; i < 4; i++ )
	{
		bmpLocation[which][i].x = left;
		bmpLocation[which][i].y = top;
		bmpLocation[which][i].argb = 0xffffffff;
		bmpLocation[which][i].rhw = 0.5;
	}

	bmpLocation[which][2].x = bmpLocation[which][3].x = right;
	bmpLocation[which][1].y = bmpLocation[which][2].y = bottom;

	bmpLocation[which][4] = bmpLocation[which][0];

	file.readIdLong( "PilotAreaLeft", 	left );
	file.readIdLong( "PilotAreaRight",	right );
	file.readIdLong( "PilotAreaTop", 	top );
	file.readIdLong( "PilotAreaBottom", bottom );

	left	+= ControlGui::hiResOffsetX;
	right   += ControlGui::hiResOffsetX;
	top     += ControlGui::hiResOffsetY;
	bottom  += ControlGui::hiResOffsetY;


	pilotLocation[which][0] = left;
	pilotLocation[which][1] = top;
	pilotLocation[which][2] = right;
	pilotLocation[which][3] = bottom;
	
	file.readIdLong( "PilotRectLeft", 	pilotRect[which].left );
	file.readIdLong( "PilotRectRight", 	pilotRect[which].right );
	file.readIdLong( "PilotRectTop", 	pilotRect[which].top );
	file.readIdLong( "PilotRectBottom", pilotRect[which].bottom );
	
	pilotRect[which].left		+= ControlGui::hiResOffsetX;
	pilotRect[which].right      += ControlGui::hiResOffsetX;
	pilotRect[which].top        += ControlGui::hiResOffsetY;
	pilotRect[which].bottom     += ControlGui::hiResOffsetY;

	file.readIdLong( "SelectionRectLeft", 	selectionRect[which].left );
	file.readIdLong( "SelectionRectRight", 	selectionRect[which].right );
	file.readIdLong( "SelectionRectTop", 	selectionRect[which].top );
	file.readIdLong( "SelectionRectBottom", selectionRect[which].bottom );
	
	selectionRect[which].left   += ControlGui::hiResOffsetX;
	selectionRect[which].right  += ControlGui::hiResOffsetX;
	selectionRect[which].top    += ControlGui::hiResOffsetY;
	selectionRect[which].bottom += ControlGui::hiResOffsetY;

	file.readIdLong( "HealthBarLeft", 	healthBar[which].left );
	file.readIdLong( "HealthBarRight", 	healthBar[which].right );
	file.readIdLong( "HealthBarTop", 	healthBar[which].top );
	file.readIdLong( "HealthBarBottom", healthBar[which].bottom );
	
	healthBar[which].left       += ControlGui::hiResOffsetX;
	healthBar[which].right      += ControlGui::hiResOffsetX;
	healthBar[which].top        += ControlGui::hiResOffsetY;
	healthBar[which].bottom     += ControlGui::hiResOffsetY;

	if ( !jumpJetIcon )
		jumpJetIcon = std::make_unique_for_overwrite<StaticInfo>();

	jumpJetIcon->init( file, "JumpJetIcon", 0, 0 );

	
}


void MechIcon::setDrawBack(bool bSet)
{
    bDrawBack = bSet;
    if (!bSet) return;
    if (backDamageIndex == -1) {
        for (int index = 0; index < 240; ++index)
            if (!s_slotUsed[index]) { backDamageIndex = index; s_slotUsed[index] = true; break; }
    }
    if (backDamageIndex == -1) return;
    const auto& rect = unit_icons::damageRect(backDamageIndex);
    const auto handle = unit_icons::damageTexture(backDamageIndex);
    TEXTUREPTR page = lockIconPage(handle);
    if (!page.pTexture) return;
    DWORD* dest = page.pTexture + rect.y * page.Width + rect.x;
    if (!mechicon_package_hud::blit(unit, unitIconX, unitIconY, true, dest, page.Width, static_cast<int>(unitIconX), static_cast<int>(unitIconY)))
        unit_icons::blit(hudSheet(false), unit->getIconPictureIndex(), true,
                         reinterpret_cast<uint32_t*>(dest), page.Width, static_cast<int>(unitIconX), static_cast<int>(unitIconY),
                         unit->getObjectType() ? unit->getObjectType()->getAppearanceTypeName() : "");
    std::mdspan<DWORD, std::dextents<size_t, 2>> pixels(page.pTexture, page.Height, page.Width);
    // Only the rear torso colours belong in the back paperdoll.
    for (int y = 0; y < unitIconY; ++y)
        for (int x = 0; x < unitIconX; ++x) {
            DWORD& pixel = pixels[rect.y + y, rect.x + x];
            bool draw = pixel == 0xff505050 || pixel == 0xffff00ff || pixel == 0xff202020;
            for (int ci = 0; ci < 3; ++ci) {
                const long first = (damageColors[0][ci] >> 16) + 4;
                if (static_cast<long>(pixel >> 16) >= first && static_cast<long>(pixel >> 16) <= first + 3) draw = true;
            }
            if (!draw) pixel = 0;
        }
    gos_UnLockTexture(handle);
}

bool ForceGroupIcon::inRegion( int x, int y )
{
	return ( x >= selectionRect[locationIndex].left && x <= selectionRect[locationIndex].right &&
			y >= selectionRect[locationIndex].top && y <= selectionRect[locationIndex].bottom );
}

void ForceGroupIcon::rightClick()
{
	(static_cast<GameCamera*>(eye))->setTarget( unit );
}

void ForceGroupIcon::click( bool shiftDn )
{
	if ( shiftDn && !unit->isSelected() )
	{
		unit->setSelected( true );
	}
	else if ( !shiftDn )
	{
		unit->setSelected( true );
	}
	else
	{
		unit->setSelected( false );
	}

	soundSystem->playDigitalSample( LOG_SELECT );
}

void ForceGroupIcon::init( )
{
}

bool MechIcon::init( Mover* pMover )
{
	ForceGroupIcon::init();

	if ( !pMover )
		return 0;

	long whichMech = pMover->getIconPictureIndex();
	unit = pMover;
	bool bRetVal = init( whichMech );
	pilotIcon.setPilot( pMover->getPilot() );

	return bRetVal;


}

bool MechIcon::init( long whichIndex )
{
    setIconVariables();
    const auto& rect = unit_icons::damageRect(damageIconIndex);
    const auto handle = unit_icons::damageTexture(damageIconIndex);
    TEXTUREPTR page = lockIconPage(handle);
    if (!page.pTexture) return false;
    DWORD* const dest = page.pTexture + rect.y * page.Width + rect.x;
    if (!mechicon_package_hud::blit(unit, unitIconX, unitIconY, false, dest, page.Width, static_cast<int>(unitIconX), static_cast<int>(unitIconY)))
        unit_icons::blit(hudSheet(false), whichIndex, false, reinterpret_cast<uint32_t*>(dest),
                         page.Width, static_cast<int>(unitIconX), static_cast<int>(unitIconY), (unit && unit->getObjectType()) ? unit->getObjectType()->getAppearanceTypeName() : "");
    gos_UnLockTexture(handle);

	for ( int i = 0; i < 5; ++i )
	{
		bmpLocation[locationIndex][i].argb = 0xffffffff;
		bmpLocation[locationIndex][i].frgb = 0;
		bmpLocation[locationIndex][i].rhw = .5;
		bmpLocation[locationIndex][i].u = 0.;
		bmpLocation[locationIndex][i].v = 0.;
		bmpLocation[locationIndex][i].z = 0.;
	}

	memset( damage, 0, 8 );
	memset( backDamage, 0, 8 );

	return true;

	
}

void MechIcon::update()
{
	// need to check object's status here
	char newDamage[8];
	char rearDamage[8];

	memset( newDamage, 0, 8 );
	memset( rearDamage, 0, 8 );

	// do check against armor location
	ArmorLocation armor = unit->armor[MECH_ARMOR_LOCATION_HEAD];
	float tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	if (unit->body[MECH_BODY_LOCATION_HEAD].damageState == IS_DAMAGE_DESTROYED)
		tmp = -1.0f;

	float backTmp = 0;

	if ( tmp < .0 )
		newDamage[7] = 3;
	else if ( tmp < .3 )
		newDamage[7] = 2;
	else if ( tmp < .6 )
		newDamage[7] = 1;

	tmp = unit->armor[MECH_ARMOR_LOCATION_CTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_CTORSO].maxArmor);
	if ( !bDrawBack )
	{
		if ( static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor) )
			tmp += unit->armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor);
		tmp/= 2.f;
	}
	else
	{
		backTmp = unit->armor[MECH_ARMOR_LOCATION_RCTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor);
	}
	
	if (unit->body[MECH_BODY_LOCATION_CTORSO].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
		backTmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[6] = 3;
	else if ( tmp < .3 )
		newDamage[6] = 2;
	else if ( tmp < .6 )
		newDamage[6] = 1;

	if ( backTmp < .0 )
		rearDamage[6] = 3;
	else if ( backTmp < .3 )
		rearDamage[6] = 2;
	else if ( backTmp < .6 )
		rearDamage[6] = 1;


	tmp = unit->armor[MECH_ARMOR_LOCATION_LTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_LTORSO].maxArmor);
	if ( !bDrawBack )
	{
		if ( static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor) )
			tmp += unit->armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor);
		tmp/= 2.f;
	}
	else
	{
		backTmp = unit->armor[MECH_ARMOR_LOCATION_RLTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor);

	}

	if (bMovie)
	{
		bool result = bMovie->update();
		if (result)
		{
			bMovie.reset();
			bMovie = nullptr;
		}
	}

	if (unit->body[MECH_BODY_LOCATION_LTORSO].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
		backTmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[5] = 3;
	else if ( tmp < .3 )
		newDamage[5] = 2;
	else if ( tmp < .6 )
		newDamage[5] = 1;

	if ( backTmp < .0 )
		rearDamage[5] = 3;
	else if ( backTmp < .3 )
		rearDamage[5] = 2;
	else if ( backTmp < .5 )
		rearDamage[5] = 1;


	tmp = unit->armor[MECH_ARMOR_LOCATION_RTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RTORSO].maxArmor);
	if ( !bDrawBack )
	{
		if ( static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor) )
			tmp += unit->armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor);
		tmp/= 2.f;
	}
	else
	{
		backTmp = unit->armor[MECH_ARMOR_LOCATION_RRTORSO].curArmor/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor);
	}
	
	if (unit->body[MECH_BODY_LOCATION_RTORSO].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
		backTmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[4] = 3;
	else if ( tmp < .3 )
		newDamage[4] = 2;
	else if ( tmp < .6 )
		newDamage[4] = 1;

	if ( backTmp < .0 )
		rearDamage[4] = 3;
	else if ( backTmp < .3 )
		rearDamage[4] = 2;
	else if ( backTmp < .4 )
		rearDamage[4] = 1;


	armor = unit->armor[MECH_ARMOR_LOCATION_LLEG];
	tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	if (unit->body[MECH_BODY_LOCATION_LLEG].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[0] = 3;
	else if ( tmp < .3 )
		newDamage[0] = 2;
	else if ( tmp < .6 )
		newDamage[0] = 1;

	armor = unit->armor[MECH_ARMOR_LOCATION_RLEG];
	tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	if (unit->body[MECH_BODY_LOCATION_RLEG].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[1] = 3;
	else if ( tmp < .3 )
		newDamage[1] = 2;
	else if ( tmp < .6 )
		newDamage[1] = 1;

	armor = unit->armor[MECH_ARMOR_LOCATION_LARM];
	tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	if (unit->body[MECH_BODY_LOCATION_LARM].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[2] = 3;
	else if ( tmp < .3 )
		newDamage[2] = 2;
	else if ( tmp < .6 )
		newDamage[2] = 1;

	armor = unit->armor[MECH_ARMOR_LOCATION_RARM];
	tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	if (unit->body[MECH_BODY_LOCATION_RARM].damageState == IS_DAMAGE_DESTROYED)
	{
		tmp = -1.0f;
		backTmp = -1.0f;
	}

	if ( tmp < .0 )
		newDamage[3] = 3;
	else if ( tmp < .3 )
		newDamage[3] = 2;
	else if ( tmp < .6 )
		newDamage[3] = 1;

	doDraw( newDamage, damage, -1, damageIconIndex );
	if ( bDrawBack )
	{
		float lratio = 1.f;
		float rratio = 1.f;
		if ( static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor) )
		{
			lratio = static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RLTORSO].maxArmor)/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor);
			rratio = static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RRTORSO].maxArmor)/static_cast<float>(unit->armor[MECH_ARMOR_LOCATION_RCTORSO].maxArmor);
		}
		rearDamage[7] = .5 + (rratio * rearDamage[4] + lratio * rearDamage[5] + rearDamage[6])/(lratio + rratio + 1.f);
		backDamage[7] = .5 + (rratio * backDamage[4] + lratio * backDamage[5] + backDamage[6])/(lratio + rratio + 1.f);
		doDraw( rearDamage, backDamage, -1, backDamageIndex );
	}

}

void MechIcon::doDraw( char* newDamage, char* oldDamage, unsigned long handle, unsigned long index )
{
	int i = 0; 
	int maxToCheck = 8;
	if ( index == backDamageIndex )
		i = 7;

	for ( ; i < maxToCheck; ++i )
	{
		// if damage has changeed, update that part of the icon, we really don't want to do this
		// every frame
		if ( newDamage[i] != oldDamage[i] )
		{			
            const auto& rect = unit_icons::damageRect(index);
            const auto textureHandle = unit_icons::damageTexture(index);
            TEXTUREPTR textureData = lockIconPage(textureHandle);
            if (!textureData.pTexture) continue;
            DWORD* pLine = textureData.pTexture + rect.y * textureData.Width + rect.x;
			DWORD* pChange = pLine;

			DWORD oldColors[3];
			DWORD newColors[3];

			// find the colors we're looking for
			for ( int p = 0; p < 3; p++ )
			{
				int tmp = damageColors[damage[i]][p];
				int redVal = (tmp >> 16) & 0x000000ff;
				redVal += i;
				oldColors[p] = ( tmp & 0xff00ffff ) | (redVal << 16);

				tmp = damageColors[newDamage[i]][p];
				redVal = (tmp >> 16) & 0x000000ff;
				redVal += i;
				newColors[p] = ( tmp & 0xff00ffff ) | (redVal << 16);

			}
			
			// look through the entire bitmap, changing old colors to new colors
			for ( int k = 0; k < unitIconY; ++k )
			{
				pChange = pLine;
				for ( int j = 0; j < unitIconX; ++j )
				{

					if ( (*pChange)&0xff000000 ) // not transparent
					{
						for (int p = 0; p < 3; p++ )
						{
							if ( oldColors[p] == *pChange )		
							{
								*pChange = newColors[p];
							}
						}
					}
					pChange++;

				}
				pLine += textureData.Width;				
			}

			gos_UnLockTexture(textureHandle);
		}
		oldDamage[i] = newDamage[i];
	}


}

bool IsDbcsString(mc2::czstring pszString)
{
	if (pszString) {
		while (*pszString) {
			if (IsDBCSLeadByte(static_cast<BYTE>(*pszString)))
				return true;
			pszString++;
		}
	}
	return false;
}


void ForceGroupIcon::render()
{
	const auto& row = mc2::render_view::hud().mover(unit);
	if ( !unit )
		return;

	int forceGroup = -1;
	for ( int i = 0; i < 10; ++i )
	{
		if ( (row.groups & (1u << i)) )
			forceGroup = i;
	}

	
	// draw little colored background
	if ( forceGroup == 0 )
		forceGroup = 10;
	int color = forceGroup == -1 ? ForceGroupColors[10] : ForceGroupColors[forceGroup-1];
	float health = row.health;

	
	// draw pilotName
	
	GUI_RECT tmpRect = { textArea[locationIndex].left, textArea[locationIndex].top, textArea[locationIndex].right, textArea[locationIndex].bottom };
	drawRect( tmpRect, color);

	char buffer[256];
	strcpy( buffer, row.pilotName.c_str() );
	for (int i = 0; i < strlen( buffer ); i++ )
		CharUpper( buffer );

	// Auto-shrink font size (rather than chop characters) when the pilot name
	// is too wide for the slot. PALERIDEPSYCHO etc. render in full but at a
	// smaller size. MC2 uses NEGATIVE sizes to mean "pixel height", so we
	// scale by shrinking magnitude toward zero; clamp to a legible floor.
	const int slotW = textArea[locationIndex].right - textArea[locationIndex].left - 2;
	const float origSize = static_cast<float>(gosFontHandle->getSize());
	float useSize = origSize;
	{
		gos_TextSetAttributes( gosFontHandle->getTempHandle(), 0xffffffff, useSize,
		                       true, true, false, false, 0 );
		DWORD tw = 0, th = 0;
		gos_TextStringLength( &tw, &th, buffer );
		const float minMagnitude = 8.0f;
		while ( static_cast<int>(tw) > slotW && fabsf(useSize) > minMagnitude ) {
			useSize *= 0.9f;
			gos_TextSetAttributes( gosFontHandle->getTempHandle(), 0xffffffff, useSize,
			                       true, true, false, false, 0 );
			gos_TextStringLength( &tw, &th, buffer );
		}
	}

	// Draw with the chosen (possibly shrunken) size.
	gos_TextSetAttributes( gosFontHandle->getTempHandle(), 0xffffffff, useSize,
	                       true, true, false, false, 0 );
	gos_TextSetRegion( textArea[locationIndex].left + 1,
	                   pilotTextTop[locationIndex],
	                   textArea[locationIndex].left + 1 + slotW,
	                   pilotTextTop[locationIndex] + 64 );
	gos_TextSetPosition( textArea[locationIndex].left + 1,
	                     pilotTextTop[locationIndex] );
	gos_TextDraw( buffer );

	// draw the health bar
	drawBar( health );

	// draw the little unit icon
	renderUnitIcon( bmpLocation[locationIndex][0].x, bmpLocation[locationIndex][0].y, bmpLocation[locationIndex][2].x, bmpLocation[locationIndex][2].y );
	
	pilotIcon.render( pilotLocation[locationIndex][0], pilotLocation[locationIndex][1], pilotLocation[locationIndex][2], pilotLocation[locationIndex][3] );

	drawEmptyRect( pilotRect[locationIndex], 0xff002f55, 0xff002f55 );



	// if has jumpjets, draws jump jet icon
	if ( row.canJump )
	{
		jumpJetIcon->move( selectionRect[locationIndex].left, selectionRect[locationIndex].bottom );
		jumpJetIcon->render( );
		jumpJetIcon->move( -selectionRect[locationIndex].left, -selectionRect[locationIndex].bottom );
	}
	



	if ( unit && pilotVideoPilot == row.pilot  )
	{
		gos_VERTEX v[4];
		for (int i = 0; i < 4; i++ )
		{
			v[i] = bmpLocation[locationIndex][i];
			v[i].u = 0.f;
			v[i].v = 0.f;
		}

		v[2].x = v[3].x = pilotLocation[locationIndex][2];
		v[2].u = v[3].u = .59375;
		v[1].v = v[2].v = .828125f;

		RECT vRect;
		vRect.left 		= bmpLocation[locationIndex][1].x;
		vRect.right 	= pilotLocation[locationIndex][2];
		vRect.top 		= bmpLocation[locationIndex][3].y;
		vRect.bottom 	= bmpLocation[locationIndex][1].y;

		if ( bMovie )
		{
			if ( unit && (row.shutdown) )
			{
				bMovie->stop();
				bMovie.reset();
				bMovie = nullptr;
				pilotVideoPilot = nullptr;
			}
			else
			{
				bMovie->setRect(vRect);
				bMovie->render();
			}
		}
		else if ( pilotVideoTexture )
		{
			gos_SetRenderState( gos_State_Texture,  pilotVideoTexture );
			v[1].v = v[2].v = .4140625;

			gos_DrawQuads( v, 4 );
		}
	}


	if ( unit && (row.shutdown) )
	{
		drawRect( selectionRect[locationIndex], 0xaa000000 );
	}


	// if selected, draw white rectangle
	if ( row.selected )
	{
		drawEmptyRect( selectionRect[locationIndex], 0xffffffff, 0xffffffff );
	}

	if ( row.pilot )
	{
		if ( row.messagePlaying )
		{
			long color = 0xffffff00;
			msgPlayTime += mc2::render_view::clock().dt;
			if ( msgPlayTime > .25 )
				msgPlayTime = 0;
			if ( msgPlayTime > .125 )
				color = 0x00000000;
			drawEmptyRect( selectionRect[locationIndex], color, color );
		}
		else
			msgPlayTime = 0;
	}

	// draw dying effect
	if ( deathAnimationTime )
	{
		drawDeathEffect();
	}


}

void ForceGroupIcon::renderUnitIcon( float left, float top, float right, float bottom )
{
    float u, v, u1, v1;
    uint64_t textureId = 0;
    if (!getUnitIconTexUV(textureId, u, v, u1, v1)) return;
    const float uDelta = u1 - u, vDelta = v1 - v;
    gos_SetRenderState(gos_State_Texture, unit_icons::damageTexture(damageIconIndex));
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	gos_SetRenderState( gos_State_AlphaTest, true);


	bmpLocation[locationIndex][0].u = bmpLocation[locationIndex][1].u = u;
	bmpLocation[locationIndex][2].u = bmpLocation[locationIndex][3].u = u + uDelta;
	bmpLocation[locationIndex][0].v = bmpLocation[locationIndex][3].v = v;
	bmpLocation[locationIndex][1].v = bmpLocation[locationIndex][2].v = v + vDelta;
	
	bmpLocation[locationIndex][0].x = 	bmpLocation[locationIndex][1].x = left;
	bmpLocation[locationIndex][2].x = bmpLocation[locationIndex][3].x = right;
	bmpLocation[locationIndex][0].y = bmpLocation[locationIndex][3].y = top;
	bmpLocation[locationIndex][1].y = bmpLocation[locationIndex][2].y  = bottom;
	bmpLocation[locationIndex][4] = bmpLocation[locationIndex][0];

	gos_DrawTriangles( bmpLocation[locationIndex], 3 );
	gos_DrawTriangles( &bmpLocation[locationIndex][2], 3 );
			

}

void ForceGroupIcon::renderUnitIconBack( float left, float top, float right, float bottom )
{
    float u, v, u1, v1;
    uint64_t textureId = 0;
    if (!getUnitIconBackTexUV(textureId, u, v, u1, v1)) return;
    const float uDelta = u1 - u, vDelta = v1 - v;
    gos_SetRenderState(gos_State_Texture, unit_icons::damageTexture(backDamageIndex));
	gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
	gos_SetRenderState( gos_State_Filter, gos_FilterNone);
	gos_SetRenderState( gos_State_AlphaTest, true);

	gos_VERTEX vertices[5];

	for (auto & vertex : vertices)
	{
		vertex.argb = 0xffffffff;
		vertex.frgb = 0;
		vertex.z = 0;
		vertex.rhw = .5;
	}

	vertices[0].u = vertices[1].u = u;
	vertices[2].u = vertices[3].u = u + uDelta;
	vertices[0].v = vertices[3].v = v;
	vertices[1].v = vertices[2].v = v + vDelta;
	
	vertices[0].x = 	vertices[1].x = left;
	vertices[2].x = vertices[3].x = right;
	vertices[0].y = vertices[3].y = top;
	vertices[1].y = vertices[2].y  = bottom;
	vertices[4] = vertices[0];

	gos_DrawTriangles( vertices, 3 );
	gos_DrawTriangles( &vertices[2], 3 );
	
}
void ForceGroupIcon::renderPilotIcon( float left, float top, float right, float bottom )
{
	pilotIcon.render( left, top, right, bottom );
}

static bool damageTexUV(long slot, float width, float height, uint64_t& texture,
                        float& u0, float& v0, float& u1, float& v1)
{
    if (slot < 0) return false;
    const auto& rect = unit_icons::damageRect(slot);
    const auto handle = unit_icons::damageTexture(slot);
    GosTexels texels = gos_GetTextureTexels(handle);
    if (!texels.width || !texels.height) return false;
    u0 = (rect.x + .1f) / texels.width; v0 = (rect.y + .1f) / texels.height;
    u1 = (rect.x + width + .2f) / texels.width; v1 = (rect.y + height + .2f) / texels.height;
    texture = gos_GetUiTextureId(handle);
    return texture != 0;
}

bool ForceGroupIcon::getUnitIconTexUV(uint64_t& texture, float& u0, float& v0, float& u1, float& v1) const
{
    return damageTexUV(damageIconIndex, unitIconX, unitIconY, texture, u0, v0, u1, v1);
}

unsigned long ForceGroupIcon::getUnitIconTexture() const
{
    return unit_icons::damageTexture(damageIconIndex);
}

bool ForceGroupIcon::getUnitIconBackTexUV(uint64_t& texture, float& u0, float& v0, float& u1, float& v1) const
{
    return damageTexUV(backDamageIndex, unitIconX, unitIconY, texture, u0, v0, u1, v1);
}

bool ForceGroupIcon::getPilotIconTexUV( uint64_t& glTex, float& u0, float& v0, float& u1, float& v1 ) const
{
	return pilotIcon.getTexUV( glTex, u0, v0, u1, v1 );
}

// INFO-IMGUI-SLICE-2: mirrors PilotIcon::render's UV math (note: the legacy
// code divides BOTH u and v by s_pilotTextureWidth — the pilot atlas is square;
// mirrored verbatim for pixel parity).
bool PilotIcon::getTexUV( uint64_t& glTex, float& u0, float& v0, float& u1, float& v1 ) const
{
	if ( !s_pilotTextureHandle || !s_pilotTextureWidth || pilotIconX <= 0.f )
		return false;

	int iconsPerRow = s_pilotTextureWidth / pilotIconX;
	if ( iconsPerRow <= 0 )
		return false;
	int xIndex = pilotTextureIndex % iconsPerRow;
	int yIndex = pilotTextureIndex / iconsPerRow;

	float xLocation = xIndex * pilotIconX;
	float yLocation = yIndex * pilotIconY;
	u0 = xLocation/s_pilotTextureWidth + (.1f/s_pilotTextureWidth);
	u1 = (xLocation + pilotIconX)/s_pilotTextureWidth+ (.1f/s_pilotTextureWidth);
	v0 = yLocation/s_pilotTextureWidth+ (.1f/s_pilotTextureWidth);
	v1 = (yLocation + pilotIconY)/s_pilotTextureWidth+ (.1f/s_pilotTextureWidth);

	long gosID = mc2::textures::sourceId( s_pilotTextureHandle );
	if ( gosID <= 0 )   // 0 = none, -1 (0xffffffff) = invalid
		return false;
	glTex = gos_GetUiTextureId( static_cast<DWORD>(gosID) );
	return glTex != 0;
}


bool VehicleIcon::init( Mover* pMover )
{
    setIconVariables();
    pilotIcon.setTextureIndex(26);
    if (!pMover) return false;
    const auto& rect = unit_icons::damageRect(damageIconIndex);
    const auto textureHandle = unit_icons::damageTexture(damageIconIndex);
    TEXTUREPTR textureData = lockIconPage(textureHandle);
    if (!textureData.pTexture) return false;
    unit_icons::blit(hudSheet(true), pMover->getIconPictureIndex(), false,
                     reinterpret_cast<uint32_t*>(textureData.pTexture + rect.y * textureData.Width + rect.x),
                     textureData.Width, static_cast<int>(unitIconX), static_cast<int>(unitIconY), pMover->getObjectType()->getAppearanceTypeName());
    gos_UnLockTexture(textureHandle);
	unit = pMover;


	for ( int i = 0; i < 5; ++i )
	{
		bmpLocation[locationIndex][i].argb = 0xffffffff;
		bmpLocation[locationIndex][i].frgb = 0;
		bmpLocation[locationIndex][i].rhw = .5;
		bmpLocation[locationIndex][i].u = 0.;
		bmpLocation[locationIndex][i].v = 0.;
		bmpLocation[locationIndex][i].z = 0.;
	}

	memset( damage, 0, 5 );

	return true;

}

void VehicleIcon::update()
{
	char newDamage[5];

	memset( newDamage, 0, 5 );

	// do check against armor location
	ArmorLocation armor = unit->armor[GROUNDVEHICLE_LOCATION_FRONT];
	float tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	
	if ( tmp < .1 )
		newDamage[0] = 3;
	else if ( tmp < .3 )
		newDamage[0] = 2;
	else if ( tmp < .6 )
		newDamage[0] = 1;

	tmp = unit->armor[GROUNDVEHICLE_LOCATION_REAR].curArmor/static_cast<float>(unit->armor[GROUNDVEHICLE_LOCATION_REAR].maxArmor);
	if ( tmp < .1 )
		newDamage[1] = 3;
	else if ( tmp < .3 )
		newDamage[1] = 2;
	else if ( tmp < .6 )
		newDamage[1] = 1;

	tmp = unit->armor[GROUNDVEHICLE_LOCATION_LEFT].curArmor/static_cast<float>(unit->armor[GROUNDVEHICLE_LOCATION_LEFT].maxArmor);
		
	if ( tmp < .1 )
		newDamage[2] = 3;
	else if ( tmp < .3 )
		newDamage[2] = 2;
	else if ( tmp < .6 )
		newDamage[2] = 1;


	tmp = unit->armor[GROUNDVEHICLE_LOCATION_RIGHT].curArmor/static_cast<float>(unit->armor[GROUNDVEHICLE_LOCATION_RIGHT].maxArmor);

	if ( tmp < .1 )
		newDamage[3] = 3;
	else if ( tmp < .3 )
		newDamage[3] = 2;
	else if ( tmp < .6 )
		newDamage[3] = 1;

	armor = unit->armor[GROUNDVEHICLE_LOCATION_TURRET];
	tmp = armor.curArmor/static_cast<float>(armor.maxArmor);
	if ( tmp < .1 )
		newDamage[4] = 3;
	else if ( tmp < .3 )
		newDamage[4] = 2;
	else if ( tmp < .6 )
		newDamage[4] = 1;


	for ( int i = 0; i < 5; ++i )
	{
		if ( newDamage[i] != damage[i] )
		{	
            const auto& rect = unit_icons::damageRect(damageIconIndex);
            const auto textureHandle = unit_icons::damageTexture(damageIconIndex);
            TEXTUREPTR textureData = lockIconPage(textureHandle);
            if (!textureData.pTexture) continue;
            DWORD* pLine = textureData.pTexture + rect.y * textureData.Width + rect.x;
			DWORD* pChange = pLine;

			DWORD oldColors[3];
			DWORD newColors[3];

			for ( int p = 0; p < 3; p++ )
			{
				int tmp = damageColors[damage[i]][p];
				int redVal = (tmp >> 16) & 0x000000ff;
				redVal += i;
				oldColors[p] = ( tmp & 0xff00ffff ) | (redVal << 16);

				tmp = damageColors[newDamage[i]][p];
				redVal = (tmp >> 16) & 0x000000ff;
				redVal += i;
				newColors[p] = ( tmp & 0xff00ffff ) | (redVal << 16);

			}
			
	
			for ( int k = 0; k < unitIconY; ++k )
			{
				pChange = pLine;
				for ( int j = 0; j < unitIconX; ++j )
				{
					if ( (*pChange)&0xff000000 ) // not transparent
					{
						for ( int p = 0; p < 3; p++ )
						{
							if ( oldColors[p] == *pChange )		
							{
								*pChange = newColors[p];
							}
						}
					}
					pChange++;

				}
				pLine += textureData.Width;				
			}

			gos_UnLockTexture(textureHandle);
		}
		damage[i] = newDamage[i];
	}
}

void ForceGroupIcon::drawBar( float barStatus )
{
	
	DWORD					color = SB_GREEN;
	
	float barLength =  (healthBar[locationIndex].right - healthBar[locationIndex].left) * barStatus;
		
	/*if (barStatus >= 0.5)
		color = SB_GREEN;
	else if (barStatus > 0.2)
		color = SB_YELLOW;
	else 
		color = SB_RED;*/
		
	
	GUI_RECT rect = { healthBar[locationIndex].left, healthBar[locationIndex].top,healthBar[locationIndex].left + static_cast<int>(barLength), healthBar[locationIndex].bottom };
	drawRect( rect, color );
	drawEmptyRect( healthBar[locationIndex],  color, color & 0x7f7f7f );
}

int ForceGroupIcon::sort( const void* p1, const void* p2 )
{
	ForceGroupIcon* m1 = *static_cast<ForceGroupIcon**>(const_cast<void*>(p1));
	ForceGroupIcon* m2 = *static_cast<ForceGroupIcon**>(const_cast<void*>(p2));

	bool bv1 = dynamic_cast<VehicleIcon*>(m1) ? 1 : 0;
	bool bv2 = dynamic_cast<VehicleIcon*>(m2) ? 1 : 0;


	for ( int i = 1; i < 10; ++i )
	{
		bool bM1 =  m1->unit->isInUnitGroup( i == 10 ? 0 : i );
		bool bM2 =  m2->unit->isInUnitGroup( i == 10 ? 0 : i );

		if ( bM1 && !bM2 )
			return -1;
		if ( bM2 && !bM1 )
			return 1;
		if ( bM2 && bM1 ) // if same force group, use weight
		{
			break;
		}
	}

	if ( bv2 && !bv1 )
		return -1;
	else if ( bv1 && !bv2 )
		return 1;
	

	int w1 = m1->unit->getTonnage();
	int w2 = m2->unit->getTonnage();

	if ( w1 < w2  )
		return -1;
	else if ( w2 < w1)
		return 1;

	// OK these are exactly the same, compare pilot names
	int retVal = ( strcmp( m1->getPilotName(), m2->getPilotName() ) );

	if ( retVal == 0 )
		return m1 < m2;

	return retVal;
}

const char* ForceGroupIcon::getPilotName()
{
	return unit->getPilot()->getName();
}

void ForceGroupIcon::setLocationIndex( int i )
{ 
	locationIndex = i; 
}

// Draw only: reads deathAnimationTime. The deselection, the clock and the group removal live in
// updateDeathAnimation (ForceGroupBar::update, quiescent frame part); the draw overlaps the sim worker.
void ForceGroupIcon::drawDeathEffect()
{
	bool bFinished = true;
    int i = 0;
	for (; i < NUM_DEATH_INFOS - 1; i++ )
	{
		if ( animationInfos[i].time < deathAnimationTime &&
			 animationInfos[i+1].time > deathAnimationTime )
		{
			float totalTime = animationInfos[i+1].time - animationInfos[i].time;
			float currentTime = deathAnimationTime - animationInfos[i].time;

			float percent = currentTime/totalTime;
			
			// since I know we're going from white to black, I'm only going to much with the alpha separately
			uint32_t alphaMin = (animationInfos[i].color >> 24) & 0xff;
			uint32_t alphaMax = (animationInfos[i+1].color >> 24)& 0xff;

			uint32_t newAlpha = static_cast<float>(alphaMin) + (static_cast<float>((alphaMax - alphaMin)))*percent;
			
			uint32_t redMin = (animationInfos[i].color & 0x00ff0000)>>16;
			uint32_t redMax = (animationInfos[i+1].color & 0x00ff0000)>>16;
			uint32_t newRed = static_cast<float>(redMin) + (static_cast<float>((redMax - redMin)))*percent;
			
			uint32_t greenMin = (animationInfos[i].color & 0x0000ff00)>>8;
			uint32_t greenMax = (animationInfos[i+1].color & 0x0000ff00)>>8;
			uint32_t newGreen = static_cast<float>(greenMin) + (static_cast<float>((greenMax - greenMin)))*percent;
			
			uint32_t blueMin = animationInfos[i].color & 0x000000ff;
			uint32_t blueMax = animationInfos[i+1].color & 0x000000ff;
			uint32_t newBlue = static_cast<float>(blueMin) + (static_cast<float>((blueMax - blueMin)))*percent;

			uint32_t newColor = newBlue + (newGreen << 8) + (newRed << 16) + (newAlpha <<24);
							
			drawRect( selectionRect[locationIndex], newColor );
			bFinished = false;

			if ( i == 0 )
			{
				long color = 0xff000000;
				if ( deathAnimationTime < .4 
					|| (deathAnimationTime > .8 && deathAnimationTime < 1.2)
					|| (deathAnimationTime > 1.6 && deathAnimationTime < 2.0))
				{
					color = 0xffff0000;
				}
				drawEmptyRect( selectionRect[locationIndex], color, color );
			}
		}
	}

	if ( bFinished )
	{
		drawRect( selectionRect[locationIndex], animationInfos[i].color );
	}
}

// Main, frame part (ForceGroupBar::update, before extract and the kick): the sim-side half of the death
// animation. Same order as the old draw: the frame that begins the animation adds no time, each later frame
// adds frameLength, and the group removal runs once, on the frame the icon is deleted.
void ForceGroupIcon::updateDeathAnimation()
{
	if ( !deathAnimationTime )
		beginDeathAnimation();
	else
		deathAnimationTime += frameLength;
	unit->setSelected( 0 );
	if ( deathAnimationOver() )
	{
		unit->addToUnitGroup( -1 );
		//------------------------------------------------------------
		// This tells all machines to drop this mover from the team...
		if (MPlayer)
			MPlayer->sendReinforcement(0, unit->netRosterIndex, "noname", 0, unit->getPosition(), 7);
	}
}

void ForceGroupIcon::setIconVariables()
{
	switch( Environment.screenWidth )
	{
		case 800:
			PilotIcon::pilotIconX = 25;
			PilotIcon::pilotIconY = 36;
			unitIconX = 32;
			unitIconY = 38;
			break;

		case 640:
			PilotIcon::pilotIconX = 20;
			PilotIcon::pilotIconY = 28;
			unitIconX = 25;
			unitIconY = 30;
			break;

		default:
			PilotIcon::pilotIconX = 33;
			PilotIcon::pilotIconY = 46;
			unitIconX = 40;
			unitIconY = 48;
			break;

	}

	curScreenWidth = Environment.screenWidth;
}

PilotIcon::PilotIcon()
{
	pilotTextureIndex = 0;

	if ( !s_pilotTextureHandle )
	{
		// find proper file based on resolution
		char path[256];
		strcpy( path, "data" PATH_SEPARATOR "art" PATH_SEPARATOR);
		if ( Environment.screenWidth == 640 )
		{
			strcat( path, "mcui_low2.tga" );
		}
		else if ( Environment.screenWidth == 800 )
		{
			strcat( path, "mcui_med2.tga" );
		}
		else
		{
			strcat( path, "mcui_high2.tga" );
		}

		s_pilotTexturesKey = AssetScale::key(path);
		s_pilotTextureHandle = mc2::textures::load( path, gos_Texture_Alpha, 0, 0, 0x2);

		// Find its logical width without forcing texture realization when metadata is available.
		DWORD logicalWidth = 0;
		DWORD logicalHeight = 0;
		if ( mc2::textures::logicalSize( s_pilotTextureHandle, logicalWidth, logicalHeight ) )
			s_pilotTextureWidth = logicalWidth;
		else
		{
            s_pilotTextureWidth=RenderCore::textureService().dimensions(RenderCore::textureService().handleOfNode(s_pilotTextureHandle)).logicalWidth;
		}
	}

}

void PilotIcon::render( float left, float top, float right, float bottom ) 
{
	gos_VERTEX pilotLocation[5];
	for (auto & pilotLocationElem : pilotLocation)
	{
		pilotLocationElem.argb = 0xffffffff;
		pilotLocationElem.rhw = .5;
		pilotLocationElem.frgb = 0;
		pilotLocationElem.z = 0;
	}
	pilotLocation[0].x = pilotLocation[1].x = left;
	pilotLocation[3].x = pilotLocation[2].x = right;
	pilotLocation[0].y = pilotLocation[3].y = top;
	pilotLocation[1].y = pilotLocation[2].y = bottom;
	pilotLocation[4] = pilotLocation[0];

	int iconsPerRow = s_pilotTextureWidth / pilotIconX;
	int xIndex = pilotTextureIndex % iconsPerRow;
	int yIndex = pilotTextureIndex / iconsPerRow;

	float xLocation = xIndex * pilotIconX;
	float yLocation = yIndex * pilotIconY;
	pilotLocation[0].u = pilotLocation[1].u = xLocation/s_pilotTextureWidth + (.1f/s_pilotTextureWidth);
	pilotLocation[3].u = pilotLocation[2].u = (xLocation + pilotIconX)/s_pilotTextureWidth+ (.1f/s_pilotTextureWidth);
	pilotLocation[0].v = pilotLocation[3].v = yLocation/s_pilotTextureWidth+ (.1f/s_pilotTextureWidth);
	pilotLocation[1].v = pilotLocation[2].v = (yLocation + pilotIconY)/s_pilotTextureWidth+ (.1f/s_pilotTextureWidth);
	pilotLocation[4] = pilotLocation[0];


	int gosID = mc2::textures::sourceId( s_pilotTextureHandle );
	gos_SetRenderState( gos_State_Texture, gosID );
	// here I need to draw the pilot icon
	gos_DrawTriangles( pilotLocation, 3 );
	gos_DrawTriangles( &pilotLocation[2], 3 );

}

void PilotIcon::setPilot( MechWarrior* pWarrior )
{
	if ( pWarrior )
		pilotTextureIndex = pWarrior->getPhoto();
}

void PilotIcon::swapResolutions( bool bForce)
{
	if ( s_pilotTextureHandle )
	{
		mc2::textures::release( s_pilotTextureHandle );
	}
	s_pilotTextureHandle = 0;

	if ( !s_pilotTextureHandle )
	{
		// find proper file based on resolution
		char path[256];
		strcpy( path, "data" PATH_SEPARATOR "art" PATH_SEPARATOR);
		if ( Environment.screenWidth == 640 )
		{
			strcat( path, "mcui_low2.tga" );
		}
		else if ( Environment.screenWidth == 800 )
		{
			strcat( path, "mcui_med2.tga" );
		}
		else
		{
			strcat( path, "mcui_high2.tga" );
		}

		s_pilotTexturesKey = AssetScale::key(path);
		s_pilotTextureHandle = mc2::textures::load( path, gos_Texture_Alpha, 0, 0, 0x2);

		// Find its logical width without forcing texture realization when metadata is available.
		DWORD logicalWidth = 0;
		DWORD logicalHeight = 0;
		if  ( mc2::textures::logicalSize( s_pilotTextureHandle, logicalWidth, logicalHeight ) )
			s_pilotTextureWidth = logicalWidth;
		else
		{
            s_pilotTextureWidth=RenderCore::textureService().dimensions(RenderCore::textureService().handleOfNode(s_pilotTextureHandle)).logicalWidth;
		}
	}

}

//*************************************************************************************************
// end of file ( MechIcon.cpp )
