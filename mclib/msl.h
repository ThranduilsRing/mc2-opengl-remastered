//-------------------------------------------------------------------------------
//
// Multiple TG Shape Layer
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
// Replace MLR for a myriad number of reasons.
//
// Started 4/16/99
//
// FFS
//
//-------------------------------------------------------------------------------

#ifndef MSL_H
#define MSL_H
#include "../RenderCore/MaterialRegistry.h"
#include "content_socket.h"
//-------------------------------------------------------------------------------
// Include Files

#ifndef TGL_H
#include"tgl.h"
#endif

#ifndef MC2_RENDER_SUBMISSIONS_H
#include"render_submissions.h"
#endif

#ifdef LINUX_BUILD
#include "platform_windows.h"
#include "platform_str.h"
#include "platform_stdlib.h"
#endif

// ARM
namespace Microsoft
{
	namespace Xna
	{
		namespace Arm
		{
			struct IProviderEngine;
		}
	}
}

//-------------------------------------------------------------------------------
// Structs used by layer.
//

class TG_MultiShape;
class TG_AnimateShape;
namespace mc2::content { struct Asset; }
//-------------------------------------------------------------------------------
// The meat and Potatoes part.
// TG_TypeMultiShape
class TG_TypeMultiShape
{
	friend class TG_MultiShape;
	friend class TG_TypeShape;
	friend class TG_Shape;
	friend class TG_AnimateShape;
	
	//---------------------------------------------------------
	// This class runs a multi-shape appearance.  Anything
	// with an ASE file of more then one shape should use this
	// Class.  Uses everything from TG_Shape except parse.
	// That is local to here to get all of the pieces.  Each
	// Shape is transformed, rendered etc. in TG_Shape.
	// This will store animation information and use it, too!

	//-------------
	//Data Members
	protected:
		long DecodeStockBinary(std::string_view fileName);
		long DecodeStockASE(std::string_view fileName, bool forceMakeBinary, Microsoft::Xna::Arm::IProviderEngine* armProvider);
		mc2::content::Asset ExtractContentIR();
		int                 numTG_TypeShapes;		//Number of TG_Shapes
		int                 numTextures;			//Total Textures for all shapes.
		TG_TypeNodePtr		*listOfTypeShapes;		//Memory holding all TG_TypeNodes and TypeShapes
		TG_TexturePtr		listOfTextures;			//List of texture Structures for all shapes.

		// The two lists above are views: listOfTypeShapes is typeNodeView.data() (a raw T** view the consumers index), the nodes are OWNED
		// by typeNodeStore (B7o.6b), index-parallel; bindTypeNodes() sizes both, adoptTypeNode() fills one slot of both, destroy() releases.
		std::vector<std::unique_ptr<TG_TypeNode>>	typeNodeStore;
		std::vector<TG_TypeNodePtr>	typeNodeView;
		std::vector<TG_Texture>		textureStore;
        std::vector<mc2::content::Socket> contentSockets_;
        std::vector<uint8_t> materialDefaultsDefined_; // first source establishes defaults; instances never overwrite them

		// The one (re)allocation point of the node arrays (binary load, imported mesh, ASE): count empty owners and a null view.
		void bindTypeNodes (long count)
		{
			std::vector<std::unique_ptr<TG_TypeNode>>(count).swap(typeNodeStore);
			typeNodeView.assign(count, nullptr);
			listOfTypeShapes = typeNodeView.data();
		}
		// Slot i takes ownership of node; returns the raw view now stored in listOfTypeShapes[i].
		TG_TypeNodePtr adoptTypeNode (long i, std::unique_ptr<TG_TypeNode> node)
		{
			node->stableOwnerType_ = stableId();
            node->stableOwnerIndex_ = static_cast<uint32_t>(i);
            typeNodeStore[i] = std::move(node);
			typeNodeView[i] = typeNodeStore[i].get();
			return typeNodeView[i];
		}

#ifdef _DEBUG
		char				*shapeName;				//FileName of shape
#endif
		
	public:			
		glm::vec3		maxBox;					//Bounding Box Max Coords
		glm::vec3		minBox;					//Bounding Box Min Coords
		float				extentRadius;			//Bounding Sphere Coords
		
	//-----------------
	//Member Functions
	protected:

