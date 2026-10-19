//-------------------------------------------------------------------------------
//
// Tiny Geometry Layer
//
// For MechCommander 2 -- Copyright (c) 1999 Microsoft
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TGL_H
#define TGL_H
//-------------------------------------------------------------------------------
// Include Files
#ifndef HEAP_H
#include"heap.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#include<stuff/stuff.hpp>
#include<gameos.hpp>
#include <cstdint>  // uint64_t (Tier-1 instrumentation, stability spec §2)
#include <cstring>  // strcmp (rebuildShapeRecParents)
#include <memory>   // the per-frame pools own their storage
#include <vector>   // an animation owns its key tracks
#include "../RenderCore/gpu_abi/GpuObjectLights.h"  // single-source per-object light record

//-------------------------------------------------------------------------------
// Structs used by layer.
//
using DWORDPtr = DWORD*;

//-------------------------------------------------------------------------------
// TG_TypeVertex
//This Structure stores information for each vertex of the shape which DOES NOT CHANGE by instance
struct _TG_TypeVertex
{
	//Only changes at load time.
	glm::vec3	position;				//Position of vertex relative to base position of shape.
	glm::vec3 normal;					//Vertex Normal
	DWORD    		aRGBLight;				//Vertex Light and Alpha

}; using TG_TypeVertex = _TG_TypeVertex;

using TG_TypeVertexPtr = TG_TypeVertex*;

//-------------------------------------------------------------------------------
// TG_HWTypeVertex
//This Structure stores information for each vertex of the shape which DOES NOT CHANGE by instance
struct _TG_HWTypeVertex
{
	//Only changes at load time.
	glm::vec3	position;				//Position of vertex relative to base position of shape.
	glm::vec3 normal;					//Vertex Normal
	DWORD    		aRGBLight;				//Vertex Light and Alpha
	float			u;
	float			v;

}; using TG_HWTypeVertex = _TG_HWTypeVertex;

using TG_HWTypeVertexPtr = TG_HWTypeVertex*;

//-------------------------------------------------------------------------------
// TG_Vertex
// This Structure stores information for each vertex of the shape which is instance specific
// Its used to store UNCHANGING or rarely changing Light/Fog data so I don't have to calc every frame!
struct _TG_Vertex
{
	BYTE			fog;
	BYTE			redSpec;
	BYTE			greenSpec;
	BYTE			blueSpec;
}; using TG_Vertex = _TG_Vertex;

using TG_VertexPtr = TG_Vertex*;

//-------------------------------------------------------------------------------
// TG_ShadowVertex
//This Structure stores information for each vertex of the shadow for this shape.
// Nothing changes frame to frame here anymore unless light source is moving which is now RARE!
struct _TG_ShadowVertex
{
	//Changes every frame if local light.  Rarely for the sun.  Moves slowly!
	bool 			bDataIsNotValid;		//Indicates that the struct contains unitialized data
	glm::vec3	position;				//Position of vertex relative to base position of shape.
}; using TG_ShadowVertex = _TG_ShadowVertex;

using TG_ShadowVertexPtr = TG_ShadowVertex*;

//-----------------------------------------------------------------------------------
// TG_ShadowVertexTemp
// Stores VOLATILE information for the shadow vertices.  These can come from a pool!
struct _TG_ShadowVertexTemp
{
	DWORD    		fRGBFog;				//Vertex Fog and Specular Color.  Needed if shadow is fogged.
											//Every frame for local light.  Once in a blue moon for infinite light.

	//Changes each call to Transform.
	Stuff::Vector4D transformedPosition;	//ScreenCoords with Z depth.  Valid after Transform called.
}; using TG_ShadowVertexTemp = _TG_ShadowVertexTemp;

using TG_ShadowVertexTempPtr = TG_ShadowVertexTemp*;

//-------------------------------------------------------------------------------
// TG_UVData
struct _TG_UVData
{
	float			u0;						//UV Data for each vertex of the triangle
	float 			v0;						//Allows same vertex to have different UVs
	float			u1;
	float			v1;
	float			u2;
	float			v2;

}; using TG_UVData = _TG_UVData;

using TG_UVDataPtr = TG_UVData*;

//-------------------------------------------------------------------------------
// TG_TypeTriangle
// This structure stores the information needed to draw the triangle which does NOT change per instance
struct _TG_TypeTriangle
{
	DWORD    		Vertices[3];			//Indices into Vertex List.
	DWORD    		localTextureHandle;		//Index into texture List.
	DWORD    		renderStateFlags;		//Flags about render for this face.
											//Bit 0 -- backFacing
	TG_UVData  		uvdata;					//Texture UVs for this face.
	glm::vec3 faceNormal;				//Normal Vector to face
}; using TG_TypeTriangle = _TG_TypeTriangle;

// Texture handle can change from frame to frame. Texture Animation!

using TG_TypeTrianglePtr = TG_TypeTriangle*;

//-------------------------------------------------------------------------------
// TG_Triangle
// This structure stores the information needed to draw the triangle per instance.
struct _TG_Triangle
{
	DWORD    		aRGBLight[3];			//RGB Light for this triangle's vertices.
	DWORD    		fRGBLight[3];			//RGB Fog for this triangle's vertices.
}; using TG_Triangle = _TG_Triangle;

// Texture handle can change from frame to frame. Texture Animation!

using TG_TrianglePtr = TG_Triangle*;

//-------------------------------------------------------------------------------
// TG_ShadowTriangle
// This structure stores the information needed to draw the shadow triangle.
struct _TG_ShadowTriangle
{
	DWORD    		Vertices[3];			//Indices into Shadow Vertex List.
}; using TG_ShadowTriangle = _TG_ShadowTriangle;

// Texture handle can change from frame to frame. Texture Animation!

using TG_ShadowTrianglePtr = TG_ShadowTriangle*;

//-------------------------------------------------------------------------------
// TG_LightTypes
inline constexpr unsigned int TG_LIGHT_NONE = 0xffffffff;
inline constexpr int TG_LIGHT_AMBIENT = static_cast<int>(0);
inline constexpr int TG_LIGHT_INFINITE = static_cast<int>(1);
inline constexpr int TG_LIGHT_INFINITEWITHFALLOFF = static_cast<int>(2);
inline constexpr int TG_LIGHT_POINT = static_cast<int>(3);
inline constexpr int TG_LIGHT_SPOT = static_cast<int>(4);
inline constexpr int TG_LIGHT_TERRAIN = static_cast<int>(5);

// 2026-05-20: bumped 256 -> 1024 for (E) SpotLight_ -> real illumination.
// Stage 1 / T1.1 (plan docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md).
// Rationale: the new BldgAppearance::spotlightLights_ + Mech3DAppearance::
// spotlightLights_ vectors call addWorldLight once per registered SpotLight_
// child at first-night-visibility. T0.3 baselines:
//   mc2_04 (night-canary): 0 static-prop + 9 distinct mech-types
//   mc2_10 (stress)      : 1 static-prop family + 9 distinct mech-types
//   mc2_24 (Vedette/LRMC): 0 static-prop + 13 distinct mech-types
// Combined with the existing per-building terrainLight pointLight (1 per
// active building), the 256 ceiling is the realistic concern only on mc2_10-
// class stress missions. 1024 carries ~4x headroom with ~124KB BSS cost
// (one LinearMatrix4D + three Vector3D arrays at tgl.h ~790-793, plus three
// TG_LightPtr arrays at camera.cpp ~411-432). Well below the project's
// RAM-cost-irrelevance threshold per memory/feedback_ram_cost_not_a_concern_below_500mb.md.
// Shader-side MAX_LIGHTS_IN_WORLD in shaders/include/lighting.hglsl is a
// DIFFERENT scope from MAX_LIGHTS_IN_WORLD here, but it is now 32 (equal to
// MAX_HW_LIGHTS_IN_WORLD, the per-object SSBO record cap), not 16.
inline constexpr int MAX_LIGHTS_IN_WORLD = 1024;

inline constexpr int TG_NODE_ID = 25;

inline constexpr int MAX_SCAN_LENGTH = 40;

inline constexpr int MAX_NODES = 256;

inline constexpr int MAX_SHADOWS = 1;
//-------------------------------------------------------------------------------
// TG_Light
// This structure stores the information necessary to light the shape.
struct _TG_Light
{
	DWORD    				lightType;			//Ambient, directional, etc.
	bool					active;				//Should this light be considered on?
	unsigned char			ownerEnabled;		//LIGHT-CULL-RADIUS-1: owner intent (FoW/status/night).
												//Camera::updateLights composes active = ownerEnabled &&
												//light-VOLUME-on-screen for POINT/SPOT, so appearance
												//update culling can no longer strand or leak lights.
	
	protected:
	DWORD    				aRGB;				//Color
	DWORD					OEMaRGB;
	
