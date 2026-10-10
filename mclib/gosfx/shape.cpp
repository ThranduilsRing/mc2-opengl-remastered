#include"gosfxheaders.hpp"

// MC2_VFX_ORACLE_SHAPE slice (default OFF). GPU mesh-effect substrate for the
// gosFX::Shape class only. Harvest the rigid model-space MLRShape + per-instance
// transform/scale/color/texture/alpha and submit through the persistent-mesh
// VFX bridge instead of MLRClipper::DrawScalableShape.
#include "particles/batcher.h"
#include <mlr/mlrindexedprimitivebase.hpp>
#include <mlr/mlrtexturepool.hpp>
#include <mlr/mlrtexture.hpp>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <cstdio>

// VFX mesh bridge ABI (implemented in GameOS/gameos/gos_vfx_mesh_bridge.cpp).
// Mirrors the struct layout in gos_vfx_mesh_bridge.h; redeclared here so this
// TU never #includes a GameOS/GL header (one-way dependency, same rule the
// particle batcher follows).
struct GosVfxMeshUpload {
    uint64_t        meshId;
    const float*    positions;
    const float*    uvs;
    const uint16_t* indices;
    uint32_t        vertexCount;
    uint32_t        indexCount;
};
struct GosVfxMeshInstance {
    uint64_t meshId;
    float    modelToWorld[16];
    float    scale;
    float    rgba[4];
    uint32_t gosTexHandle;
    int      blendMode;
};
extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload*   uploads,
                                   unsigned int              numUploads,
                                   const GosVfxMeshInstance* instances,
                                   unsigned int              numInstances);

namespace {

// Set of MLRShape ids already uploaded to the GPU this process. Mirrors the
// bridge cache so we only attach the heavy geometry payload on first sighting
// (the bridge ignores the payload on a cache hit, but skipping it here avoids
// the per-frame harvest of static model verts/indices too).
std::unordered_set<uint64_t> g_uploadedShapeIds;

// Harvest counters (process lifetime; dumped under MC2_VFX_ORACLE_SHAPE_LOG).
unsigned long long g_shp_submitted    = 0;  // shapes routed to the bridge
unsigned long long g_shp_fallback     = 0;  // shapes that fell back to legacy
unsigned long long g_shp_unsupported  = 0;  // fallback: non-indexed primitive
unsigned long long g_shp_missingMesh  = 0;  // fallback: no shape / empty geometry
unsigned long long g_shp_missingMat   = 0;  // primitive had no resolvable texture
unsigned long long g_shp_drawCalls    = 0;  // bridge flush calls issued
bool g_shp_firstHarvest = false;

// Resolve an MLR pool index to a GOS texture handle (same path as
// Batcher::ResolveTextures). Returns 0 if unresolvable (-> untextured white).
uint32_t resolveMlrToGos(unsigned mlrIndex) {
    if (mlrIndex == 0) return 0u;
    if (!MidLevelRenderer::MLRTexturePool::Instance) return 0u;
    MidLevelRenderer::MLRTexture* tex =
        (*MidLevelRenderer::MLRTexturePool::Instance)[static_cast<int>(mlrIndex)];
    if (!tex) return 0u;
    MidLevelRenderer::GOSImage* img = tex->GetImage();
    if (!img) return 0u;
    return static_cast<uint32_t>(img->GetHandle());
}

}  // namespace

//==========================================================================//
// File:	 gosFX_Shape.cpp											    //
// Contents: Base gosFX::Shape Component									//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//############################################################################
//########################  gosFX::Shape__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::Shape__Specification::Shape__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	Singleton__Specification(gosFX::ShapeClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Verify(m_class == ShapeClassID);
	Verify(gos_GetCurrentHeap() == Heap);

	//
	//---------------
	// Load the shape
	//---------------
	//
	m_shape =
		MidLevelRenderer::MLRShape::Make(
			stream,
			MidLevelRenderer::ReadMLRVersion(stream)
		);
	Register_Object(m_shape);
	*stream >> m_radius;
}

//------------------------------------------------------------------------------
//
gosFX::Shape__Specification::Shape__Specification(
	MidLevelRenderer::MLRShape *shape
):
	Singleton__Specification(gosFX::ShapeClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_shape = nullptr;
	SetShape(shape);
}

//------------------------------------------------------------------------------
//
gosFX::Shape__Specification::~Shape__Specification()
{
	Check_Object(this);
	if (m_shape)
	{
		Check_Object(m_shape);
		m_shape->DetachReference();
	}
}