	public:
		void init (void)
		{
            contentSockets_.clear();
			numTG_TypeShapes = 0;
            materialDefaultsDefined_.clear();
			listOfTypeShapes = nullptr;

			numTextures = 0;
			listOfTextures = nullptr;

#ifdef _DEBUG
			shapeName = nullptr;
#endif
			maxBox.x =
			maxBox.y =
			maxBox.z = -100000.0f;
			
			minBox.x =
			minBox.y =
			minBox.z = 100000.0f;
			
			extentRadius = 0.0f;
		}
		
		TglIdentity<TG_TypeMultiShape> stableIdentity_{this};
        TG_TypeId stableId() const { return stableIdentity_.id(); }

        TG_TypeMultiShape (void)
		{
			init();
		}

		void destroy (void);

		~TG_TypeMultiShape (void)
		{
			destroy();
		}

		//Function returns 0 if OK.  -1 if file not found or file not ASE Format.
		//Function runs through each piece of ASE file and creates a separate
		//TG_Shape for each one.  First pass is count number of GEOMOBJECTS.
		//Second pass is load each one.
		long LoadTGMultiShapeFromASE (const char *fileName, bool forceMakeBinary = false, Microsoft::Xna::Arm::IProviderEngine * armProvider = nullptr);

		//Function returns 0 if OK.  -1 if textureName is longer then nameLength-1.
		//This function digs the texture name(s) out of the ASE file so that the
		//User can load and manage them anyway they want to.
		long GetTextureName (DWORD textureNum, char *textureName, long nameLength);

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
		long SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle);

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
		long SetTextureAlpha (DWORD textureNum, bool alphaFlag);

		DWORD GetTextureHandle (DWORD textureNum)
		{
			if (textureNum < numTextures)
				return listOfTextures[textureNum].mcTextureNodeIndex;

			return 0xffffffff;
		}

		long GetNumShapes (void)
		{
			return numTG_TypeShapes;
		}

		// Public accessor for the GPU static-prop batcher registration walk.
		// Returns NULL if shapeNum is out of range or listOfTypeShapes is NULL.
		TG_TypeNodePtr GetTypeNode (long shapeNum)
		{
			if (listOfTypeShapes && shapeNum >= 0 && shapeNum < numTG_TypeShapes)
				return listOfTypeShapes[shapeNum];
			return nullptr;
		}

		long GetNumTextures (void)
		{
			return numTextures;
		}

		char *GetNodeId (long shapeNum)
		{
			if ((shapeNum >= 0) && (shapeNum < numTG_TypeShapes))
				return (listOfTypeShapes[shapeNum]->getNodeId());
			
			return nullptr;
		}

		glm::vec3 GetNodeCenter (long shapeNum)
		{
			glm::vec3 result;
			result.x = result.y = result.z = 0.0f;

			if ((shapeNum >= 0) && (shapeNum < numTG_TypeShapes))
				result = listOfTypeShapes[shapeNum]->GetNodeCenter();
			
			return result;
		}

		glm::vec3 GetNodeCenter (char * nodeId)
		{
			glm::vec3 result;
			result.x = result.y = result.z = 0.0f;

			for (int i=0;i<numTG_TypeShapes;i++)
			{
				if (S_stricmp(listOfTypeShapes[i]->getNodeId(),nodeId) == 0)
					result = listOfTypeShapes[i]->GetNodeCenter();
			}
			
			return result;
		}

		//This function creates an instance of the TG_MultiShape from the Type
		TG_MultiShape *CreateFrom (void);

		void SetAlphaTest (bool flag)
		{
			for (int i=0;i<numTG_TypeShapes;i++)
				listOfTypeShapes[i]->SetAlphaTest(flag);
		}

		void SetFilter (bool flag)
		{
			for (int i=0;i<numTG_TypeShapes;i++)
				listOfTypeShapes[i]->SetFilter(flag);
		}
		
		void SetLightRGBs (DWORD hPink, DWORD hGreen, DWORD hYellow)
		{
			for (int i=0;i<numTG_TypeShapes;i++)
				listOfTypeShapes[i]->SetLightRGBs(hPink,hGreen,hYellow);
		}

		long LoadBinaryCopy (const char *fileName);
		long BuildContentIR(mc2::content::Asset&& asset, std::string_view source, float* groundDy = nullptr);
        const std::vector<mc2::content::Socket>& ContentSockets() const { return contentSockets_; }
		void SaveBinaryCopy (const char *fileName);

		// Track D — narrow construction API for format-agnostic importers
		// (FBX/GLB via Assimp). Each method is the import-side analog of a
		// portion of LoadTGMultiShapeFromASE / LoadBinaryCopy:
		//
		//   AllocateImportedShapes — allocates listOfTypeShapes[] and
		//     constructs `numShapes` empty TG_TypeShape instances at slots
		//     0..numShapes-1. Caller populates each via
		//     listOfTypeShapes[i]->InitFromImportedMesh(...).
		//
		//   SetImportedTextures — populates the multi-shape's TG_Texture
		//     array (names + alpha + sentinel handles 0xffffffff per
		//     mc2_texture_handle_is_live.md), then walks listOfTypeShapes
		//     calling CreateListOfTextures on each TG_TypeShape so the
		//     per-shape TG_TinyTexture index linkage is wired identically
		//     to the ASE path.
		//
		// Both methods leave maxBox/minBox/extentRadius for the caller to
		// compute (importer has the vertex iteration anyway). No Assimp
		// types in either signature.
		void AllocateImportedShapes(int numShapes);
		void SetImportedTextures(DWORD count, const char* const* names,
		                         const bool* alphas);

		// Track D — format-probe entry point. Replaces direct
		// LoadTGMultiShapeFromASE call sites. Probes for {tglPath}{baseName}.glb
		// (preferred new format); on hit, calls ImportGeometryFromFile and
		// returns. On miss or failure, falls through to the legacy
		// LoadTGMultiShapeFromASE({tglPath}{baseName}.ase) — stock install
		// must remain playable per memory:stock_install_must_remain_playable.
		//
		// `baseName` is the asset name without extension (e.g. "madcat").
		// MVP scope: .glb probe only. .fbx supported by Assimp but
		// scope-restricted to M2 per advisor verdict D3.
		long LoadFromFile(const char* baseName);
};