	public:
	float					intensity;			//How Bright
	float    				closeDistance;		//Distance out light is constant
	float    				farDistance;		//Distance at which light is off
	float					oneOverDistance;	//Used for falloff calc
	glm::vec3 			direction;			//Direction in world of light source.  This is the light spot center for POINT and SPOT
	glm::mat4	lightToWorld;		//Transformation Matrix
	glm::vec3			position;			//Explicit position to aid terrain code with point sources.
	glm::vec3			spotDir;			//Direction of the actual Spotlight to help with shadows, etc.
	float					maxSpotLength;		//Maximum length spotlight can be from target.

	void init (DWORD lType)
	{
		gosASSERT((lType != TG_LIGHT_NONE) && (lType >= TG_LIGHT_AMBIENT) && (lType <= TG_LIGHT_TERRAIN));

		lightType = lType;
		aRGB = OEMaRGB = 0xffffffff;
		intensity = 1.0f;
		direction.x = direction.y = direction.z = 0.0f;
		closeDistance = 0.0f;
		farDistance = 0.0f;
		oneOverDistance = 0.0f;
		position.x = position.y = position.z = -999999.0f;
		spotDir.x = spotDir.y = spotDir.z = 0.0f;
		maxSpotLength = 0.0f;
		active = false;
		ownerEnabled = 1;	//LIGHT-CULL-RADIUS-1: enabled until an owner says otherwise
	}

	void SetaRGB (DWORD newaRGB)
	{
		OEMaRGB = aRGB = newaRGB;
	}

	DWORD GetaRGB (void)
	{
		return aRGB;
	}
	
	void SetLightToWorld (glm::mat4 *l2w)
	{
		lightToWorld = *l2w;
	}

	void SetPosition (glm::vec3 *pos)
	{
		position = *pos;
	}

	void SetFalloffDistances (float inner, float outer)
	{
		closeDistance = inner;
		farDistance = outer;

		oneOverDistance = 1.0f/(outer - inner);
	}

	void SetIntensity (float intensity)
	{
		if (intensity > 1.0f)
			intensity = 1.0f;

		//Scale aRGB by intensity
		DWORD r,g,b;
		r = intensity * ((OEMaRGB >> 16) & 0x000000ff);
		g = intensity * ((OEMaRGB >> 8) & 0x000000ff);
		b = intensity * (OEMaRGB & 0x000000ff);

		aRGB = (0xff << 24) + (r << 16) + (g << 8) + (b);
	}

	bool GetFalloff(float length, float &falloff)
	{
		if (length <= closeDistance)
		{
			falloff = 1.0f;
			return true;
		}

		if (length >= farDistance)
		{
			return false;
		}

		falloff = (farDistance - length) * oneOverDistance;
		return true;
	}

}; using TG_Light = _TG_Light;

using TG_LightPtr = TG_Light*;

// LIGHT-ABI-WIDEN-STAGE0-1: C++ per-object GPU ABI cap, derived from the single-source
// record in RenderCore/gpu_abi/GpuObjectLights.h (its generated asserts and
// lighting.hglsl's ObjectLights come from the same declaration). This is the record
// inner-array size, NOT a runtime clamp -- runtime population stays 16 (txmmgr.cpp
// GatherLightsParameters hardcodes 16). DIFFERENT from this header's
// MAX_LIGHTS_IN_WORLD (=1024, the global scene light pool -- do not conflate).
inline constexpr int MAX_HW_LIGHTS_IN_WORLD = static_cast<int>(kGpuObjectLightsMax);

// Slice 2 (object-offload) -- Stage 2.C: per-object GPU light record, including the
// per-light falloff distances (lightFalloff[i] = closeDistance, farDistance,
// oneOverDistance, unused). The layout (std430, N*112 + 16 bytes per record) lives in
// GpuObjectLights; this wrapper only adds the zero-initialising constructor. The mc2_24
// regression of 2026-05-02 came from extending the C++ side without the matching GLSL
// change, which is exactly what the shared declaration now rules out.
struct TG_HWLightsData : GpuObjectLights {
	TG_HWLightsData()
    {
		memset(static_cast<GpuObjectLights*>(this), 0, sizeof(GpuObjectLights));
        pad[0] = pad[1] = pad[2] = 13;
    }
};

static_assert(sizeof(TG_HWLightsData) == sizeof(GpuObjectLights),
              "TG_HWLightsData must add no members to the shared GPU record");

using TG_HWLightsDataPtr = TG_HWLightsData*;

//-------------------------------------------------------------------------------
// RENDERER-INPUT-READBACK-1 -- backing call for the `light_row <partId>`
// command-channel verb. Body lives in mclib/txmmgr.cpp, immediately below
// GatherLightsParameters(), because it must see the same file-static state.
//
// WHY A WRAPPER IS NEEDED AT ALL: GatherLightsParameters() takes no light list.
// It reads TG_Shape::s_listOfLights / s_numLights -- statics that the LAST
// SetLightList() caller wrote. "Which lights did object X get?" is therefore
// not a property of X; it is the result of running the gather against the list
// X's appearance class passes (eye->getWorldLights() for Bldg/Tree/Mech3D/GV,
// and literally NULL for GenericAppearance -- mclib/genactor.cpp:1238).
//
// This installs `lights`/`nLights`, calls the REAL GatherLightsParameters, and
// restores both statics. It does NOT reimplement the active-filter, the
// runtime clamp, or the MC2_LIGHT_POS_COLUMN_FIX translation copy: a second
// copy of that arithmetic could agree with itself while disagreeing with the
// shipped draw, which is the exact instrument failure this readback family
// exists to remove.
//
// `lights == NULL, nLights == 0` is a legitimate input, not an error.
// Read-only with respect to the frame: both statics are restored before
// return, so probing from the mission-update half of a frame cannot change
// what the render that follows gathers.
//
// KNOWN OBSERVER EFFECT -- read this before trusting a counter.
// Calling the real gather means this probe also trips the gather's own
// DIAGNOSTIC state, all of it in mclib/txmmgr.cpp:
//   * MC2_OBJECT_PARITY_TRACE's one-shot dump (s_lightDumpDone) -- a probe
//     can consume the single session dump that a real draw was meant to emit;
//   * MC2_LIGHT_ROW_CENSUS tallies (s_lightRowCensusTab / ...Calls) and its
//     "FIRST positional light" edge;
//   * MC2_LIGHT_CLAMP_PROBE's call count and high-water;
//   * the SPOT_DIAG v1 pack counters (s_spotDiagPack*).
// So: do NOT issue `light_row` during a run whose purpose is to read any of
// those counters. The alternative -- a private reimplementation of the
// gather -- was rejected deliberately: a second copy can agree with itself
// while disagreeing with the shipped draw, and a wrong light row reported
// confidently is far worse than a perturbed diagnostic tally. Nothing here
// touches render state, only diagnostic bookkeeping.
void ProbeGatherLightsForList(TG_LightPtr* lights, unsigned long nLights,
                              TG_HWLightsData* out);



//-------------------------------------------------------------------------------
// TG_Texture
struct _TG_Texture
{
	char				textureName[256];
	DWORD				mcTextureNodeIndex;
	DWORD				gosTextureHandle;
	bool				textureAlpha;
}; using TG_Texture = _TG_Texture;

using TG_TexturePtr = TG_Texture*;

//-------------------------------------------------------------------------------
// TG_TinyTexture
struct _TG_TinyTexture
{
	DWORD				mcTextureNodeIndex;
	DWORD				gosTextureHandle;
	bool				textureAlpha;
}; using TG_TinyTexture = _TG_TinyTexture;

using TG_TinyTexturePtr = TG_TinyTexture*;

class TG_Shape;

// Ceilings for the counts a shape file claims (mod content): no stock shape comes near them, a larger count is a corrupt file.
inline constexpr DWORD kMaxShapeVertices = 1u << 20;
inline constexpr DWORD kMaxShapeTriangles = 1u << 20;
inline constexpr DWORD kMaxShapeTextures = 4096;
inline constexpr int kMaxMultiShapeNodes = 4096;
//-------------------------------------------------------------------------------
// TG_Animation
struct _TG_Animation
{
	char 						nodeId[TG_NODE_ID];		//Node ID
	DWORD						shapeId;				//DON'T SCAN EVERY FRAME.  WOW IS IT SLOW!!!!!  Set this first time through and its simple.
	DWORD						numFrames;				//Number of Frames of animation.
	float 						frameRate;				//Number of Frames Per Second.
	float						tickRate;				//Number of Ticks Per Second.
	glm::quat		*quat = nullptr;		//Stores animation offset in Quaternion rotation.  A view of quatStore.
	glm::vec3				*pos = nullptr;			//Stores Positional offsets if present.  OTHERWISE NULL!!!!!!!!  A view of posStore.
	std::vector<glm::quat>	quatStore;		//Owns the rotation track; sized once (numFrames) before quat is taken.
	std::vector<glm::vec3>			posStore;		//Owns the position track.

