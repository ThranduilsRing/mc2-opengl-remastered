//------------------------------------------------------------------------------
// UnitActionFlowPanel
//
// Heuristically parses a unit's brain .abl TEXT into a readable condition ->
// trigger -> action flow ("Unit Action Flow") and renders it INLINE inside the
// existing AI/Brain panel (UnitBrainPanel). e.g. mc2_01 Starslayer: powered-down
// state -> wake trigger -> patrol/guard.
//
// READ-ONLY + ADDITIVE: takes the .abl text the brain panel already loads
// (loadBrainText), so it touches no file IO and no brain-arc state. Resilient
// line-scan, tolerant of ABL variation (partial extraction is fine).
//------------------------------------------------------------------------------
#ifndef UNIT_ACTION_FLOW_PANEL_H
#define UNIT_ACTION_FLOW_PANEL_H

namespace UnitActionFlowPanel
{
	// Render the brain flow for the given .abl text as a collapsing section.
	// Pass loadBrainText(brainName) from the AI/Brain panel. Cached per text.
	void DrawInline(const char* ablText);
}

#endif // UNIT_ACTION_FLOW_PANEL_H