using TG_TypeMultiShapePtr = TG_TypeMultiShape*;

//-------------------------------------------------------------------------------
// The meat and Potatoes part.
// TG_MultiShape
class TG_MultiShape
{
	//---------------------------------------------------------
	// This class runs a multi-shape appearance.  Anything
	// with an ASE file of more then one shape should use this
	// Class.  Uses everything from TG_Shape except parse.
	// That is local to here to get all of the pieces.  Each
	// Shape is transformed, rendered etc. in TG_Shape.
	// This will store animation information and use it, too!

	friend class TG_TypeMultiShape;
	friend class TG_Shape;
	friend class TG_TypeShape;
	friend class TG_AnimateShape;
	friend class GpuStaticPropBatcher;
	friend class GpuMechBatcher;
	
	//-------------
	//Data Members
	protected:
        std::vector<TG_TinyTexture> instanceTextures_;
        std::vector<RenderCore::MaterialHandle> instanceMaterials_;
        RenderCore::MaterialSetRef materialSet_;
        void initMaterialSlots();
		TG_TypeMultiShapePtr	myMultiType;			//Pointer to the type
		int                     numTG_Shapes;			//Number of TG_Shapes
		TG_ShapeRecPtr			listOfShapes;			//Memory holding all TG_ShapeRecs; a view of shapeRecStore
		std::vector<TG_ShapeRec>	shapeRecStore;		//Owns the records; parent pointers point into it, so it is sized once per list
		float					frameNum;				//Frame number of animation
		bool					d_useShadows;
		bool					isHudElement;
		BYTE					alphaValue;				//To fade shapes in and out
		bool					isClamped;				//So I can force a shape to clamp its textures

		// Slice 2 (object-offload) — Stage 2.D.2 fix: GPU light-data index
		// cached at update() time while worldLights[0]->aRGB is per-actor-correct.
		// Read by submitMultiShape() during renderLists() instead of calling
		// GatherGpuObjectLightDataOnly() there (at which point worldLights[0]->aRGB
		// has been overwritten by later actors). UINT32_MAX = not yet cached (first
		// frame or non-GPU path); submitMultiShape falls back to gather-now if UINT32_MAX.
		uint32_t				cachedGpuLightIndex_;