	// A copy would leave the views pointing at the original's tracks; a move keeps the buffers.
	_TG_Animation (void) = default;
	_TG_Animation (const _TG_Animation&) = delete;
	_TG_Animation& operator= (const _TG_Animation&) = delete;
	_TG_Animation (_TG_Animation&&) noexcept = default;
	_TG_Animation& operator= (_TG_Animation&&) noexcept = default;

	void SaveBinaryCopy (File *binFile);
	// False when the file claims a frame count or track size the animation cannot hold; the animation is left empty.
	bool LoadBinaryCopy (File *binFile);
	
}; using TG_Animation = _TG_Animation;

using TG_AnimationPtr = TG_Animation*;

//-------------------------------------------------------------------------------
// TG_ShapeRec
struct _TG_ShapeRec
{
	std::unique_ptr<TG_Shape>	node;					//OWNS the TG_Shape for this piece (B7o.6b); moved only by transferFrom
	glm::mat4		localShapeToWorld;		//Matrix to transform this TG_Shape.
	glm::mat4		shapeToWorld;			//Matrix to transform this TG_Shape.
	glm::mat4		worldToShape;			//Inverse of above Matrix.
	int32_t                     calcedThisFrame;		//Turn number this matrix is current for.
	bool						processMe;				//Flag indicating if I should transform/draw this.  Used for arms off.
	TG_AnimationPtr				currentAnimation;		//Animation data being applied to this shape.  OK if NULL
	_TG_ShapeRec				*parentNode;			//Parent Node.  OK if NULL but only for ROOT node!
	glm::quat		baseRotation;			//Always ZERO unless set by appearance controlling this shape.
	
	// A record owns its node, so it is never copied (B7o.6b: the old copy assignment duplicated the owner).
	_TG_ShapeRec (void) = default;
	_TG_ShapeRec (const _TG_ShapeRec&) = delete;
	_TG_ShapeRec &operator=(const _TG_ShapeRec&) = delete;

	// B7o.6a: Detach and the shape compaction MOVE a record: the node goes over (the source keeps none) and the per-frame fields
	// reset exactly as the old copy assignment reset them; the parent pointer is copied and must be rebuilt for the new array.
	// Defined after TG_Shape (the unique_ptr move needs the complete type).
	inline void transferFrom (_TG_ShapeRec &rec);
	inline void transferFrom (_TG_ShapeRec &rec, const TG_TinyTexture* textures);

}; using TG_ShapeRec = _TG_ShapeRec;

using TG_ShapeRecPtr = TG_ShapeRec*;

// B7o.6a: points each record's parent at the record of the SAME array whose node id equals its parent id (the first match);
// a record whose parent id matches none keeps its parent pointer (the detached root was nulled by the caller).
template <class NodeIdOf, class ParentIdOf>
inline void rebuildShapeRecParents (TG_ShapeRec *recs, long count, NodeIdOf nodeIdOf, ParentIdOf parentIdOf)
{
	for (long i = 0; i < count; i++)
	{
		for (long j = 0; j < count; j++)
		{
			if (strcmp(parentIdOf(recs[i]), nodeIdOf(recs[j])) == 0)
			{
				recs[i].parentNode = &recs[j];
				break;
			}
		}
	}
}

inline constexpr int TYPE_NODE = 0;
inline constexpr int SHAPE_NODE = 1;

class TG_TypeNode
{
	//---------------
	// Data Members
	protected:
		glm::vec3			nodeCenter;
		glm::vec3			relativeNodeCenter;
		char					nodeId[TG_NODE_ID];
		char					parentId[TG_NODE_ID];
		
	//---------------------
	// Member Functions
	protected:

	public:
		// Virtual so a TG_TypeShape deleted through a TG_TypeNode* runs ~TG_TypeShape. Deliberately does NOT call destroy(): a base
		// destructor never dispatches virtually; the derived destructor releases its own stores through destroy() (idempotent).
		virtual ~TG_TypeNode (void) = default;

 		virtual void init (void)
		{
			nodeId[0] = parentId[0] = '\0';
			relativeNodeCenter.x = relativeNodeCenter.y = relativeNodeCenter.z = 0.0f;
			nodeCenter.x = nodeCenter.y = nodeCenter.z = 0.0f;
		}
		
		virtual void destroy (void);
		
		char *getNodeId (void)
		{
			return nodeId;
		}

		char *getParentId (void)
		{
			return parentId;
		}

		glm::vec3 GetNodeCenter (void)
		{
			return nodeCenter;
		}

		glm::vec3 GetRelativeNodeCenter (void)
		{
			return relativeNodeCenter;
		}

		void MoveNodeCenterRelative (glm::vec3 parent)
		{
			relativeNodeCenter = nodeCenter;
			relativeNodeCenter -= parent;
		}
		
		virtual void movePosRelativeCenterNode (void)
		{
		}

		virtual int GetNumTypeVertices (void)
		{
			return 0;
		}

		virtual int GetNodeType (void)
		{
			return TYPE_NODE;
		}
		
		//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
		//This function simply parses the ASE buffers handed to it.  This allows
		//users to load the ase file themselves and manage their own memory for it.
		//It allocates memory for internal Lists.  These are straight mallocs at present.
		//
		// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
		// Files will require user intervention to parse!!
		virtual long ParseASEFile (BYTE *aseBuffer, const char *filename)
		{
			return 0;
		}

		//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
		//This function simply parses the ASE buffers handed to it.  This allows
		//users to load the ase file themselves and manage their own memory for it.
		//It allocates memory for internal Lists.  These are straight mallocs at present.
		//
		// NOTE: Only takes the first HELPEROBJECT from the ASE file.  Multi-object
		// Files will require user intervention to parse!!
		virtual long MakeFromHelper (BYTE *aseBuffer, const char *filename);

		//Function returns 0 if OK.  -1 if file not found or file not ASE Format.		
		//This function loads the ASE file into the TG_Triangle and TG_Vertex lists.
		//It allocates memory.  These are straight mallocs at present.
		//
		// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
		// Files will require user intervention to parse!!
		virtual int LoadTGShapeFromASE (const char *fileName)
		{
			return 0;
		}

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
		virtual long SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle)
		{
			return 0;
		}

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
		virtual long SetTextureAlpha (DWORD textureNum, bool alphaFlag)
		{
			return 0;
		}

		//Need this so that Multi-Shapes can let each shape know texture info.
		virtual void CreateListOfTextures (TG_TexturePtr list, DWORD numTxms)
		{
		}

		//--------------------------------------------------------------
		// Creates an instance of this shape for the game to muck with.
		virtual std::unique_ptr<TG_Shape> CreateFrom (void);

		virtual void SetAlphaTest (bool flag)
		{
		}

		virtual void SetFilter (bool flag)
		{
		}

		virtual void SetLightRGBs (DWORD hPink, DWORD hGreen, DWORD hYellow)
		{
		}

		// False when the file claims a count this node cannot hold; the stream is then mid-node and the caller must stop reading it.
 		virtual bool LoadBinaryCopy (File &binFile);
		virtual void SaveBinaryCopy (File &binFile);

};

using TG_TypeNodePtr = TG_TypeNode*;

class TG_MultiShape;
class TG_TypeMultiShape;
class TG_Shape;
//-------------------------------------------------------------------------------
// The meat and Potatoes part.
// TG_Shape
class TG_TypeShape : public TG_TypeNode
{
	//-------------------------------------------------------
	// This class runs the shape.  Knows how to load/import
	// an ASE file.  Can dig information out of the file for
	// User use (i.e. Texture Names).  Transforms, lights, clips
	// and renders the shape.

	friend class TG_TypeMultiShape;
	friend class TG_MultiShape;
	friend class TG_Shape;
	friend class GpuStaticPropBatcher;
	friend class GpuMechBatcher;

	//-------------
	//Data Members
	protected:
		DWORD					numTypeVertices;			//Number of vertices in Shape
		DWORD					numTypeTriangles;			//NUmber of triangles in Shape
		DWORD					numTextures;				//Number of textures in Shape

		TG_TypeVertexPtr		listOfTypeVertices;			//Memory holding all vertex data
		TG_TypeTrianglePtr		listOfTypeTriangles;		//Memory holding all triangle data
		TG_TinyTexturePtr		listOfTextures;				//List of texture Structures for this shape.

		// The three lists above are views of these stores, which own the memory; each is sized once (from a checked count) before its view is taken.
		std::vector<TG_TypeVertex>		typeVertexStore;
		std::vector<TG_TypeTriangle>	typeTriangleStore;
		std::vector<TG_TinyTexture>		tinyTextureStore;