//------------------------------------------------------------------------------
//
gosFX::Shape__Specification*
	gosFX::Shape__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	Shape__Specification *spec =
		new gosFX::Shape__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Shape__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	Singleton__Specification::Save(stream);
	MidLevelRenderer::WriteMLRVersion(stream);
	m_shape->Save(stream);
	*stream << m_radius;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Shape__Specification::Copy(Shape__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	Singleton__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_radius = spec->m_radius;
	m_shape = spec->m_shape;
	gos_PopCurrentHeap();

	Check_Object(m_shape);
	m_shape->AttachReference();
}

//------------------------------------------------------------------------------
//
void
	gosFX::Shape__Specification::SetShape(MidLevelRenderer::MLRShape *shape)
{
	Check_Object(this);

	//
	//------------------------------------
	// Detach the old shape if it is there
	//------------------------------------
	//
	if (m_shape)
	{
		Check_Object(m_shape);
		m_shape->DetachReference();
	}

	//
	//------------------------------------
	// Attach the new shape if it is there
	//------------------------------------
	//
	if (shape)
	{
		Check_Object(shape);
		m_shape = shape;
		m_shape->AttachReference();

		//
		//-----------------------------------------------------------------
		// Get the radius of the bounding sphere.  This will be the largest
		// distance any point is from the origin
		//-----------------------------------------------------------------
		//
		m_radius = 0.0f;
		int count = m_shape->GetNum();
		for (int i=0; i<count; ++i)
		{
			MidLevelRenderer::MLRPrimitiveBase *primitive = m_shape->Find(i);
			Check_Object(primitive);
			Stuff::Point3D *points;
			int vertex_count;
			primitive->GetCoordData(&points, &vertex_count);
			for (int v=0; v<vertex_count; ++v)
			{
				Stuff::Scalar len = points[v].GetLengthSquared();
				if (len > m_radius)
					m_radius = len;
			}
		}
		m_radius = Stuff::Sqrt(m_radius);
	}
}

//############################################################################
//##############################  gosFX::Shape  ################################
//############################################################################

gosFX::Shape::ClassData*
	gosFX::Shape::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::Shape::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			ShapeClassID,
			"gosFX::Shape",
			Singleton::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Shape::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::Shape::Shape(
	Specification *spec,
	unsigned flags
):
	Singleton(DefaultData, spec, flags)
{
	Verify(gos_GetCurrentHeap() == Heap);
	m_radius = spec->m_radius;
}