		// 2026-05-05: frame-stamp for cull-aware static replay.
		// Set in CacheGpuLightData() / ResubmitCachedGpuLightData() to the
		// current g_mc2FrameCounter value. Registry flush() compares this
		// against the current frame and SKIPS the markVisible draw when stale
		// — this happens when the actor went offscreen, the cull skipped its
		// update(), and its cachedGpuLightIndex_ is now pointing into a slot
		// whose content was filled by a different actor this frame.
		// Sentinel UINT32_MAX = never cached.
		uint32_t				cachedFrame_;

	//-----------------
	//Member Functions
	protected:

	public:
		void init (void)
		{
            instanceTextures_.clear(); instanceMaterials_.clear(); materialSet_={};
			myMultiType = nullptr;
			numTG_Shapes = 0;
			listOfShapes = nullptr;

			frameNum = 0.0f;
			d_useShadows = true;

			isHudElement = false;

			alphaValue = 0xff;

			isClamped = false;

			cachedGpuLightIndex_ = 0xFFFFFFFFu;  // sentinel: not yet cached
			cachedFrame_         = 0xFFFFFFFFu;  // sentinel: never refreshed
		}
		
		TglIdentity<TG_MultiShape> stableIdentity_{this};
        TG_InstanceId stableId() const { return stableIdentity_.id(); }

        TG_MultiShape (void)
		{
			init();
		}

		void destroy (void);

		~TG_MultiShape (void)
		{
			destroy();
		}

		// Stage 3.C: propagate Touch() to all shape-node leaves.
		void Touch();

		// Stage 3.C: expose the per-frame light-data UBO slot index for
		// GpuStaticPropRegistry::flush() to patch into cached recipe copies.
		// CacheGpuLightData() (called from TreeAppearance::render() each frame
		// in the static path) keeps this fresh. Returns UINT32_MAX if
		// CacheGpuLightData() has not yet been called (first frame or non-GPU
		// path) — render() guards against emitting a static instance with
		// UINT32_MAX by falling through to the dynamic submit path.
		uint32_t getCachedGpuLightIndex() const { return cachedGpuLightIndex_; }
		uint32_t getCachedFrame()        const { return cachedFrame_; }
		void     setCachedFrame(uint32_t f)    { cachedFrame_ = f; }

		//This function sets the list of lights used by the TransformShape function
		//to light the shape.
		//Function returns 0 if lightList entries are all OK.  -1 otherwise.
		//
		long SetLightList (TG_LightPtr *lightList, DWORD nLights);

		// Slice 2 (object-offload) — Stage 2.D.2 fix: cache GPU light-data index
		// while worldLights[0]->aRGB is per-actor-correct (during update()).
		// Must be called AFTER SetLightList and BEFORE other actors overwrite
		// worldLights[0]->aRGB. submitMultiShape() reads cachedGpuLightIndex_
		// instead of calling GatherGpuObjectLightDataOnly() at render time.
		void CacheGpuLightData();

		// VEHICLE-LIGHTING-PATH-1: gather a light row into EVERY SHAPE_NODE
		// leaf, not just the first.
		//
		// CacheGpuLightData() above populates only firstShapeNodeLeaf->lightData_
		// because its consumer, GpuStaticPropBatcher::submitMultiShape, BROADCASTS
		// the one returned index into every per-leaf GpuStaticPropInstance. That is
		// correct for the batcher path.
		//
		// It is NOT sufficient for the legacy TG_MultiShape::Render path used by
		// ground vehicles (GVAppearance): that loops every leaf and each
		// TG_Shape::Render independently submits its OWN lightData_
		// (mclib/tgl.cpp:3508). With only the first leaf populated, every other
		// leaf still carries numLights_==0, so calc_light takes its early-return
		// and those leaves render at raw albedo -- the exact defect this lane
		// fixes, just narrowed from "whole vehicle" to "all but one leaf".
		// Measured: with only CacheGpuLightData(), deterministic A/B poses showed
		// ZERO pixel change despite the CPU gather demonstrably running.
		void CacheGpuLightDataAllLeaves();

		// Stage 3.C: re-submit cached light data each frame on update()-skipped frames.
		// Finds the first SHAPE_NODE leaf and calls ResubmitCachedLightData() on it,
		// refreshing cachedGpuLightIndex_ without touching s_listOfLights.
		// Precondition: at least one update() must have run (lightData_ populated).
		// IsStaticNow() guarantees this — registration requires a prior full-bake.
		void ResubmitCachedGpuLightData();