		DWORD					hotPinkRGB;					//Stores the value for this shape to replace hot Pink With
		DWORD					hotYellowRGB;				//Stores the value for this shape to replace hot Yellow With 
		DWORD					hotGreenRGB;            	//Stores the value for this shape to replace hot Green With 

		bool					alphaTestOn;				//Decides if we should draw alphaTest On or not!
		bool					filterOn;					//Decides if we should filter the shape or not!

		HGOSBUFFER				vb_;
		HGOSBUFFER				ib_;
		HGOSVERTEXDECLARATION	vdecl_;

	//-----------------
	//Member Functions
	protected:

	public:
		virtual void init (void)
		{
			numTypeVertices = numTypeTriangles = numTextures = 0;

			listOfTypeVertices = nullptr;
			listOfTypeTriangles = nullptr;
			listOfTextures = nullptr;

			alphaTestOn = false;

			filterOn = true;

			relativeNodeCenter.x = relativeNodeCenter.y = relativeNodeCenter.z = 0.0f;
			nodeCenter.x = nodeCenter.y = nodeCenter.z = 0.0f;
			
			hotPinkRGB = 0x00cbf0ff;
			hotYellowRGB = 0x00FEfF91;
			hotGreenRGB = 0x000081b6;

			vb_ = ib_ = 0;
			vdecl_ = 0;
		}
		
		TG_TypeShape (void)
		{
			init();
		}

		// Idempotent: stores swapped empty, views and counts zeroed, GPU handles released once and nulled. Inline (B7o.6b) so the
		// unit tier exercises this exact body.
		virtual void destroy (void)
		{
			std::vector<TG_TypeVertex>().swap(typeVertexStore);
			listOfTypeVertices = nullptr;

			std::vector<TG_TypeTriangle>().swap(typeTriangleStore);
			listOfTypeTriangles = nullptr;

			std::vector<TG_TinyTexture>().swap(tinyTextureStore);
			listOfTextures = nullptr;

			numTypeVertices = numTypeTriangles = numTextures = 0;

			if (vb_) {
				gos_DestroyBuffer(vb_);
				vb_ = nullptr;
			}

			if (ib_) {
				gos_DestroyBuffer(ib_);
				ib_ = nullptr;
			}

			if (vdecl_) {
				gos_DestroyVertexDeclaration(vdecl_);
				vdecl_ = nullptr;
			}
		}

		~TG_TypeShape (void)
		{
			destroy();
		}

		virtual int GetNumTypeVertices (void)
		{
			return numTypeVertices;
		}

		// NS3 standalone-loader / asset-viewer read-only accessors.
		// Expose the immutable per-type geometry arrays (otherwise protected)
		// so a game-free consumer can walk vertices/triangles after load.
		const TG_TypeVertex* GetTypeVertices (void) const
		{
			return listOfTypeVertices;
		}
		// BT2018-SKEL-ENGINE-1B-RUNTIME: writable type-vertex access for the
		// per-frame imported-mech re-bake (mech_anim_runtime / assimp_importer).
		// Imported skinned mechs re-pose their merged type geometry in place each
		// frame; CPU TransformMultiShape then re-reads it. Do NOT use for stock
		// ASE shapes (their type geometry is immutable).
		TG_TypeVertex* GetTypeVerticesMutable (void)
		{
			return listOfTypeVertices;
		}
		const TG_TypeTriangle* GetTypeTriangles (void) const
		{
			return listOfTypeTriangles;
		}
		long GetNumTypeTriangles (void) const
		{
			return static_cast<long>(numTypeTriangles);
		}

		// MODEL-OVERRIDE auto-ground: shift all type-vertices along the up axis
		// (stuff.y) so an imported mesh's base sits at the local origin.
		void TranslateTypeVerticesY(float dy)
		{
			if (!listOfTypeVertices) return;
			for (DWORD i = 0; i < numTypeVertices; ++i)
				listOfTypeVertices[i].position.y += dy;
		}

 		//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
		//This function simply parses the ASE buffers handed to it.  This allows
		//users to load the ase file themselves and manage their own memory for it.
		//It allocates memory for internal Lists.  These are straight mallocs at present.
		//
		// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
		// Files will require user intervention to parse!!
		virtual long ParseASEFile (BYTE *aseBuffer, const char *filename);	//filename for error reporting ONLY

		//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
		//This function simply parses the ASE buffers handed to it.  This allows
		//users to load the ase file themselves and manage their own memory for it.
		//It allocates memory for internal Lists.  These are straight mallocs at present.
		//
		// NOTE: Only takes the first HELPEROBJECT from the ASE file.  Multi-object
		// Files will require user intervention to parse!!
		virtual long MakeFromHelper (BYTE *aseBuffer, const char *filename);

		//Function returns 0 if OK.  -1 if file not found or file not ASE Format.		
		//This function loads the ASE file into the TG_Triangle and TG_Vertex lists.
		//It allocates memory.  These are straight mallocs at present.
		//
		// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
		// Files will require user intervention to parse!!
		virtual int LoadTGShapeFromASE (const char *fileName);

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
		virtual long SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle);

		//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
		//This function takes the gosTextureHandle passed in and assigns it to the
		//textureNum entry of the listOfTextures;
		virtual long SetTextureAlpha (DWORD textureNum, bool alphaFlag);

		//Need this so that Multi-Shapes can let each shape know texture info.
		virtual void CreateListOfTextures (TG_TexturePtr list, DWORD numTxms);

 		virtual void movePosRelativeCenterNode (void);
		
		//--------------------------------------------------------------
		// Creates an instance of this shape for the game to muck with.
		virtual std::unique_ptr<TG_Shape> CreateFrom (void);

		virtual void SetAlphaTest (bool flag)
		{
			alphaTestOn = flag;
		}

		virtual void SetFilter (bool flag)
		{
			filterOn = flag;
		}

		virtual void SetLightRGBs (DWORD hPink, DWORD hGreen, DWORD hYellow)
		{
			hotPinkRGB = hPink;
			hotGreenRGB = hGreen;
			hotYellowRGB = hYellow;
		}
		
		virtual int GetNodeType (void)
		{
			return SHAPE_NODE;
		}

		// Track D — populate this shape from a format-agnostic importer
		// (FBX/GLB via Assimp). Caller builds the vertex and triangle
		// vectors and moves them in; this method takes them into class storage
		// and sets nodeId/parentId/nodeCenter. Texture wiring happens separately via the existing
		// CreateListOfTextures method (driven from TG_TypeMultiShape).
		// No Assimp types appear in this signature — header stays clean.
		void InitFromImportedMesh(const char* nodeIdIn, const char* parentIdIn,
		                          const glm::vec3& center,
		                          std::vector<TG_TypeVertex>&& vertices,
		                          std::vector<TG_TypeTriangle>&& triangles);

 		virtual bool LoadBinaryCopy (File &binFile);
		virtual void SaveBinaryCopy (File &binFile);
};

using TG_TypeShapePtr = TG_TypeShape*;

//-------------------------------------------------------------------------------------
// The OTHER meat and Potatoes part.  This is the actual instance the game draws with.
// TG_Shape
// Phase-helper state of MultiTransformShape (defined in tgl_transform_frame.h).
struct TgXformFrame;

class TG_Shape
{
	//-------------------------------------------------------
	// This class runs the shape instance.
	// Transforms, lights, clips and renders the shape.

	friend class TG_MultiShape;
	friend struct _TG_ShapeRec;
	friend class TG_TypeShape;
	friend class TG_TypeNode;
	friend class TG_TypeMultiShape;
	friend class GpuStaticPropBatcher;
	// Slice 2 (object-offload) — Stage 2.A introduced this free function as
	// the per-leaf eligibility check called from the addRenderShape gate at
	// tgl.cpp:2522 (legacy bShadersDrawPathEnabled path). It needs to read
	// `myType` and `myType->GetNodeType()`. Caught by Stage 2.B's clean
	// rebuild of gos_static_prop_batcher.cpp (msl.h header touch invalidated
	// the .obj cache that previously masked the access; cdcdb7d shipped this
	// declaration but the existing mc2.exe binary predated the commit).
	friend bool eligibleForGpuObjects(class TG_Shape* shape);

	//-------------
	//Data Members
	protected:
        const TG_TinyTexture* instanceTextures_ = nullptr; // borrowed from the owning multishape
		TG_TypeNodePtr			myType;						//Pointer to the instance of the shape.
		DWORD					numVertices;				//Number of vertices in Shape
		DWORD					numTriangles;				//NUmber of triangles in Shape
		DWORD					numVisibleFaces;			//Number of non-backfaced non-clipped faces.
		DWORD					numVisibleShadows;			//Number of visible Shadow Faces.