//------------------------------------------------------------------------------
//
gosFX::Shape*
	gosFX::Shape::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	Shape *cloud = new gosFX::Shape(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void gosFX::Shape::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);
	Check_Object(info->m_parentToWorld);

	//
	//----------------------------
	// Set up the common draw info
	//----------------------------
	//
	MidLevelRenderer::DrawScalableShapeInformation dinfo;
	MidLevelRenderer::MLRShape *shape = GetSpecification()->m_shape;
	dinfo.clippingFlags.SetClippingState(0x3f);
	dinfo.worldToShape = nullptr;
	Specification *spec = GetSpecification();
	Check_Object(spec);
	dinfo.state.Combine(info->m_state, spec->m_state);
	dinfo.activeLights = nullptr;
	dinfo.nrOfActiveLights = 0;
	dinfo.shape = shape;
	Stuff::Vector3D scale(m_scale, m_scale, m_scale);
	dinfo.scaling = &scale;
	dinfo.paintMe = &m_color;
	Stuff::LinearMatrix4D local_to_world;
	local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
	dinfo.shapeToWorld = &local_to_world;

	//
	//--------------------------------------------------------------
	// Check the orientation mode.  The first case is XY orientation
	//--------------------------------------------------------------
	//
	if (spec->m_alignZUsingX)
	{
		Stuff::Point3D
			camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
		Stuff::Point3D card_in_world(local_to_world);
		Stuff::Vector3D look_at;
		look_at.Subtract(camera_in_world, card_in_world);
		if (spec->m_alignZUsingY)
			local_to_world.AlignLocalAxisToWorldVector(
				look_at,
				Stuff::Z_Axis,
				Stuff::Y_Axis,
				Stuff::X_Axis
			);
		else
			local_to_world.AlignLocalAxisToWorldVector(
				look_at,
				Stuff::Z_Axis,
				Stuff::X_Axis,
				-1
			);
	}

	//
	//-------------------------------------------------------
	// Each matrix needs to be aligned to the camera around Y
	//-------------------------------------------------------
	//
	else if (spec->m_alignZUsingY)
	{
		Stuff::Point3D
			camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
		Stuff::Point3D card_in_world(local_to_world);
		Stuff::Vector3D look_at;
		look_at.Subtract(camera_in_world, card_in_world);
		local_to_world.AlignLocalAxisToWorldVector(
			look_at,
			Stuff::Z_Axis,
			Stuff::Y_Axis,
			-1
		);
	}

	//
	//------------------------------------------------------------------
	// MC2_VFX_ORACLE_SHAPE slice (default OFF): route this Shape through
	// the persistent-mesh GPU bridge instead of DrawScalableShape. The
	// final local_to_world (with any camera-alignment already baked above),
	// m_scale, and m_color are reused. On any unsupported case we fall
	// through to the legacy DrawScalableShape below (routed=false).
	//------------------------------------------------------------------
	//
	bool routed = false;
	if (mc2::particles::Batcher::is_oracle_shape_enabled() && shape)
	{
		const bool logOn = mc2::particles::Batcher::is_oracle_shape_log_enabled();

		// Classifier: support only shapes whose primitives are ALL indexed
		// (clean triangle lists). A non-indexed primitive (e.g. raw strip /
		// non-mesh) -> unsupported -> legacy fallback for the whole shape so
		// MLR sort/clip semantics are preserved for that case.
		int numPrim = shape->GetNum();
		bool supported = (numPrim > 0);
		for (int p = 0; p < numPrim && supported; ++p)
		{
			MidLevelRenderer::MLRPrimitiveBase *prim = shape->Find(p);
			if (!prim ||
				!prim->IsDerivedFrom(MidLevelRenderer::MLRIndexedPrimitiveBase::DefaultData))
			{
				supported = false;
			}
		}

		if (!supported)
		{
			++g_shp_unsupported;
		}
		else
		{
			// Build the GL column-vector model->world matrix from the Stuff
			// row-vector LinearMatrix4D (world_c = Σ_r local_r*M(r,c)+M(3,c)).
			// Column-major m[col*4+row]: m[k*4+c]=M(k,c). Row 3 = (0,0,0,1).
			float m2w[16];
			for (int c = 0; c < 4; ++c)
				for (int k = 0; k < 4; ++k)
					m2w[k*4 + c] = 0.0f;
			for (int c = 0; c < 3; ++c)
			{
				for (int k = 0; k < 3; ++k)
					m2w[k*4 + c] = (float)local_to_world(k, c);
				m2w[3*4 + c] = (float)local_to_world(3, c);  // translation
			}
			m2w[3*4 + 3] = 1.0f;

			const uint64_t meshId = (uint64_t)(uintptr_t)shape;
			const bool needUpload = (g_uploadedShapeIds.find(meshId) ==
									 g_uploadedShapeIds.end());

			// Harvest geometry only on first sighting (upload-once cache).
			std::vector<float>    pos;     // x,y,z per vertex
			std::vector<float>    uv;      // u,v per vertex
			std::vector<uint16_t> idx;     // triangle-list indices (re-based)
			uint32_t firstTexHandle = 0;   // GOS handle of first textured prim
			int      blendMode       = 0;  // 0=alpha, 1=additive (first prim)
			bool     gotMaterial     = false;
			bool     harvestOk       = true;

			// Even on a cache hit we must derive the per-instance material
			// (texture handle + blend mode) from the primitive state, so walk
			// the primitives regardless; geometry vectors fill only on upload.
			for (int p = 0; p < numPrim && harvestOk; ++p)
			{
				MidLevelRenderer::MLRIndexedPrimitiveBase *prim =
					static_cast<MidLevelRenderer::MLRIndexedPrimitiveBase*>(shape->Find(p));

				// Material from the per-primitive current state.
				const MidLevelRenderer::MLRState &st = prim->GetCurrentState();
				unsigned mlrTex = st.GetTextureHandle();
				MidLevelRenderer::MLRState::AlphaMode am = st.GetAlphaMode();
				int thisBlend = (am == MidLevelRenderer::MLRState::OneOneMode) ? 1 : 0;
				if (!gotMaterial)
				{
					uint32_t gosH = resolveMlrToGos(mlrTex);
					firstTexHandle = gosH;
					blendMode      = thisBlend;
					gotMaterial    = true;
					if (mlrTex != 0 && gosH == 0) ++g_shp_missingMat;
				}

				if (needUpload)
				{
					Stuff::Point3D   *coords = nullptr; int nCoords = 0;
					MidLevelRenderer::Vector2DScalar *texc = nullptr; int nTex = 0;
					unsigned short   *indices = nullptr; int nIdx   = 0;
					prim->GetCoordData(&coords, &nCoords);
					prim->GetTexCoordData(&texc, &nTex);
					prim->GetIndexData(&indices, &nIdx);
					if (!coords || nCoords <= 0 || !indices || nIdx <= 0)
					{
						harvestOk = false;
						break;
					}
					const uint16_t base = (uint16_t)(pos.size() / 3);
					for (int v = 0; v < nCoords; ++v)
					{
						pos.push_back((float)coords[v].x);
						pos.push_back((float)coords[v].y);
						pos.push_back((float)coords[v].z);
						if (texc && v < nTex)
						{
							uv.push_back((float)texc[v][0]);
							uv.push_back((float)texc[v][1]);
						}
						else { uv.push_back(0.0f); uv.push_back(0.0f); }
					}
					for (int e = 0; e < nIdx; ++e)
						idx.push_back((uint16_t)(base + indices[e]));
				}
			}

			if (harvestOk && (!needUpload || (!pos.empty() && !idx.empty())))
			{
				GosVfxMeshUpload up;
				up.meshId      = meshId;
				up.positions   = needUpload ? pos.data() : nullptr;
				up.uvs         = needUpload ? uv.data()  : nullptr;
				up.indices     = needUpload ? idx.data() : nullptr;
				up.vertexCount = needUpload ? (uint32_t)(pos.size()/3) : 0u;
				up.indexCount  = needUpload ? (uint32_t)idx.size()     : 0u;

				GosVfxMeshInstance inst;
				inst.meshId = meshId;
				for (int e = 0; e < 16; ++e) inst.modelToWorld[e] = m2w[e];
				inst.scale        = (float)m_scale;
				inst.rgba[0]      = (float)m_color.red;
				inst.rgba[1]      = (float)m_color.green;
				inst.rgba[2]      = (float)m_color.blue;
				inst.rgba[3]      = (float)m_color.alpha;
				inst.gosTexHandle = firstTexHandle;
				inst.blendMode    = blendMode;

				gos_vfx_mesh_flush(needUpload ? &up : nullptr, needUpload ? 1u : 0u,
								   &inst, 1u);
				if (needUpload) g_uploadedShapeIds.insert(meshId);
				++g_shp_submitted;
				++g_shp_drawCalls;
				routed = true;

				if (logOn && !g_shp_firstHarvest)
				{
					g_shp_firstHarvest = true;
					std::fprintf(stderr,
						"[VFX_MESH v1] event=FIRST_HARVEST(shape) meshId=%llu prims=%d "
						"verts=%u indices=%u tex=%u blend=%s scale=%.3f\n",
						(unsigned long long)meshId, numPrim,
						up.vertexCount, up.indexCount, firstTexHandle,
						blendMode == 1 ? "additive" : "alpha", (double)m_scale);
					std::fflush(stderr);
				}
			}
			else
			{
				++g_shp_missingMesh;
			}
		}
	}

	//
	//----------------------------
	// Let our parent do its thing
	//----------------------------
	//
	if (!routed)
	{
		if (mc2::particles::Batcher::is_oracle_shape_enabled())
			++g_shp_fallback;
		info->m_clipper->DrawScalableShape(&dinfo);
	}
	Singleton::Draw(info);

	// MC2_VFX_ORACLE_SHAPE_LOG: 240-call summary (mirror the tube/batcher idiom).
	if (mc2::particles::Batcher::is_oracle_shape_log_enabled())
	{
		static unsigned long long s_calls = 0;
		if ((++s_calls % 240ull) == 0ull)
		{
			std::fprintf(stderr,
				"[VFX_MESH v1] event=summary(shape) calls=%llu submitted=%llu "
				"fallback=%llu unsupported=%llu missingMesh=%llu missingMat=%llu "
				"drawCalls=%llu meshesCached=%u\n",
				s_calls, g_shp_submitted, g_shp_fallback, g_shp_unsupported,
				g_shp_missingMesh, g_shp_missingMat, g_shp_drawCalls,
				(unsigned)g_uploadedShapeIds.size());
			std::fflush(stderr);
		}
	}
}

//------------------------------------------------------------------------------
//
void
	gosFX::Shape::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