		// [LIGHTBAKE v1] peekCachedLeafLightData: after CacheGpuLightData()
		// has run this frame, returns the first SHAPE_NODE leaf's durable
		// post-decompose lightData_ (TG_Shape member; TG_MultiShape is a
		// friend) or nullptr — the bit-identity-preserving bake source.
		// EmitBakedGpuLightData: re-emit a baked constant into a per-frame
		// slot WITHOUT GatherLights/decompose/template recompute.
		const TG_HWLightsData* peekCachedLeafLightData();
		void EmitBakedGpuLightData(int32_t recipeIndex, const TG_HWLightsData& baked);

		//This function sets the fog values for the shape.  Straight fog right now.
		void SetFogRGB (DWORD fRGB);

		// Track B (widen registry): recipe-build-only variant.
		// Runs the hierarchy traversal to populate listOfShapes[i].shapeToWorld
		// per leaf without allocating from TGL vertex/face/color pools and without
		// requiring TG_Shape::s_cameraOrigin to be non-null. Safe to call during
		// Mission::init before the camera is initialized.
		long TransformMultiShape_BuildRecipe (glm::vec3 *pos, glm::quat *rot);

		// Slice D-leaf-skip-v2 (2026-05-09): runtime alias of _BuildRecipe for the
		// GPU mech body callsite. Reuses s_buildRecipeOnly mechanism to skip
		// per-leaf dispatch + MultiTransformShadows; preserves the OUTER
		// hierarchy walk that populates listOfShapes[i].shapeToWorld (which
		// the GPU mech batcher's submitActor and getNodePosition read).
		// Aliased rather than reusing _BuildRecipe directly for self-documenting
		// use at the mech runtime callsite vs the static-prop registry init use.
		long TransformMultiShape_HierarchyOnly (glm::vec3 *pos, glm::quat *rot);
		// Presentation joints can move while the simulation turn is frozen.
		void InvalidateHierarchy()
		{
			for (long i = 0; i < numTG_Shapes; ++i)
				listOfShapes[i].calcedThisFrame = -1;
		}

		//This function rotates the heirarchy from this node down.  Used for torso twists, arms, etc.
		// SHould only be called once this way.  This way is DAMNED SLOW!!!  STRICMP!  IT returns the node num
		// Call that from then on!
		long SetNodeRotation (const char *nodeName, glm::quat *rot);

		void SetNodeRotation (long nodeNum, glm::quat *rot)
		{
			if ((nodeNum >= 0) && (nodeNum < numTG_Shapes))
				listOfShapes[nodeNum].baseRotation = *rot;
		}

		// CPU fallback draw of every leaf the batchers declined (CpuDraw: ambient + infinite lights, queued through the
		// render submissions); call after a TransformMultiShape_* walk.
		// spotlightsOnly: just the SpotLight_ cone leaves (a body a batcher already drew).
		void Render (bool refreshTextures = false, float forceZ = -1.0f, bool spotlightsOnly = false);
		// Call in the update phase, after the hierarchy walk, for any shape Render may draw: reserves its triangles.
		void ReserveCpuDraw (bool spotlightsOnly = false);
		// UI preview (SimpleCamera, preview_draw.cpp): draws every leaf from the hierarchy matrices; call after a TransformMultiShape*.
		void PreviewDraw (void);

		long GetNumShapes (void)
		{
			return numTG_Shapes;
		}

		// Task 5 (Track B): const access to per-leaf shape records for
		// mission-load bulk registration in BldgAppearance/TreeAppearance.
		const TG_ShapeRec* GetShapeRec (int i) const
		{
			if (i < 0 || i >= numTG_Shapes) return nullptr;
			return &listOfShapes[i];
		}

		void ScaleShape (float scaleFactor)
		{
			if (scaleFactor >= 0.0f)
			{
				for (int i=0;i<numTG_Shapes;i++)
					listOfShapes[i].node->shapeScalar = scaleFactor;
			}
		}
		
		long GetNumVerticesInShape (long i)
		{
			if ((i >= 0) && (i < numTG_Shapes))
			{
				return myMultiType->listOfTypeShapes[i]->GetNumTypeVertices();
			}
			
			return 0;
		}

		glm::vec3 GetShapeVertexInWorld(long shapeNum, long vertexNum, float rotation);
		glm::vec3 GetShapeVertexInEditor(long shapeNum, long vertexNum, float rotation);
		
