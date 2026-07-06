/*************************************************************************************************\
hudtabregistry.cpp : HudTabs::Registry out-of-line bits (slice C, MC2_HUD_IMGUI_TABS).
Pure logic -- no GameOS/GL/ImGui. Also linked into tests/unit (leaf TU).
\*************************************************************************************************/

#include "hudtabregistry.h"

#include <string.h>

namespace HudTabs
{

int Registry::indexOfId( const char* id ) const
{
	if ( !id )
		return -1;
	for ( int i = 0; i < count_; ++i )
		if ( 0 == strcmp( tabs_[i].id, id ) )
			return i;
	return -1;
}

} // namespace HudTabs