		TG_Vertex *				listOfColors;				//Memory holding all unchanged or rarely changed color data.
		gos_VERTEX * 			listOfVertices;				//Memory holding all vertex data
		TG_TrianglePtr			listOfTriangles;			//Memory holding all triangle data
		DWORDPtr				listOfVisibleFaces;			//Memory holding indices into listOfTriangles
															//Draw in this order.  First entry with value 0xffffffff
															//Means all done, no more to draw.

		float					cur_viewport[4];
		glm::mat4			cur_shape2clip;
        TG_HWLightsData         lightData_;

		std::vector<TG_ShadowVertex>	shadowVertexStore;	//Owns the shadow vertices (built in CreateFrom, released in destroy(): the instance memory is freed without a destructor).
		TG_ShadowVertexPtr		listOfShadowVertices;		//Stores shadow vertex information for the shape.  A view of shadowVertexStore.
															//We just use existing listOfTriangles to draw!
															
		TG_ShadowVertexTempPtr	listOfShadowTVertices;		//Stores just the Volatile data.  Comes from a pool!!
															
		DWORDPtr				listOfVisibleShadows;		//Memory holding indices into listOfTriangles
															//Draw in this order.  First entry with value 0xffffffff
															//Means all done, no more to draw.

		bool					shadowsVisible[MAX_SHADOWS];//Is this shadow worth drawing?

		DWORD					aRGBHighlight;				//Color to add to vertices to make building stand out.
		DWORD					fogRGB;						//Color to make fog.
		
		float					shapeScalar;

		bool					lightsOut;
		bool					noShadow;
		bool					recalcShadows;
		bool					isWindow;
		bool					isSpotlight;
		// EDGE-LIGHT-BILLBOARD-CULL-1 (master-FAIL #8): beacon/runway edge-light
		// glow cards are single flat quads that backface-cull (vanish) as the
		// RTS camera orbits toward top-down. Flagged from the node name at load
		// so Render() can render them two-sided (see MC2_LIGHT_BILLBOARD_TWOSIDED).
		bool					isLightBillboard;

		DWORD					lastTurnTransformed;

	public:
		//Matrices used to transform the shapes.
		static glm::mat4 	*s_cameraOrigin;
		static glm::mat4			*s_cameraToClip;
		static glm::mat4			s_worldToClip;
		static glm::mat4	s_worldToCamera;
		static TG_LightPtr				*s_listOfLights;		//List passed in a transform time
		static DWORD					s_numLights;

		static float					viewMulX;
		static float					viewAddX;
		static float					viewMulY;
		static float					viewAddY;

		static DWORD					fogColor;
		static float					fogFull;
		static float					fogStart;

		static glm::mat4 	s_lightToShape[MAX_LIGHTS_IN_WORLD];
		static glm::vec3			s_lightDir[MAX_LIGHTS_IN_WORLD];
		static glm::vec3			s_spotDir[MAX_LIGHTS_IN_WORLD];
		static glm::vec3			s_rootLightDir[MAX_LIGHTS_IN_WORLD];

		static DWORD					lighteningLevel;

	//-----------------
	//Member Functions
	protected:
		// MultiTransformShape phases (tgl_transform.cpp, tgl_transform_light.cpp).
		void xformGpuOnlyShape(glm::mat4* shapeToClip, bool isHudElement, BYTE alphaValue, bool isClamped);
		void xformVertexProject(glm::mat4* shapeToClip, bool& oneOff, bool& oneOn, TG_TypeShapePtr theShape, long j);
		void xformVertexBaseColor(bool isHudElement, bool isNight, float nightFactor, TG_TypeShapePtr theShape, long j, TgXformFrame& f);
		void xformVertexLights(TG_TypeShapePtr theShape, long j, TgXformFrame& f);
		void xformVertexFog(TG_ShapeRecPtr parentNode, TG_TypeShapePtr theShape, long j, TgXformFrame& f);
		void xformFaceLight(TG_TypeShapePtr theShape, long j, TgXformFrame& f);
		void xformFaceCorners(BYTE alphaValue, TG_TypeShapePtr theShape, long j, TgXformFrame& f);
		void xformFaceQueue(bool isHudElement, BYTE alphaValue, bool isClamped, TG_TypeShapePtr theShape, long j);
		void xformEmitShape(glm::mat4* shapeToClip, bool isHudElement, BYTE alphaValue, bool isClamped, TG_TypeShapePtr theShape);

	public:
		void init (void)
		{
			numVertices = numTriangles = numVisibleFaces = 0;

			listOfVertices = nullptr;
			listOfTriangles = nullptr;
			// Slice 2 (object-offload) — Stage 2.C: keep s_listOfLights and
			// s_numLights consistent. Pre-Stage-2.C a latent bug here cleared
			// s_listOfLights but not s_numLights — never fired in practice
			// because the only GatherLightsParameters call ran inside
			// MultiTransformShape, immediately after SetLightList. Stage 2.C
			// adds a render-time reader (GatherGpuObjectLightDataOnly via
			// submitMultiShape) that can fire when init() ran on a freshly-
			// constructed TG_Shape between an earlier actor's SetLightList
			// and this read — null-deref on s_listOfLights[i] in
			// GatherLightsParameters. Reset both to keep the count truthful.
			s_listOfLights = nullptr;
			s_numLights = 0;
			listOfVisibleFaces = nullptr;

			listOfShadowVertices = nullptr;
			listOfVisibleShadows = nullptr;

			aRGBHighlight = 0x00000000;
			
			shapeScalar = 0.0f;
			
			lightsOut = false;
			
			noShadow = false;
			
			recalcShadows = true;

			isWindow = isSpotlight = false;
			isLightBillboard = false;

			for (long i=0;i<MAX_SHADOWS;i++)
				shadowsVisible[i] = false;

            // sebi: init to not contain garbage
            fogRGB = 0xFFFFFFFF;
            instanceTextures_ = nullptr;
            myType = nullptr;
            numVisibleShadows = 0;
            listOfColors = nullptr;
            listOfShadowTVertices = nullptr;
            lastTurnTransformed = 0;
            //
			
		}
		
		TG_Shape (void)
		{
			init();
		}

		void destroy (void);

		~TG_Shape (void)
		{
			destroy();
		}

		// Stage 3.C: advance lastTurnTransformed without running vertex transform.
		// Prevents TG_Shape::Render's staleness guard from firing when update() is
		// skipped by the static-registry fast path.
		void Touch();

		// GHOST-LIGHT-VISIBILITY-1: read-only view of the staleness stamp that
		// TG_Shape::Render gates on (tgl.cpp:3155 — a leaf whose
		// lastTurnTransformed != turn is NOT drawn this frame). Exposed so
		// light/glow emitters can apply the SAME predicate as the model draw
		// instead of emitting for a leaf the renderer already rejected.
		DWORD GetLastTurnTransformed (void) const { return lastTurnTransformed; }

		//This function sets up the camera Matrices for this TG_Shape to transform
		//itself with.  These matrices are static and only need to be set once per
		//render pass if the camera does not change for that pass.
		static void SetCameraMatrices (glm::mat4 *camOrigin, glm::mat4 *camToClip);

		static void SetViewport (float mulX, float mulY, float addX, float addY);

		static void SetFog (DWORD fRGB, float fStart, float fFull);

		//This function sets the list of lights used by the TransformShape function
		//to light the shape.
		//Function returns 0 if lightList entries are all OK.  -1 otherwise.
		//
		long SetLightList (TG_LightPtr *lightList, DWORD nLights);
		
		//This function sets the fog values for the shape.  Straight fog right now.
		void SetFogRGB (DWORD fRGB);

		//This function does exactly what TranformShape does EXCEPT that the shapeToClip,
		//Lighting and backface matrices have been calculated in the step above this one.
		//This saves enormous processor cycles when matrices are the same and transforms
		//Can just be run from the same matrices without recalcing them!
		//Function returns -1 if all vertex screen positions are off screen.
		//Function returns 0 if any one vertex screen position is off screen.
		//Function returns 1 is all vertex screen positions are on screen.
		// NOTE:  THIS IS NOT A RIGOROUS CLIP!!!!!!!!!
		long MultiTransformShape (glm::mat4 *shapeToClip, glm::vec3 *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped);

		// Slice 2 (object-offload): reduced CPU pass — copy-and-strip variant
		// of MultiTransformShape that keeps transform / screen-space positions /
		// shadow-vertex projection / backface cull (listOfVisibleFaces) /
		// lastTurnTransformed AND retains pool allocations for listOfColors,
		// listOfTriangles, listOfVisibleShadows (PerPolySelect at tglpp.cpp:14-21
		// requires these pointers non-null). Strips the per-vertex lighting
		// kernel, aRGBHighlight additive, per-face lighting, listOfTriangles
		// aRGBLight/fRGBLight writes, addTriangle queue calls, and the
		// addRenderShape block. Stage 2.A: declared and defined but unused;
		// Stage 2.B wires call sites in BldgAppearance/TreeAppearance/
		// GenericAppearance::update inside their existing cull gates.
		long MultiTransformShape_PositionsOnly (glm::mat4 *shapeToClip, glm::vec3 *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped);

