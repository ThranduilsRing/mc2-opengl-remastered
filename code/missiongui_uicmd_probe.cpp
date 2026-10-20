// MissionInterfaceManager: RENDERER-INPUT-READBACK-1 verbs (light_row, draw_path, shadow_feed, ground, pixel).
// Declared in missiongui.h; shared names in missiongui_internal.h.
#include "missiongui_internal.h"
#include "mc2fmt.h"

// RENDERER-INPUT-READBACK-1 verbs (light_row, draw_path, shadow_feed, ground, pixel). Returns true when a verb of this group matched.
bool MissionInterfaceManager::uiCmdProbe(UiCmdCtx& c)
{
	const std::string& line = c.line;
	const char*& result = c.result;
	std::string& note = c.note;
	std::string& extra = c.extra;

	//---------------------------------------------------------------------
	// RENDERER-INPUT-READBACK-1. Five verbs, one purpose: make a
	// wrong-renderer-INPUT defect assertable without grading whole-frame
	// pixels. Everything above this block either drives the world or reads
	// back GAME state (position, hp, gesture); nothing could read back what
	// the RENDERER was handed -- not the light row a draw used, not the
	// pose the shadow pass submitted, not the ground under an object. So
	// every such defect was being graded by mean_luma over the whole frame,
	// the instrument class that produced the retracted +14.8-luma result.
	//
	// All five are READ-ONLY and therefore ungated beyond
	// MC2_UI_COMMAND_FILE itself. That is a deliberate choice, not an
	// oversight: a second env gate in front of a read-only verb behind an
	// already-gated channel buys no safety and adds one more
	// documented-vs-actual default that can drift out of truth.
	if (line.compare(0, 10, "light_row ") == 0) {
		// light_row <partId> -- the per-object light row, produced by the
		// SAME GatherLightsParameters() the draw path calls, fed from the
		// SAME list this object's appearance class passes to SetLightList.
		//
		// Class -> list mapping, read off the SetLightList call sites (NOT
		// from docs -- these are the only five in the tree):
		//   BUILDING_APPR_TYPE  mclib/bdactor.cpp  worldLights
		//   TREE_APPR_TYPE      mclib/bdactor.cpp  worldLights
		//   VEHICLE_APPR_TYPE   mclib/gvactor.cpp         worldLights
		//   MECH_APPR_TYPE      mclib/mech3d.cpp          worldLights
		//   GENERIC_APPR_TYPE   mclib/genactor.cpp        NULL, 0
		// That last row is not a typo and not a bug report: GenericAppearance
		// really does hand the gather a null list, so its objects gather
		// ZERO lights on this path. Any theory about a whole object class
		// looking unlit should be checked against `feed=` first.
		const std::string arg = line.substr(10);
		char* end = nullptr;
		const long pid = strtol(arg.c_str(), &end, 10);
		if (arg.empty() || !end || *end != '\0') {
			note = "light_row <partId>  (ids from `list` / `list buildings`)";
		} else if (!eye) {
			result = "blocked"; note = "no camera (world light list lives on it)";
		} else {
			GameObjectPtr obj = ObjectManager ? ObjectManager->findByPartId(pid) : nullptr;
			AppearancePtr ap = obj ? obj->getAppearance() : nullptr;
			if (!obj) {
				result = "blocked"; note = "no such object";
			} else if (!ap) {
				result = "blocked"; note = "object has no appearance -- it feeds the renderer nothing";
			} else {
				const unsigned long cls = ap->getAppearanceClass();
				const bool nullFeed = (cls == GENERIC_APPR_TYPE);
				TG_LightPtr* srcList = nullFeed ? nullptr : eye->getWorldLights();
				const unsigned long srcCount = nullFeed ? 0UL : static_cast<unsigned long>(eye->getNumLights());

				static const char* kClsName[] = {
					"building", "tree", "vehicle", "mech", "generic" };
				const char* clsName = (cls >= BUILDING_APPR_TYPE && cls <= GENERIC_APPR_TYPE)
					? kClsName[cls - BUILDING_APPR_TYPE] : "other";

				TG_HWLightsData rows;
				ProbeGatherLightsForList(srcList, srcCount, &rows);

				char b[512];
				mc2::fmt::to_buf(b, "  src: class={}(0x{:02x}) feed={} srcListCount={:d} gathered={:d}\n", mc2::fmt::safe(clsName), mc2::fmt::as_u32(cls), mc2::fmt::safe(nullFeed ? "NULL" : "worldLights"), mc2::fmt::as_u32(srcCount), mc2::fmt::as_i32(rows.numLights_));
				extra += b;

				// Per slot: BOTH the translation the gather READ (floats
				// 3/7/11, where BuildTranslation leaves it) and the one it
				// UPLOADED (floats 12/13/14, the only ones any shader reads
				// as m[3].xyz). atOrigin=1 means the uploaded origin is
				// (0,0,0) while the source was not -- i.e. this light is
				// evaluated at the world origin no matter where it sits.
				// That is the whole "lamps at world origin" question,
				// answered per light instead of by an aggregate percentage.
				static const char* kType[6] = {
					"AMBIENT", "INFINITE", "INFINITEWITHFALLOFF",
					"POINT", "SPOT", "TERRAIN" };
				long atOrigin = 0;
				for (int li = 0; li < rows.numLights_ && li < MAX_HW_LIGHTS_IN_WORLD; ++li) {
					const float* m = rows.lightToWorld[li];
					const int ty = static_cast<int>(rows.lightDir[li][3]);
					const bool srcNonZero =
						(m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f);
					const bool upZero =
						(m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f);
					const int origin = (srcNonZero && upZero) ? 1 : 0;
					atOrigin += origin;
					mc2::fmt::to_buf(b, "  [{:d}] type={:d}({}) up=({:.2f},{:.2f},{:.2f}) src=({:.2f},{:.2f},{:.2f}) atOrigin={:d} dir=({:.4f},{:.4f},{:.4f}) color=({:.4f},{:.4f},{:.4f}) falloff=({:.2f},{:.2f},{:.6f})\n", mc2::fmt::as_i32(li), mc2::fmt::as_i32(ty), mc2::fmt::safe((ty >= 0 && ty <= 5) ? kType[ty] : "?"), m[12], m[13], m[14], m[3], m[7], m[11], mc2::fmt::as_i32(origin), rows.lightDir[li][0], rows.lightDir[li][1], rows.lightDir[li][2], rows.lightColor[li][0], rows.lightColor[li][1], rows.lightColor[li][2], rows.lightFalloff[li][0], rows.lightFalloff[li][1], rows.lightFalloff[li][2]);
					extra += b;
				}

				// For a static-registered prop the row ABOVE is what the
				// gather would produce NOW; the row BELOW is the one that
				// was actually uploaded and is still being replayed. A
				// difference between them is a stale bake, and it is the
				// only way to see one.
				const int32_t recipe = ap->getStaticRecipeIndex();
				uint32_t ldi = 0xFFFFFFFFu;
				if (recipe >= 0 &&
					GameAdapters::StaticProp::recipeLightDataIndex(recipe, &ldi) &&
					mc2::textures::ready()) {
					TG_HWLightsData baked;
					if (mc2::vkLights::copyLightSlot(ldi, baked)) {
						mc2::fmt::to_buf(b, "  baked: recipe={:d} lightDataIndex={:d} slots={:d} numLights={:d} firstType={:d} firstColor=({:.4f},{:.4f},{:.4f})\n", mc2::fmt::as_i32(static_cast<int>(recipe)), mc2::fmt::as_u32(ldi), mc2::fmt::as_u32(mc2::vkLights::getLightStructCount()), mc2::fmt::as_i32(baked.numLights_), mc2::fmt::as_i32(baked.numLights_ > 0 ? static_cast<int>(baked.lightDir[0][3]) : -1), baked.numLights_ > 0 ? baked.lightColor[0][0] : 0.0f, baked.numLights_ > 0 ? baked.lightColor[0][1] : 0.0f, baked.numLights_ > 0 ? baked.lightColor[0][2] : 0.0f);
					} else {
						mc2::fmt::to_buf(b, "  baked: recipe={:d} lightDataIndex={:d} UNREADABLE (index beyond allocated slot count {:d})\n", mc2::fmt::as_i32(static_cast<int>(recipe)), mc2::fmt::as_u32(ldi), mc2::fmt::as_u32(mc2::vkLights::getLightStructCount()));
					}
					extra += b;
				} else if (recipe >= 0) {
					mc2::fmt::to_buf(b, "  baked: recipe={:d} lightDataIndex=<none> (registry off, or recipe tombstoned)\n", mc2::fmt::as_i32(static_cast<int>(recipe)));
					extra += b;
				} else {
					extra += "  baked: <none> (object is not on the static recipe path)\n";
				}

				result = "ok";
				mc2::fmt::to_buf(b, "{} feed={} gathered={:d} atOrigin={:d}", mc2::fmt::safe(clsName), mc2::fmt::safe(nullFeed ? "NULL" : "worldLights"), mc2::fmt::as_i32(rows.numLights_), mc2::fmt::as_i32(atOrigin));
				note = b;
			}
		}
	}
	else if (line.compare(0, 10, "draw_path ") == 0) {
		// draw_path <partId> -- which renderer lane this object is on, and
		// for a static prop, whether the recipe still describes it.
		//
		// `unit <partId>` already prints inView + staticRecipe. What it
		// CANNOT say is whether that recipe is STALE: the recipe carries a
		// baked model matrix, and the batcher replays that matrix, not the
		// object's live transform. recipeDrift below is the distance
		// between the two. A non-zero drift on a live object means every
		// pixel and every shadow texel for it is being placed from an old
		// pose -- the sunk-turret and black-quad hypotheses both live
		// exactly here, and both currently rest on source reading alone.
		const std::string arg = line.substr(10);
		char* end = nullptr;
		const long pid = strtol(arg.c_str(), &end, 10);
		if (arg.empty() || !end || *end != '\0') {
			note = "draw_path <partId>";
		} else {
			GameObjectPtr obj = ObjectManager ? ObjectManager->findByPartId(pid) : nullptr;
			AppearancePtr ap = obj ? obj->getAppearance() : nullptr;
			if (!obj) {
				result = "blocked"; note = "no such object";
			} else if (!ap) {
				result = "blocked"; note = "object has no appearance";
			} else {
				char b[512];
				const unsigned long cls = ap->getAppearanceClass();
				const int32_t recipe = ap->getStaticRecipeIndex();
				AppearanceTypePtr at = ap->getAppearanceType();
				glm::vec3 p = obj->getPosition();
				Stuff::Vector4D sp = ap->getScreenPos();

				// Verdict vocabulary, derived from the three mutually
				// exclusive submission routes:
				//   recipe_replay  -- registered AND still flagged static:
				//                     the GPU batcher replays a baked
				//                     instance; the per-frame walk is skipped
				//   dynamic_submit -- has a recipe slot but is NOT static
				//                     now (animating / damaged / falling):
				//                     it is re-submitted every frame
				//   legacy_cpu     -- no recipe at all: the CPU TG path
				const bool reg = ap->isStaticRegistered();
				const bool statNow = ap->IsStaticNow();
				const char* verdict = (recipe >= 0 && reg && statNow) ? "recipe_replay"
									: (recipe >= 0)                  ? "dynamic_submit"
									:                                  "legacy_cpu";

				mc2::fmt::to_buf(b, "  path: verdict={} class=0x{:02x} type={} appearNum={:d} inView={:d} staticReg={:d} staticNow={:d} recipe={:d}\n", mc2::fmt::safe(verdict), mc2::fmt::as_u32(cls), mc2::fmt::safe((at && at->name) ? at->name : "?"), mc2::fmt::as_u32(at ? at->appearanceNum : 0UL), mc2::fmt::as_i32(ap->canBeSeen() ? 1 : 0), mc2::fmt::as_i32(reg ? 1 : 0), mc2::fmt::as_i32(statNow ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(recipe)));
				extra += b;
				mc2::fmt::to_buf(b, "  pose: obj=({:.3f},{:.3f},{:.3f}) screen=({:.1f},{:.1f},{:.3f},{:.3f})\n", p.x, p.y, p.z, sp.x, sp.y, sp.z, sp.w);
				extra += b;

				if (recipe >= 0) {
					float mm[16];
					char shapeName[128] = {0};
					GameAdapters::StaticProp::getRecipeShapeName(
						recipe, shapeName, sizeof(shapeName));
					if (GameAdapters::StaticProp::recipeModelMatrix(recipe, mm)) {
						// Registry translation indices are [3]/[7]/[11]
						// (gos_static_prop_registry.h), in the same
						// Stuff space as GameObject::getPosition().
						const float dx = mm[3]  - p.x;
						const float dy = mm[7]  - p.y;
						const float dz = mm[11] - p.z;
						const float drift = sqrtf(dx*dx + dy*dy + dz*dz);
						mc2::fmt::to_buf(b, "  recipe: shape={} baked=({:.3f},{:.3f},{:.3f}) recipeDrift={:.4f}\n", mc2::fmt::safe(shapeName[0] ? shapeName : "?"), mm[3], mm[7], mm[11], drift);
					} else {
						mc2::fmt::to_buf(b, "  recipe: shape={} baked=<unreadable> recipeDrift=n/a (tombstoned or registry off)\n", mc2::fmt::safe(shapeName[0] ? shapeName : "?"));
					}
					extra += b;

					uint32_t tid = 0xFFFFFFFFu, flags = 0;
					bool hadCull = false;
					const bool okT = GameAdapters::StaticProp::recipeTypeId(recipe, &tid);
					const bool okF = GameAdapters::StaticProp::recipeInstanceFlags(recipe, &flags);
					const bool okC = GameAdapters::StaticProp::recipeHadCullRecord(recipe, &hadCull);
					// Separate buffer, not shapeName: an earlier draft
					// formatted the typeID into shapeName via a comma
					// operator inside this argument list, which clobbered
					// the shape name mid-statement. It happened to be
					// harmless (the name had already been printed) and it
					// is exactly the kind of cleverness that turns an
					// instrument into a liar after one careless edit.
					char tidBuf[24] = "?";
					if (okT) mc2::fmt::to_buf(tidBuf, "{:d}", mc2::fmt::as_u32(tid));
					// hadCullRecord is the PREVIOUS frame's submit receipt
					// (ExtractRenderSnapshot runs before flush), so a fresh
					// 0 on frame N does not by itself mean "never drawn".
					mc2::fmt::to_buf(b, "  submit: typeID={} flags=0x{:02x}(lightsOut={:d} window={:d} spotlight={:d}) cullRecordPrevFrame={}\n", mc2::fmt::safe(tidBuf), mc2::fmt::as_u32(okF ? flags : 0u), mc2::fmt::as_i32(okF ? static_cast<int>((flags & 1u)) : -1), mc2::fmt::as_i32(okF ? static_cast<int>(((flags >> 1) & 1u)) : -1), mc2::fmt::as_i32(okF ? static_cast<int>(((flags >> 2) & 1u)) : -1), mc2::fmt::safe(okC ? (hadCull ? "1" : "0") : "?"));
					extra += b;
				}
				result = "ok";
				mc2::fmt::to_buf(b, "{} recipe={:d}", mc2::fmt::safe(verdict), mc2::fmt::as_i32(static_cast<int>(recipe)));
				note = b;
			}
		}
	}
	else if (line.compare(0, 12, "shadow_feed ") == 0) {
		// shadow_feed <partId> -- what the shadow passes hold for this
		// object, and from what pose.
		//
		// The leading black-quad hypothesis is that an animating building
		// casts from its frozen mission-load pose. That is checkable here
		// and nowhere else: the static building shadow map is built ONCE
		// per mission (mclib/txmmgr.cpp) from the registry's
		// BAKED matrices, and the per-frame dynamic prop caster feed
		// (:3283) reads those same baked matrices. Neither reads the live
		// transform. shadowDrift = |submitted - live| is the assert.
		const std::string arg = line.substr(12);
		char* end = nullptr;
		const long pid = strtol(arg.c_str(), &end, 10);
		if (arg.empty() || !end || *end != '\0') {
			note = "shadow_feed <partId>";
		} else {
			GameObjectPtr obj = ObjectManager ? ObjectManager->findByPartId(pid) : nullptr;
			AppearancePtr ap = obj ? obj->getAppearance() : nullptr;
			if (!obj) {
				result = "blocked"; note = "no such object";
			} else if (!ap) {
				result = "blocked"; note = "object has no appearance";
			} else {
				const int32_t recipe = ap->getStaticRecipeIndex();
				gos_render_probe::ShadowFeedReport rep;
				gos_render_probe::queryShadowFeed(recipe, rep);
				glm::vec3 p = obj->getPosition();
				char b[448];

				mc2::fmt::to_buf(b, "  feed: registryQueried={:d} recipe={:d} recipeResolved={:d} buildingFeed={:d} dynamicFeed={:d} modernShadowPath={:d}\n", mc2::fmt::as_i32(rep.registryQueried ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(recipe)), mc2::fmt::as_i32(rep.recipeResolved ? 1 : 0), mc2::fmt::as_i32(rep.buildingFeedTotal), mc2::fmt::as_i32(rep.dynamicFeedTotal), mc2::fmt::as_i32(rep.modernShadowPathActive ? 1 : 0));
				extra += b;

				// gos_GetStaticBuildingShadowCounts is documented zero
				// unless the static building shadow pass actually DREW this
				// mission. Membership in a feed that never drew is not a
				// shadow, so printing membership without this would be a
				// lying instrument.
				mc2::fmt::to_buf(b, "  staticBldgPass: drew={:d} types={:d} instances={:d} draws={:d}\n", mc2::fmt::as_i32((rep.bldgDrawInst > 0 || rep.bldgDrawCalls > 0) ? 1 : 0), mc2::fmt::as_i32(rep.bldgDrawTypes), mc2::fmt::as_i32(rep.bldgDrawInst), mc2::fmt::as_i32(rep.bldgDrawCalls));
				extra += b;

				const gos_render_probe::ShadowFeedHit* hits[2] =
					{ &rep.inBuildingFeed, &rep.inDynamicFeed };
				static const char* kFeedName[2] = { "building", "dynamic" };
				for (int fi = 0; fi < 2; ++fi) {
					const gos_render_probe::ShadowFeedHit& h = *hits[fi];
					// Drift ONLY when an exact leaf was found. With
					// sameTypeLeaves > 0 but exactLeaves == 0 the feed
					// carries OTHER instances of the same building type,
					// and differencing against one of those would emit a
					// large meaningless number that reads exactly like the
					// stale-pose signal this verb exists to detect.
					// -1 means "not computable", never "zero".
					float drift = -1.0f;
					if (h.leavesExact > 0) {
						const float dx = h.submittedPos[0] - p.x;
						const float dy = h.submittedPos[1] - p.y;
						const float dz = h.submittedPos[2] - p.z;
						drift = sqrtf(dx*dx + dy*dy + dz*dz);
					}
					mc2::fmt::to_buf(b, "  {}: present={:d} sameTypeLeaves={:d} exactLeaves={:d} submitted=({:.3f},{:.3f},{:.3f}) live=({:.3f},{:.3f},{:.3f}) shadowDrift={:.4f}\n", mc2::fmt::safe(kFeedName[fi]), mc2::fmt::as_i32(h.found ? 1 : 0), mc2::fmt::as_i32(h.leavesTotal), mc2::fmt::as_i32(h.leavesExact), h.submittedPos[0], h.submittedPos[1], h.submittedPos[2], p.x, p.y, p.z, drift);
					extra += b;
				}
				result = "ok";
				mc2::fmt::to_buf(b, "bldg={:d} dyn={:d} staticPassDrew={:d}", mc2::fmt::as_i32(rep.inBuildingFeed.found ? 1 : 0), mc2::fmt::as_i32(rep.inDynamicFeed.found ? 1 : 0), mc2::fmt::as_i32((rep.bldgDrawInst > 0 || rep.bldgDrawCalls > 0) ? 1 : 0));
				note = b;
			}
		}
	}
	else if (line.compare(0, 7, "ground ") == 0) {
		// ground <partId> | ground <x> <y>
		//
		// The objective form of "the turret is sunk". Reports the terrain
		// surface under a point next to the thing standing on it, and --
		// for a static prop -- next to the elevation the BAKED recipe
		// matrix uses, which is what actually places the pixels. Three
		// numbers, three different ways to be wrong:
		//   objAboveGround    live sim position vs terrain
		//   recipeAboveGround baked render position vs terrain
		//   recipeVsObj       render pose vs sim pose (0 when in sync)
		const std::string arg = line.substr(7);
		if (!land) {
			result = "blocked"; note = "no terrain loaded";
		} else {
			glm::vec3 q; q.x = 0; q.y = 0; q.z = 0;
			GameObjectPtr obj = nullptr;
			bool haveQuery = false;
			float fx = 0, fy = 0;
			char* end = nullptr;
			const long pid = strtol(arg.c_str(), &end, 10);
			if (!arg.empty() && end && *end == '\0') {
				obj = ObjectManager ? ObjectManager->findByPartId(pid) : nullptr;
				if (!obj) { result = "blocked"; note = "no such object"; }
				else { q = obj->getPosition(); haveQuery = true; }
			} else if (sscanf(arg.c_str(), "%f %f", &fx, &fy) == 2) {
				q.x = fx; q.y = fy; haveQuery = true;
			} else {
				note = "ground <partId> | ground <x> <y>";
			}

			if (haveQuery) {
				char b[448];
				const float terr  = land->getTerrainElevation(q);
				const float water = land->getWaterElevation();
				const short ttype = land->getTerrainType(q);
				glm::vec3 n = land->getTerrainNormal(q);
				mc2::fmt::to_buf(b, "  at=({:.3f},{:.3f}) terrainZ={:.4f} waterZ={:.4f} terrainType={:d} normal=({:.4f},{:.4f},{:.4f}) terrainLight={:.4f}\n", q.x, q.y, terr, water, mc2::fmt::as_i32(static_cast<int>(ttype)), n.x, n.y, n.z, land->getTerrainLight(q));
				extra += b;
				if (obj) {
					mc2::fmt::to_buf(b, "  obj: id={:d} z={:.4f} objAboveGround={:.4f}\n", mc2::fmt::as_i32(obj->getPartId()), q.z, q.z - terr);
					extra += b;
					AppearancePtr ap = obj->getAppearance();
					const int32_t recipe = ap ? ap->getStaticRecipeIndex() : -1;
					float mm[16];
					if (recipe >= 0 &&
						GameAdapters::StaticProp::recipeModelMatrix(recipe, mm)) {
						glm::vec3 rq; rq.x = mm[3]; rq.y = mm[7]; rq.z = mm[11];
						const float rterr = land->getTerrainElevation(rq);
						mc2::fmt::to_buf(b, "  recipe: idx={:d} baked=({:.3f},{:.3f},{:.3f}) recipeTerrainZ={:.4f} recipeAboveGround={:.4f} recipeVsObj={:.4f}\n", mc2::fmt::as_i32(static_cast<int>(recipe)), mm[3], mm[7], mm[11], rterr, mm[11] - rterr, mm[11] - q.z);
						extra += b;
					}
				}
				result = "ok";
				mc2::fmt::to_buf(b, "terrainZ={:.4f}{}", terr, mc2::fmt::safe(obj ? "" : " (point query; no object)"));
				note = b;
			}
		}
	}
	else if (line.compare(0, 6, "pixel ") == 0) {
		// pixel <x> <y> [z] | pixel screen <px> <py>
		//
		// Samples ONE texel of the scene colour attachment, either at a
		// world point (projected with the same Camera the renderer uses) or
		// at a raw FBO texel. Turns "there is a black quad there" from an
		// eyeball verdict into a number.
		//
		// TWO PROVENANCE FACTS, BOTH PRINTED, BOTH LOAD-BEARING:
		//  1. srcFrame is CURRENT-1. This channel is polled from
		//     MissionInterfaceManager::update(), which runs before this
		//     frame renders, so the native scene image still holds the previous
		//     frame. A `screenshot` scheduled on the same line is an
		//     END-of-frame capture and is one frame AHEAD of this sample.
		//     Comparing the two without accounting for that is the most
		//     likely way this verb could lie.
		//  2. Camera::projectForScreenXY returns Y-DOWN screen coordinates
		//     (mclib/camera.h, the D3D pixel-homogeneous convention
		//     with viewMulY/viewAddY), while glReadPixels is Y-UP. The flip
		//     happens here and both coordinates are printed so a
		//     disagreement with a TGA is diagnosable rather than mysterious.
		const std::string arg = line.substr(6);
		int wantPx = -1, wantPy = -1;
		bool haveTarget = false, behind = false, offScreen = false;
		float wx = 0, wy = 0, wz = 0, sxOut = 0, syOut = 0, swOut = 0;
		bool worldForm = false;
		char b[448];

		if (arg.compare(0, 7, "screen ") == 0) {
			if (sscanf(arg.c_str() + 7, "%d %d", &wantPx, &wantPy) == 2)
				haveTarget = true;
			else
				note = "pixel screen <px> <py>  (FBO texels, origin bottom-left)";
		} else {
			const int nf = sscanf(arg.c_str(), "%f %f %f", &wx, &wy, &wz);
			if (nf < 2) {
				note = "pixel <x> <y> [z] | pixel screen <px> <py>";
			} else if (!eye) {
				result = "blocked"; note = "no camera";
			} else {
				worldForm = true;
				glm::vec3 wp; wp.x = wx; wp.y = wy;
				wp.z = (nf >= 3) ? wz
					 : (land ? land->getTerrainElevation(wp) : 0.0f);
				wz = wp.z;
				Stuff::Vector4D screen;
				// projectForScreenXY is the intent-specific wrapper for
				// "I want the screen position", per the projectZ policy
				// split; its bool is the legacy screen-RECT admission test,
				// which is exactly the on-screen predicate wanted here.
				const bool onScreen = eye->projectForScreenXY(wp, screen);
				sxOut = screen.x; syOut = screen.y; swOut = screen.w;
				behind = (screen.w <= 0.0f);
				offScreen = !onScreen;
				if (onScreen) {
					// Normalise out of screenResolution space, then into
					// FBO texels, then flip Y. The FBO is not required to
					// be the same size as the window (free resize / render
					// scale), so this cannot assume 1:1.
					//
					// getScreenRes() is the public accessor; the field
					// itself is protected. This is the SAME value
					// projectZ's screen-rect admission test compares
					// against (camera.h), so normalising by it and
					// then scaling to the FBO cannot disagree with the
					// on-screen verdict above.
					int fw = 0, fh = 0;
					const glm::vec3 sres = eye->getScreenRes();
					if (gos_render_probe::sceneImageSize(&fw, &fh) &&
						sres.x > 0.0f && sres.y > 0.0f) {
						const float nx = screen.x / sres.x;
						const float ny = screen.y / sres.y;
						wantPx = static_cast<int>((nx * static_cast<float>(fw)));
						wantPy = fh - 1 - static_cast<int>((ny * static_cast<float>(fh)));
						haveTarget = true;
					} else {
						result = "blocked";
						note = "native scene image unavailable (post-process not up?)";
					}
				}
			}
		}

		// Emit the projection line whenever the world form was parsed,
		// INCLUDING the off-screen / behind-camera cases. Those are the
		// cases where knowing where the point landed matters most -- an
		// earlier draft printed nothing but "off-screen", which tells a
		// driver that its probe failed but not whether the camera or the
		// coordinate was wrong.
		if (worldForm) {
			mc2::fmt::to_buf(b, "  world=({:.3f},{:.3f},{:.3f}) screenYDown=({:.1f},{:.1f}) w={:.4f} behindCamera={:d} offScreen={:d} screenRes=({:.0f},{:.0f})\n", wx, wy, wz, sxOut, syOut, swOut, mc2::fmt::as_i32(behind ? 1 : 0), mc2::fmt::as_i32(offScreen ? 1 : 0), eye ? eye->getScreenRes().x : 0.0f, eye ? eye->getScreenRes().y : 0.0f);
			extra += b;
		}
		if (haveTarget) {
			gos_render_probe::PixelProbe pp;
			gos_render_probe::readScenePixel(wantPx, wantPy, pp);
			mc2::fmt::to_buf(b, "  scene={:d}x{:d} texelYUp=({:d},{:d}) inBounds={:d} rgba=({:d},{:d},{:d},{:d}) luma={:.3f} srcFrame={:d}\n", mc2::fmt::as_i32(pp.sceneWidth), mc2::fmt::as_i32(pp.sceneHeight), mc2::fmt::as_i32(pp.px), mc2::fmt::as_i32(pp.py), mc2::fmt::as_i32(pp.inBounds ? 1 : 0), mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[0])), mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[1])), mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[2])), mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[3])), (pp.rgba[0] + pp.rgba[1] + pp.rgba[2]) / 3.0, mc2::fmt::as_u32(pp.srcFrameHint));
			extra += b;
			if (pp.inBounds) {
				result = "ok";
				mc2::fmt::to_buf(b, "rgb={:d},{:d},{:d} srcFrame={:d}", mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[0])), mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[1])), mc2::fmt::as_u32(static_cast<unsigned>(pp.rgba[2])), mc2::fmt::as_u32(pp.srcFrameHint));
				note = b;
			} else {
				result = "blocked";
				note = pp.sceneAvailable ? "texel outside the native scene image"
									   : "native scene image unavailable";
			}
		} else if (worldForm && strcmp(result, "blocked") != 0 && note.empty()) {
			result = "blocked";
			note = behind ? "world point is behind the camera"
						  : "world point projects off-screen";
		}
	}
	else
		return false;
	return true;
}
