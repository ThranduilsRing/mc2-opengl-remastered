#ifndef INFOWINDOWMODEL_H
#define INFOWINDOWMODEL_H
#include <span>
/*************************************************************************************************\
infowindowmodel.h : pure component-list aggregation for the in-mission info window
                    (IN-MISSION-HUD-IMGUI-SCOPE-1 slice D, no such gate).

Extracted from the weapon-dedupe loop inside InfoWindow::drawScrollingStuff so the
ImGui panel and the offline unit tests share one implementation. The LEGACY draw
path keeps its own inline copy untouched (gate-off byte-parity).

Contract (mirrors the legacy loop exactly):
  - Rows are deduped by masterID in FIRST-SEEN order.
  - totalCount counts every occurrence; workingCount counts occurrences with
    working == true.
  - ammo comes from the FIRST occurrence only (legacy: master ammo list is
    already combined across weapons of one type); -1 == "no ammo line".
  - Header-only, no engine includes -- unit-testable offline
    (tests/unit/test_infowindow_model.cpp).
\*************************************************************************************************/

namespace InfoWindowModel
{
	struct WeaponRec
	{
		unsigned char masterID;
		bool working;   // !disabled && shots > 0 && health > 0
		long ammo;      // -1 = unlimited / no ammo type (no ammo line drawn)
		int  range;     // 0 short, 1 medium, 2 long
	};

	struct ComponentRow
	{
		unsigned char masterID;
		int  workingCount;
		int  totalCount;
		long ammo;
		int  range;
	};

	inline int aggregateWeapons( std::span<const WeaponRec> recs,
								 std::span<ComponentRow> out )
	{
		if ( recs.empty() || out.empty() )
			return 0;
		int count = 0;
		for (const auto & rec : recs)
		{
			int found = -1;
			for ( int j = 0; j < count; ++j )
			{
				if ( out[j].masterID == rec.masterID )
				{
					found = j;
					break;
				}
			}
			if ( found >= 0 )
			{
				out[found].totalCount++;
				if ( rec.working )
					out[found].workingCount++;
				continue;   // legacy: duplicates do NOT add ammo
			}
			if ( static_cast<size_t>(count) >= out.size() )
				continue;
			ComponentRow& row = out[count++];
			row.masterID = rec.masterID;
			row.totalCount = 1;
			row.workingCount = rec.working ? 1 : 0;
			row.ammo = rec.ammo;
			row.range = rec.range;
		}
		return count;
	}

	// Legacy InfoWindow::drawSkillBar segment count: skill/NUMBERSKILLBARS,
	// guarded against a zero/negative divisor from a bad .fit.
	inline int skillBarCount( int skill, int unitsPerBar )
	{
		if ( unitsPerBar <= 0 || skill <= 0 )
			return 0;
		return skill / unitsPerBar;
	}
}

#endif // INFOWINDOWMODEL_H