		// Slice 2 (object-offload): per-actor light-data cache without the
		// per-vertex bake side-effects of MultiTransformShape. Uses the
		// texture manager's scene-template path, then broadcasts the returned
		// index into per-leaf GpuStaticPropInstance.lightDataIndex
		// (Recon Section 9 Item 5: all leaves of one multi-shape see identical
		// lightData_, so this is per-actor not per-leaf).
		uint32_t GatherGpuObjectLightDataOnly();

		// Stage 3.C: re-submit the already-populated lightData_ to get a fresh
		// UBO slot index for this frame, without re-gathering from s_listOfLights.
		// Call from touch() on frames where update() is skipped — lightData_ was
		// set by GatherLightsParameters during the last update() run and is valid
		// until shape or registration changes (both trigger invalidation).
		uint32_t ResubmitCachedLightData();

		//This function creates the list of shadows and transforms them in preparation to drawing.
		//
		void MultiTransformShadows (glm::vec3 *pos, glm::mat4 *s2w, float rotation);

		//This function takes the current listOfVisibleFaces and draws them using
		//gos_DrawTriangle.  Does clipping, too!
		void Render (float forceZ = -1.0f, bool isHudElement = false, BYTE alphaValue = 0xff, bool isClamped = false, const glm::mat4* shapeToClip = nullptr, const glm::mat4* shapeToWorld = nullptr);

		//This function takes the current listOfShadowTriangles and draws them using
		//gos_DrawTriangle.  Does clipping, too!
		long RenderShadows (long startFace);

		void SetARGBHighLight (DWORD argb)
		{
			aRGBHighlight = argb;
		}

		// Task 5 (Track B): read-only accessors for recipe building outside GpuStaticPropBatcher.
		DWORD GetARGBHighlight()  const { return aRGBHighlight; }
		DWORD GetFogRGB()         const { return fogRGB; }
		bool  GetLightsOut()      const { return lightsOut; }
		bool  GetIsWindow()       const { return isWindow; }
		bool  GetIsSpotlight()    const { return isSpotlight; }
		// LIGHT-BILLBOARD-GPU-1: read-only access for the GPU static-prop
		// batcher, which needs the beacon/runway light-card classification to
		// set the per-instance camera-facing billboard flag bit. The batcher
		// is a friend of TG_MultiShape but not of TG_Shape, so a public
		// accessor (mirroring GetIsSpotlight above) is the clean seam.
		bool  GetIsLightBillboard() const { return isLightBillboard; }
		// LIGHT-CONE-MODELS-1: read-only type/scalar access for the cone-glow
		// module (mclib/cone_glow.cpp) — needs the spotlight child's immutable
		// type geometry (cone axis derivation + additive re-render) and the
		// instance scale legacy MultiTransformShape applies (pos *= shapeScalar
		// at tgl.cpp:1921). Non-friend caller, hence public accessors.
        const TG_TinyTexture* materialTextures() const {
            if (!myType || myType->GetNodeType()!=SHAPE_NODE) return nullptr;
            return instanceTextures_ ? instanceTextures_ : static_cast<const TG_TypeShape*>(myType)->listOfTextures;
        }
        bool matchesTypeMaterials() const {
            if (!myType || myType->GetNodeType()!=SHAPE_NODE) return false;
            const auto* type=static_cast<const TG_TypeShape*>(myType);
            const auto* slots=materialTextures();
            if (!slots || !type->listOfTextures) return type->numTextures==0;
            for (DWORD i=0;i<type->numTextures;++i)
                if (slots[i].mcTextureNodeIndex!=type->listOfTextures[i].mcTextureNodeIndex ||
                    slots[i].textureAlpha!=type->listOfTextures[i].textureAlpha) return false;
            return true;
        }
		TG_TypeNodePtr GetMyType (void) const { return myType; }
		float GetShapeScalarValue (void) const { return shapeScalar; }
		// 2026-05-10: SHAPE_NODE filter accessor for non-friend callers
		// (BldgAppearance/TreeAppearance::registerStatic). Mirrors the
		// guard used by submitMultiShape at gos_static_prop_batcher.cpp:2047.
		// Returns true only when myType is non-null AND its node-type is
		// SHAPE_NODE (the only kind that has TG_TypeShape geometry).
		bool  IsShapeNode()       const;
		// FOLIAGE-ALPHAFLAG-1: alpha-test (foliage/lattice) classification for
		// non-friend recipe builders (BldgAppearance/TreeAppearance::registerStatic).
		// alphaTestOn lives on the TG_TypeShape behind the protected myType, so
		// this mirrors IsShapeNode's out-of-line seam. This is the SAME source
		// GpuStaticPropBatcher::submitMultiShape already reads for its bit-2
		// (gos_static_prop_batcher.cpp:5575) — the accessor exists so the recipe
		// path can agree with it instead of carrying the retired spotlight meaning.
		bool  GetIsAlphaTest()    const;

		void SetLightsOut (bool lightFlag)
		{
			lightsOut = lightFlag;
		}

		char * getNodeName (void)
		{
			return myType->getNodeId();
		}
		
		glm::vec3 GetRelativeNodeCenter (void)
		{
			return myType->GetRelativeNodeCenter();
		}
		
		bool PerPolySelect (float mouseX, float mouseY);
		
		void SetRecalcShadows (bool flag)
		{
			recalcShadows = flag;
		}
		
		virtual void ScaleShape (float scaleFactor)
		{
			shapeScalar = scaleFactor;
		}
};

using TG_ShapePtr = TG_Shape*;

inline void _TG_ShapeRec::transferFrom (_TG_ShapeRec &rec)
{
	node 				= std::move(rec.node);
	calcedThisFrame		= -1;
	processMe			= true;
	currentAnimation	= nullptr;
	parentNode			= rec.parentNode;
	baseRotation		= rec.baseRotation;
}


inline void _TG_ShapeRec::transferFrom (_TG_ShapeRec &rec, const TG_TinyTexture* textures)
{
    transferFrom(rec);
    if (node) node->instanceTextures_=textures;
}

////////////////////////////////////////////////////////////////////////////////
class TG_RenderShape {
public:
	HGOSBUFFER vb_;
	HGOSBUFFER ib_;
	HGOSVERTEXDECLARATION vdecl_;
	glm::mat4 mvp_; // model -> projection
	glm::mat4 mw_; // model -> world
	float viewport_[4];
    uint32_t light_data_buffer_index_;
    bool isHudElement_;
};
////////////////////////////////////////////////////////////////////////////////

// CPU routing for sidecar override shapes; no GL material is involved.
void TG_SetRenderShapePbrOverride();
void TG_ClearRenderShapePbrOverride();


//Pools are defined here.

// -- Tier-1 instrumentation (stability spec §2) -----------------------------
// First-NULL snapshot captured per pool per frame. Render-thread only; no
// locks. Defined here so all five independent pool classes can embed it.
struct TGL_NullSnapshot {
	const char* caller;                  // __FUNCTION__ of first NULL caller this frame
	const void* shape;                   // optional shape pointer if passed via shape-aware macro
	DWORD       numRequested;            // request count at first NULL
	DWORD       numUsed_at_failure;      // pool high-water at first NULL
	DWORD       totalCapacity;           // pool capacity
};

class TG_VertexPool
{
	protected:
		TG_Vertex 	*tgVertexPool;
		std::unique_ptr<TG_Vertex[]>	tgVertexPoolStore;	// owns the pool; tgVertexPool is a view of it
		TG_Vertex 	*nextVertex;

		DWORD		totalVertices;
		DWORD		numVertices;

	public:
		// Tier-1 instrumentation. All accessed only from the render thread.
		DWORD            nullCountThisFrame    = 0;
		uint64_t         nullCountMonotonic    = 0;
		TGL_NullSnapshot firstNullSnapshot     = {};
		DWORD            peakUsedThisMission   = 0;

		DWORD capacity (void) const { return totalVertices; }

		void recordNull (const char* caller, DWORD numRequested, const void* shape)
		{
			if (nullCountThisFrame == 0)
			{
				firstNullSnapshot.caller             = caller;
				firstNullSnapshot.shape              = shape;
				firstNullSnapshot.numRequested       = numRequested;
				firstNullSnapshot.numUsed_at_failure = numVertices;
				firstNullSnapshot.totalCapacity      = totalVertices;
			}
			nullCountThisFrame++;
			nullCountMonotonic++;
		}
		void resetFrameCounters (void)
		{
			nullCountThisFrame = 0;
			firstNullSnapshot  = {};
			// nullCountMonotonic intentionally NOT reset
		}