		float GetFrameNum (void)
		{
			return frameNum;
		}

		void SetFrameNum (float frame)
		{
			frameNum = frame;
		}

		void SetIsClamped (bool value)
		{
			isClamped = value;
		}
		
		//Function returns 0 if OK.  -1 if textureName is longer then nameLength-1.
		//This function digs the texture name(s) out of the ASE file so that the
		//User can load and manage them anyway they want to.
		long GetTextureName (DWORD textureNum, char *textureName, long nameLength)
		{
			return myMultiType->GetTextureName(textureNum,textureName,nameLength);
		}

		long GetNumTextures (void)
		{
			return myMultiType->GetNumTextures();
		}

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
        long SetTextureHandle(DWORD textureNum, DWORD node);
        DWORD GetTextureHandle(DWORD textureNum) const;
        void BindMaterials();
        bool usesTypeMaterials() const;
        RenderCore::MaterialSetRef materialSet() const { return materialSet_; }

		float GetExtentRadius (void)
		{
			return myMultiType->extentRadius;
		}
		
		glm::vec3 GetMaxBox (void)
		{
			return myMultiType->maxBox;
		}
		
		glm::vec3 GetMinBox (void)
		{
			return myMultiType->minBox;
		}
		
		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
        long SetTextureAlpha(DWORD textureNum, bool alphaFlag);

		glm::vec3 GetRootNodeCenter (void)
		{
			glm::vec3 result;
			result.x = result.y = result.z = 0.0f;
			for (int i=0;i<numTG_Shapes;i++)
			{
				if (listOfShapes[i].parentNode == nullptr)
				{
					result = listOfShapes[i].node->myType->GetNodeCenter();
				}
			}
			
			return result;
		}

		glm::vec3 GetRootRelativeNodeCenter (void)
		{
			glm::vec3 result;
			result.x = result.y = result.z = 0.0f;
			for (int i=0;i<numTG_Shapes;i++)
			{
				if (listOfShapes[i].parentNode == nullptr)
				{
					result = listOfShapes[i].node->myType->GetRelativeNodeCenter();
				}
			}
			
			return result;
		}

		void SetCurrentAnimation (long shapeNum, TG_AnimationPtr anim)
		{
			if ((shapeNum >= 0) && (shapeNum < numTG_Shapes))
				listOfShapes[shapeNum].currentAnimation = anim;
		}

		//This function clears all of the animation pointers.
		//As such, the shape will stop animating and return to base pose.
		void ClearAnimation (void)
		{
			for (int i=0;i<numTG_Shapes;i++)
				listOfShapes[i].currentAnimation = nullptr;
		}

		void SetARGBHighLight (DWORD argb)
		{
			for (int i=0;i<numTG_Shapes;i++)
				listOfShapes[i].node->SetARGBHighLight(argb);
		}

		// Team paint (0x??RRGGBB x3: red, green, blue slots of the stock paint scheme). The static-prop batcher
		// forwards it per instance; a material's team mask tints with it in the fragment shader.
		void SetTeamPaint (DWORD r, DWORD g, DWORD b) { teamPaint[0] = r; teamPaint[1] = g; teamPaint[2] = b; teamPaintValid = true; }
		DWORD teamPaint[3] = {0, 0, 0};
		bool  teamPaintValid = false;
		
		void SetLightsOut (bool lightFlag)
		{
			for (int i=0;i<numTG_Shapes;i++)
				listOfShapes[i].node->SetLightsOut(lightFlag);
		}
		
 		glm::vec3 GetTransformedNodePosition (glm::vec3 *pos, glm::quat *rot, const char *nodeId);
 		glm::vec3 GetTransformedNodePosition (glm::vec3 *pos, glm::quat *rot, long nodeId);

		TG_TypeMultiShapePtr GetMultiType (void)
		{
			return myMultiType;
		}
		
		// This function takes the shape named nodeName and all of its children, detaches them
		// from the current heirarchy and stuffs them into a new MultiShape which it passes back
		// Uses are endless but for now limited to blowing the arms off of the mechs!
		TG_MultiShape* Detach (const char *nodeName);
		
		// This function takes the shape named nodeName and all of its children and stops processing
		// them forever.  Since we can never re-attach a mech arm in the field, this is OK!
		void StopUsing (const char *nodeName);
		
