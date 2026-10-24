// LOCAL-LIGHTS-3: mech and vehicle lamps as real spot lights -- the one implementation behind
// Mech3DAppearance and GVAppearance (lamp registration per SpotLight_ leaf; per-frame aim).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <vector>
#include "cone_glow.h"
#include "spotlight_diag.h"
#include "tgl.h"
#include "camera.h"

namespace mc2_mover_lamps {

inline constexpr int   kMaxLamps = 8;
// A coned lamp is no longer dimmed as a mover pool, so it is well below the building lamps.
inline constexpr float kLampIntensity = 0.25f;

// One halogen world light per authored lamp of the leaf (the node id repeats once per lamp, in lamp order).
// Returns false when the world-light pool had no slot for a lamp.
template <class NodeIds, class Lights, class Slots>
bool registerLamps(TG_MultiShape* shape, long nodeId, NodeIds& nodeIds, Lights& lights, Slots& slots,
                   mc2_spotlight_diag::SourceClass src, const void* owner)
{
	const int nLamps = std::max(1, mc2_cone_glow::GetMoverBeams(shape, nodeId, glm::vec3(0.0f), nullptr, kMaxLamps));
	for (int lamp = 0; lamp < nLamps; ++lamp)
	{
		TG_LightPtr light = static_cast<TG_LightPtr>(malloc(sizeof(TG_Light)));
		light->init(TG_LIGHT_POINT);              // v1 (OQ2/M6) -- POINT
		light->SetaRGB(mc2_spotlight_diag::mover_light_argb());   // MOVER-LIGHT-HALOGEN-1: warm halogen, not legacy yellow
		light->SetIntensity(0.15f);                // anubis default
		light->SetFalloffDistances(50.0f, 250.0f); // anubis default
		light->moverLamp = true;                    // LIGHTS-R4: dynamic-only shadow tile
		const long slotId = eye->addWorldLight(light);
		if (slotId < 0) { free(light); return false; }
		nodeIds.push_back(nodeId);
		lights.push_back(light);
		slots.push_back(static_cast<DWORD>(slotId));
		mc2_spotlight_diag::tag_slot(eye, slotId, src, owner);   // T1.16: (E)-owned
	}
	return true;
}

// Per-frame walk over a mover's lights: the k-th light of a node is that node's k-th lamp.
struct LampCursor
{
	mc2_cone_glow::NodeBeam lamps[kMaxLamps];
	int nLamps = 0, lampIdx = 0;
	long lampNode = -1;

	// Aims `light` (the next one, on `nodeId`) at its lamp: a real spot light at the lamp (soft cone, every receiver,
	// shadow tile). `childPos` moves to the lamp apex. Returns false when the node has no usable lamp cone.
	bool next(TG_MultiShape* shape, long nodeId, TG_LightPtr light, glm::vec3& childPos)
	{
		if (nodeId != lampNode)
		{
			lampNode = nodeId;
			nLamps = mc2_cone_glow::GetMoverBeams(shape, lampNode, childPos, lamps, kMaxLamps);
			lampIdx = 0;
		}
		else
			++lampIdx;
		light->spotCosOuter = 0.0f;
		const bool aimed = lampIdx < nLamps && lamps[lampIdx].aim.z <= 0.15f;
		if (aimed)
		{
			mc2_cone_glow::ConfigureBeamLight(light, lamps[lampIdx]);
			childPos = lamps[lampIdx].apex;
		}
		else
			light->SetFalloffDistances(50.0f, 250.0f);
		light->SetIntensity(aimed ? kLampIntensity : 0.15f);
		return aimed;
	}
};

} // namespace mc2_mover_lamps