		TG_VertexPool (void)
		{
			tgVertexPool = nextVertex = nullptr;
			totalVertices = numVertices = 0;
		}
		
		~TG_VertexPool (void)
		{
			destroy();
		}
		
		void destroy (void)
		{
			tgVertexPoolStore.reset();
			tgVertexPool = nextVertex = nullptr;
			totalVertices = numVertices = 0;
		}
		
		void init (DWORD maxVertices)
		{
			tgVertexPoolStore = std::make_unique_for_overwrite<TG_Vertex[]>(maxVertices);
			tgVertexPool = tgVertexPoolStore.get();
			gosASSERT(tgVertexPool != nullptr);
			
			nextVertex = tgVertexPool;
			
			totalVertices = maxVertices;
			numVertices = 0;
		}
		
		void reset (void)
		{
			nextVertex = tgVertexPool;
			numVertices = 0;
		}
		
		TG_VertexPtr getColorsFromPool (DWORD numRequested, const char* caller = "?", const void* shape = nullptr)
		{
			TG_VertexPtr result = nullptr;
			numVertices += numRequested;
			if (numVertices < totalVertices)
			{
				result = nextVertex;
				nextVertex += numRequested;
			}
			if (result && numVertices > peakUsedThisMission)
				peakUsedThisMission = numVertices;
			if (!result) recordNull(caller, numRequested, shape);
			return result;
		}
};

class TG_GOSVertexPool
{
	protected:
		gos_VERTEX 	*gVertexPool;
		std::unique_ptr<gos_VERTEX[]>	gVertexPoolStore;	// owns the pool; gVertexPool is a view of it
		gos_VERTEX 	*nextVertex;

		DWORD		totalVertices;
		DWORD		numVertices;

	public:
		// Tier-1 instrumentation (stability spec §2).
		DWORD            nullCountThisFrame    = 0;
		uint64_t         nullCountMonotonic    = 0;
		TGL_NullSnapshot firstNullSnapshot     = {};
		DWORD            peakUsedThisMission   = 0;

		DWORD capacity (void) const { return totalVertices; }

		void recordNull (const char* caller, DWORD numRequested, const void* shape)
		{
			if (nullCountThisFrame == 0)
			{
				firstNullSnapshot.caller             = caller;
				firstNullSnapshot.shape              = shape;
				firstNullSnapshot.numRequested       = numRequested;
				firstNullSnapshot.numUsed_at_failure = numVertices;
				firstNullSnapshot.totalCapacity      = totalVertices;
			}
			nullCountThisFrame++;
			nullCountMonotonic++;
		}
		void resetFrameCounters (void)
		{
			nullCountThisFrame = 0;
			firstNullSnapshot  = {};
			// nullCountMonotonic intentionally NOT reset
		}

		TG_GOSVertexPool (void)
		{
			gVertexPool = nextVertex = nullptr;
			totalVertices = numVertices = 0;
		}
		
		~TG_GOSVertexPool (void)
		{
			destroy();
		}
		
		void destroy (void)
		{
			gVertexPoolStore.reset();
			gVertexPool = nextVertex = nullptr;
			totalVertices = numVertices = 0;
		}
		
		void init (DWORD maxVertices)
		{
			gVertexPoolStore = std::make_unique_for_overwrite<gos_VERTEX[]>(maxVertices);
			gVertexPool = gVertexPoolStore.get();
			gosASSERT(gVertexPool != nullptr);
			
			nextVertex = gVertexPool;
			
			totalVertices = maxVertices;
			numVertices = 0;
		}
		
		void reset (void)
		{
			nextVertex = gVertexPool;
			numVertices = 0;
		}
		
		gos_VERTEX * getVerticesFromPool (DWORD numRequested, const char* caller = "?", const void* shape = nullptr)
		{
			gos_VERTEX* result = nullptr;
			numVertices += numRequested;
			if (numVertices < totalVertices)
			{
				result = nextVertex;
				nextVertex += numRequested;
			}
			if (result && numVertices > peakUsedThisMission)
				peakUsedThisMission = numVertices;
			if (!result) recordNull(caller, numRequested, shape);
			return result;
		}
};

class TG_TrianglePool
{
	protected:
		TG_Triangle *trianglePool;
		std::unique_ptr<TG_Triangle[]>	trianglePoolStore;	// owns the pool; trianglePool is a view of it
		TG_Triangle	*nextTriangle;

		DWORD		totalTriangles;
		DWORD		numTriangles;

	public:
		// Tier-1 instrumentation (stability spec §2).
		DWORD            nullCountThisFrame    = 0;
		uint64_t         nullCountMonotonic    = 0;
		TGL_NullSnapshot firstNullSnapshot     = {};
		DWORD            peakUsedThisMission   = 0;

		DWORD capacity (void) const { return totalTriangles; }

		void recordNull (const char* caller, DWORD numRequested, const void* shape)
		{
			if (nullCountThisFrame == 0)
			{
				firstNullSnapshot.caller             = caller;
				firstNullSnapshot.shape              = shape;
				firstNullSnapshot.numRequested       = numRequested;
				firstNullSnapshot.numUsed_at_failure = numTriangles;
				firstNullSnapshot.totalCapacity      = totalTriangles;
			}
			nullCountThisFrame++;
			nullCountMonotonic++;
		}
		void resetFrameCounters (void)
		{
			nullCountThisFrame = 0;
			firstNullSnapshot  = {};
			// nullCountMonotonic intentionally NOT reset
		}

		TG_TrianglePool (void)
		{
			trianglePool = nextTriangle = nullptr;
			totalTriangles = numTriangles = 0;
		}
		
		~TG_TrianglePool (void)
		{
			destroy();
		}
		
		void destroy (void)
		{
			trianglePoolStore.reset();
			trianglePool = nextTriangle = nullptr;
			totalTriangles = numTriangles = 0;
		}
		
		void init (DWORD maxTriangles)
		{
			trianglePoolStore = std::make_unique_for_overwrite<TG_Triangle[]>(maxTriangles);
			trianglePool = trianglePoolStore.get();
			gosASSERT(trianglePool != nullptr);
			
			nextTriangle = trianglePool;
			
			totalTriangles = maxTriangles;
			numTriangles = 0;
		}
		
		void reset (void)
		{
			nextTriangle = trianglePool;
			numTriangles = 0;
		}
		
		TG_Triangle * getTrianglesFromPool (DWORD numRequested, const char* caller = "?", const void* shape = nullptr)
		{
			TG_Triangle* result = nullptr;
			numTriangles += numRequested;
			if (numTriangles < totalTriangles)
			{
				result = nextTriangle;
				nextTriangle += numRequested;
			}
			if (result && numTriangles > peakUsedThisMission)
				peakUsedThisMission = numTriangles;
			if (!result) recordNull(caller, numRequested, shape);
			return result;
		}
};

class TG_ShadowPool
{
	protected:
		TG_ShadowVertexTemp 	*tVertexPool;
		std::unique_ptr<TG_ShadowVertexTemp[]>	tVertexPoolStore;	// owns the pool; tVertexPool is a view of it
		TG_ShadowVertexTemp 	*nextVertex;

		DWORD					totalVertices;
		DWORD					numVertices;

	public:
		// Tier-1 instrumentation (stability spec §2).
		DWORD            nullCountThisFrame    = 0;
		uint64_t         nullCountMonotonic    = 0;
		TGL_NullSnapshot firstNullSnapshot     = {};
		DWORD            peakUsedThisMission   = 0;

		DWORD capacity (void) const { return totalVertices; }

		void recordNull (const char* caller, DWORD numRequested, const void* shape)
		{
			if (nullCountThisFrame == 0)
			{
				firstNullSnapshot.caller             = caller;
				firstNullSnapshot.shape              = shape;
				firstNullSnapshot.numRequested       = numRequested;
				firstNullSnapshot.numUsed_at_failure = numVertices;
				firstNullSnapshot.totalCapacity      = totalVertices;
			}
			nullCountThisFrame++;
			nullCountMonotonic++;
		}
		void resetFrameCounters (void)
		{
			nullCountThisFrame = 0;
			firstNullSnapshot  = {};
			// nullCountMonotonic intentionally NOT reset
		}

		TG_ShadowPool (void)
		{
			tVertexPool = nextVertex = nullptr;
			totalVertices = numVertices = 0;
		}
		
		~TG_ShadowPool (void)
		{
			destroy();
		}
		
		void destroy (void)
		{
			tVertexPoolStore.reset();
			tVertexPool = nextVertex = nullptr;
			totalVertices = numVertices = 0;
		}
		