 		// Tells me if the passed in nodeName is a child of the parentName.
		bool isChildOf (const char *nodeName, const char* parentName);
		
		char *GetNodeId (long shapeNum)
		{
			if ((shapeNum >= 0) && (shapeNum < numTG_Shapes))
				return (listOfShapes[shapeNum].node->getNodeName());
			
			return nullptr;
		}
		
		long GetNodeNameId (const char * nodeId)
		{
			for (int  i=0;i<numTG_Shapes;i++)
			{
				//-----------------------------------------------------------------
				// Scan through the list of shapes and dig out the number needed.
				// DO NOT UPDATE THE HEIRARCHY!!!!
				// This thing may not have updated itself this turn yet!!!
				if (S_stricmp(listOfShapes[i].node->myType->getNodeId(),nodeId) == 0)
					return i;
			}

			return -1;
		}

		void SetIsHudElement (void)
		{
			isHudElement = true;
		}

		void SetAlphaValue (BYTE aVal)
		{
			alphaValue = aVal;
		}
		
		BYTE GetAlphaValue (void)
		{
			return alphaValue;
		}
		
		void SetUseShadow (bool flag)
		{
			d_useShadows = flag;
		}
};

using TG_MultiShapePtr = TG_MultiShape*;

//-------------------------------------------------------------------------------
// The meat and Potatoes part of the animation system
// TG_AnimShape
class TG_AnimateShape
{
	friend class TG_TypeMultiShape;
	friend class TG_MultiShape;
	//--------------------------------------------------------------------
	//This class stores a single gesture worth of animation data.
	//Works by loading the animation data for each NODE in the multiShape
	//From an ASE file.  When we change an animation gesture, this class
	//will shove the correct pointers to the data down to the TG_ShapeRecs.
	//Other then data storage, parsing, and the pointer sets, does zipp!
	protected:
		std::vector<TG_Animation>	animationStore;	//Owns the animations; listOfAnimation is a view, never resized after it is taken.
		TG_AnimationPtr		listOfAnimation;
		int                 count;
		int                 actualCount;
		bool				shapeIdsSet;

	public:
		void init (void)
		{
			listOfAnimation = nullptr;
			count = actualCount = 0;
			shapeIdsSet = false;
		}

		TG_AnimateShape (void)
		{
			init();
		}

		~TG_AnimateShape (void)
		{
			destroy();
		}

		long LoadBinaryCopy (const char *fileName);
		void SaveBinaryCopy (const char *fileName);

		//This function frees all of the RAM allocated by this class and resets vars to initial state.
		void destroy (void);

		//This function loads the animation data contained in the file passed in.
		//It sets up a pointer to the multi-shape so that animation data for each
		//Node in the Multi-Shape can be loaded.
		//It mallocs memory.
		long LoadTGMultiShapeAnimationFromASE (const char *filename, TG_TypeMultiShapePtr shape, bool skipIfBinary = false, bool forceMakeBinary = false);

		//This function copies the pointers to the animation data in this class to the
		//TG_MultiShape being drawn.  Nothing else happens.
		void SetAnimationState (TG_MultiShapePtr mShape);
		const char* MissingAnimationNode(TG_TypeMultiShape* shape) const;

		float GetFrameRate (void)
		{
			if (listOfAnimation)
				return listOfAnimation[0].frameRate;

			return 30.0f;
		}

		void SetFrameRate (float nFrameRate)
		{
			if (listOfAnimation)
				listOfAnimation[0].frameRate = nFrameRate;
		}

		void ReverseFrameRate (void)
		{
			if (listOfAnimation)
				listOfAnimation[0].frameRate = -listOfAnimation[0].frameRate;
		}

		long GetNumFrames (void)
		{
			if (listOfAnimation)
				return listOfAnimation[0].numFrames;

			return 0;
		}

		void resetShapeIds (void)
		{
			shapeIdsSet = false;
		}
};

using TG_AnimateShapePtr = TG_AnimateShape*;
//-------------------------------------------------------------------------------

// FRAME-JOBS-2D: counts ResubmitCachedGpuLightData calls made from worker threads.
// Defined in msl.cpp; read by objmgr.cpp for the per-frame diagnostic.
#include <atomic>
extern std::atomic<int> g_workerResubmitCalls;

#endif