		void init (DWORD maxVertices)
		{
			tVertexPoolStore = std::make_unique_for_overwrite<TG_ShadowVertexTemp[]>(maxVertices);
			tVertexPool = tVertexPoolStore.get();
			gosASSERT(tVertexPool != nullptr);
			
			nextVertex = tVertexPool;
			
			totalVertices = maxVertices;
			numVertices = 0;
		}
		
		void reset (void)
		{
			nextVertex = tVertexPool;
			numVertices = 0;
		}
		
		TG_ShadowVertexTempPtr getShadowsFromPool (DWORD numRequested, const char* caller = "?", const void* shape = nullptr)
		{
			TG_ShadowVertexTempPtr result = nullptr;
			numVertices += numRequested;
			if (numVertices < totalVertices)
			{
				result = nextVertex;
				nextVertex += numRequested;
			}
			if (result && numVertices > peakUsedThisMission)
				peakUsedThisMission = numVertices;
			if (!result) recordNull(caller, numRequested, shape);
			return result;
		}
};

class TG_DWORDPool
{
	protected:
		DWORD 	*triPool;
		std::unique_ptr<DWORD[]>	triPoolStore;	// owns the pool; triPool is a view of it
		DWORD	*nextTri;

		DWORD	totalTriangles;
		DWORD	numTriangles;

	public:
		// Tier-1 instrumentation (stability spec §2).
		DWORD            nullCountThisFrame    = 0;
		uint64_t         nullCountMonotonic    = 0;
		TGL_NullSnapshot firstNullSnapshot     = {};
		DWORD            peakUsedThisMission   = 0;

		DWORD capacity (void) const { return totalTriangles; }

		void recordNull (const char* caller, DWORD numRequested, const void* shape)
		{
			if (nullCountThisFrame == 0)
			{
				firstNullSnapshot.caller             = caller;
				firstNullSnapshot.shape              = shape;
				firstNullSnapshot.numRequested       = numRequested;
				firstNullSnapshot.numUsed_at_failure = numTriangles;
				firstNullSnapshot.totalCapacity      = totalTriangles;
			}
			nullCountThisFrame++;
			nullCountMonotonic++;
		}
		void resetFrameCounters (void)
		{
			nullCountThisFrame = 0;
			firstNullSnapshot  = {};
			// nullCountMonotonic intentionally NOT reset
		}

		TG_DWORDPool (void)
		{
			triPool = nextTri = nullptr;
			totalTriangles = numTriangles = 0;
		}
		
		~TG_DWORDPool (void)
		{
			destroy();
		}
		
		void destroy (void)
		{
			triPoolStore.reset();
			triPool = nextTri = nullptr;
			totalTriangles = numTriangles = 0;
		}
		
		void init (DWORD maxTriangles)
		{
			triPoolStore = std::make_unique_for_overwrite<DWORD[]>(maxTriangles);
			triPool = triPoolStore.get();
			gosASSERT(triPool != nullptr);
			
			nextTri = triPool;
			
			totalTriangles = maxTriangles;
			numTriangles = 0;
		}
		
		void reset (void)
		{
			nextTri = triPool;
			numTriangles = 0;
		}
		
		DWORD * getFacesFromPool (DWORD numRequested, const char* caller = "?", const void* shape = nullptr)
		{
			DWORD* result = nullptr;
			numTriangles += numRequested;
			if (numTriangles < totalTriangles)
			{
				result = nextTri;
				nextTri += numRequested;
			}
			if (result && numTriangles > peakUsedThisMission)
				peakUsedThisMission = numTriangles;
			if (!result) recordNull(caller, numRequested, shape);
			return result;
		}
};

//-------------------------------------------------------------------------------
extern TG_VertexPool 		*colorPool;
extern TG_GOSVertexPool 	*vertexPool;
extern TG_DWORDPool			*facePool;
extern TG_ShadowPool		*shadowPool;
extern TG_TrianglePool		*trianglePool;

// -- Tier-1 instrumentation macros (stability spec §2.1) --------------------
// Wrap every TGL pool get-call so __FUNCTION__ is captured automatically.
// Shape-aware variant additionally records the calling shape pointer.
#define MC2_TGL_GET_VERTS(pool, n)                  (pool)->getVerticesFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_COLORS(pool, n)                 (pool)->getColorsFromPool((n),  __FUNCTION__)
#define MC2_TGL_GET_FACES(pool, n)                  (pool)->getFacesFromPool((n),   __FUNCTION__)
#define MC2_TGL_GET_SHADOW(pool, n)                 (pool)->getShadowsFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_TRIANGLES(pool, n)              (pool)->getTrianglesFromPool((n), __FUNCTION__)
#define MC2_TGL_GET_VERTS_FOR_SHAPE(pool, n, shape) (pool)->getVerticesFromPool((n), __FUNCTION__, (shape))

// Frame-end drain (env-gated per-frame line + always-on monotonic summary).
// Called from GameOS/gameos/gameosmain.cpp just before swap_window().
void drainTglPoolStats (void);
void drainTglPoolStatsOnShutdown (void);

// Emit unconditional per-mission peak lines for all live pools.
// Call just BEFORE any pool destroy() block at mission teardown.
// context is a short label (e.g. "mission_unload" or "next_mission_start").
void logTglPoolPeaks (const char* context);

//-------------------------------------------------------------------------------
// ASE File Parse String Macros
#define ASE_HEADER				"*3DSMAX_ASCIIEXPORT\t200"
#define ASE_OBJECT				"*GEOMOBJECT {"
#define ASE_HELP_OBJECT			"*HELPEROBJECT {"
#define ASE_MESH				"*MESH {"
#define ASE_NUM_VERTEX			"*MESH_NUMVERTEX"
#define ASE_NUM_FACE			"*MESH_NUMFACES"
#define ASE_VERTEX_LIST			"*MESH_VERTEX_LIST {"
#define ASE_FACE_LIST			"*MESH_FACE_LIST {"
#define ASE_NUM_TVERTEX			"*MESH_NUMTVERTEX"
#define ASE_TVERTLIST			"*MESH_TVERTLIST {"
#define ASE_NUM_TVFACES			"*MESH_NUMTVFACES"
#define ASE_TFACELIST			"*MESH_TFACELIST {"
#define ASE_NUM_CVERTEX			"*MESH_NUMCVERTEX"
#define ASE_CVERTLIST			"*MESH_CVERTLIST {"
#define ASE_NUM_CVFACES			"*MESH_NUMCVFACES"
#define	ASE_CFACELIST			"*MESH_CFACELIST {"
#define ASE_MESH_NORMALS		"*MESH_NORMALS {"

#define ASE_NODE_NAME			"*NODE_NAME"
#define ASE_NODE_PARENT			"*NODE_PARENT"
#define ASE_NODE_POS			"*TM_POS"

#define ASE_ANIMATION			"*TM_ANIMATION {"
#define ASE_ANIM_ROT_HEADER		"*CONTROL_ROT_TRACK {"
#define ASE_ANIM_ROT_SAMPLE		"*CONTROL_ROT_SAMPLE"
#define ASE_ANIM_POS_HEADER		"*CONTROL_POS_TRACK {"
#define ASE_ANIM_POS_SAMPLE		"*CONTROL_POS_SAMPLE"
#define ASE_ANIM_FIRST_FRAME	"*SCENE_FIRSTFRAME"
#define ASE_ANIM_LAST_FRAME		"*SCENE_LASTFRAME"
#define ASE_ANIM_FRAME_SPEED	"*SCENE_FRAMESPEED"
#define ASE_ANIM_TICKS_FRAME	"*SCENE_TICKSPERFRAME"

#define ASE_FACE_NORMAL_ID		"*MESH_FACENORMAL"
#define ASE_VERTEX_NORMAL_ID	"*MESH_VERTEXNORMAL"
#define ASE_MESH_CFACE_ID		"*MESH_CFACE"
#define ASE_MESH_VERTCOL_ID		"*MESH_VERTCOL"
#define ASE_MESH_TFACE_ID		"*MESH_TFACE"
#define ASE_MESH_TVERT_ID		"*MESH_TVERT"
#define ASE_MESH_FACE_ID		"*MESH_FACE"
#define ASE_MESH_VERTEX_ID		"*MESH_VERTEX"

#define ASE_MATERIAL_COUNT		"*MATERIAL_COUNT"
#define ASE_SUBMATERIAL_COUNT	"*NUMSUBMTLS"
#define ASE_MATERIAL_ID			"*MATERIAL"
#define ASE_MATERIAL_BITMAP_ID	"*BITMAP "
#define ASE_MATERIAL_CLASS		"*MATERIAL_CLASS"
#define ASE_MATERIAL_TWOSIDED	"*MATERIAL_TWOSIDED"
#define ASE_FACE_MATERIAL_ID	"*MESH_MTLID"

//-------------------------------------------------------------------------------
#endif
