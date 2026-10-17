#include"gosfxheaders.hpp"
#include<mlr/mlrindexedtrianglecloud.hpp>
#include"fx_trace/fx_cost_split.h"  // Q2-S0 FX cost-split (env MC2_FX_COST_SPLIT)
#include <tracy/Tracy.hpp>          // Q2-S0 coarse CPU zone
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

// B1 Stage 2' C8: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"
#include"particles/spawn.h"

#include<cstdio>   // VFX-TUBE-PROFILE-ORACLE-1 diagnostics
#include<cstdlib>
#include<map>      // MC2_TUBE_PROFILE_LOG per-spec peak tracking
#include<string>
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "math/stuff_glm_bridge.h"

// MC2_VFX_ORACLE_TUBE slice 1: ribbon mesh submit bridge (GameOS-side GL).
// Declared extern "C" here to avoid pulling GameOS headers into mclib.
// Immediate flush (kept intact; used only if a caller bypasses the deferred path).
extern "C" void gos_tube_ribbon_flush(const float*          positions,
                                      const float*          colors,
                                      const float*          uvs,
                                      unsigned int          numVerts,
                                      const unsigned short* indices,
                                      unsigned int          numIndices,
                                      unsigned int          gosHandle,
                                      int                   blendMode);
// TUBE-DEFERRED-FLUSH-1: enqueue for post-renderLists composite.
// gosHandle==0 must NOT be enqueued (caller must leave ribbonSubmitted=false).
extern "C" void gos_tube_ribbon_enqueue(const float*          positions,
                                        const float*          colors,
                                        const float*          uvs,
                                        unsigned int          numVerts,
                                        const unsigned short* indices,
                                        unsigned int          numIndices,
                                        unsigned int          gosHandle,
                                        int                   blendMode);

//==========================================================================//
// File:	 gosFX_Tube.cpp										            //
// Contents: Base gosFX::Tube Component									    //
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
//############################################################################
//########################  gosFX::Tube__Specification  #############################
//############################################################################

//------------------------------------------------------------------------------
//
gosFX::Tube__Specification::Tube__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	Effect__Specification(TubeClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(gos_GetCurrentHeap() == Heap);

	//
	//-------------------
	// Load in the curves
	//-------------------
	//
	m_profilesPerSecond.Load(stream, gfx_version);
	m_pLifeSpan.Load(stream, gfx_version);
	m_emitterSizeX.Load(stream, gfx_version);
	m_emitterSizeY.Load(stream, gfx_version);
	m_emitterSizeZ.Load(stream, gfx_version);
	m_minimumDeviation.Load(stream, gfx_version);
	m_maximumDeviation.Load(stream, gfx_version);
	m_pDisplacement.Load(stream, gfx_version);
	if (gfx_version < 12)
	{
		m_pScale.m_ageCurve.SetCurve(1.0f);
		m_pScale.m_seeded = false;
		m_pScale.m_seedCurve.SetCurve(1.0f);
	}
	else
		m_pScale.Load(stream, gfx_version);
	m_pRed.Load(stream, gfx_version);
	m_pGreen.Load(stream, gfx_version);
	m_pBlue.Load(stream, gfx_version);
	m_pAlpha.Load(stream, gfx_version);
	m_pUOffset.Load(stream, gfx_version);
	m_pVOffset.Load(stream, gfx_version);
	m_pUSize.Load(stream, gfx_version);
	m_pVSize.Load(stream, gfx_version);

	int type;
	*stream >> m_maxProfileCount >> type >> m_insideOut;
	m_profileType = static_cast<ProfileType>(type);
	CalculateUBias(true);
	BuildTemplate();
}

//------------------------------------------------------------------------------
//
gosFX::Tube__Specification::Tube__Specification():
	Effect__Specification(TubeClassID)
{
	Verify(gos_GetCurrentHeap() == Heap);
	m_maxProfileCount = 0;
	m_profileType = e_Ribbon;
	m_insideOut = false;
	m_UBias = 0.0f;
	Check_Pointer(this);
}

//------------------------------------------------------------------------------
//
gosFX::Tube__Specification*
	gosFX::Tube__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	Tube__Specification *spec =
		new gosFX::Tube__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	Effect__Specification::Save(stream);

	//
	//----------------
	// Save our curves
	//----------------
	//
	m_profilesPerSecond.Save(stream);
	m_pLifeSpan.Save(stream);
	m_emitterSizeX.Save(stream);
	m_emitterSizeY.Save(stream);
	m_emitterSizeZ.Save(stream);
	m_minimumDeviation.Save(stream);
	m_maximumDeviation.Save(stream);
	m_pDisplacement.Save(stream);
	m_pScale.Save(stream);
	m_pRed.Save(stream);
	m_pGreen.Save(stream);
	m_pBlue.Save(stream);
	m_pAlpha.Save(stream);
	m_pUOffset.Save(stream);
	m_pVOffset.Save(stream);
	m_pUSize.Save(stream);
	m_pVSize.Save(stream);

	*stream << m_maxProfileCount << static_cast<int>(m_profileType);
	*stream << m_insideOut;
}

//------------------------------------------------------------------------------
//
void 
	gosFX::Tube__Specification::BuildDefaults()
{

	Check_Object(this);
	Effect__Specification::BuildDefaults();

	m_profilesPerSecond.SetCurve(4.0f);
	m_pLifeSpan.SetCurve(1.0f);

	m_emitterSizeX.SetCurve(0.0f);
	m_emitterSizeY.SetCurve(0.0f);
	m_emitterSizeZ.SetCurve(0.0f);

	m_minimumDeviation.SetCurve(0.0f);
	m_maximumDeviation.SetCurve(0.0f);

	m_pDisplacement.m_ageCurve.SetCurve(0.0f);
	m_pDisplacement.m_seeded = false;
	m_pDisplacement.m_seedCurve.SetCurve(1.0f);

	m_pScale.m_ageCurve.SetCurve(1.0f);
	m_pScale.m_seeded = false;
	m_pScale.m_seedCurve.SetCurve(1.0f);

	m_pRed.m_ageCurve.SetCurve(1.0f);
	m_pRed.m_seeded = false;
	m_pRed.m_seedCurve.SetCurve(1.0f);

	m_pGreen.m_ageCurve.SetCurve(1.0f);
	m_pGreen.m_seeded = false;
	m_pGreen.m_seedCurve.SetCurve(1.0f);

	m_pBlue.m_ageCurve.SetCurve(1.0f);
	m_pBlue.m_seeded = false;
	m_pBlue.m_seedCurve.SetCurve(1.0f);

	m_pAlpha.m_ageCurve.SetCurve(1.0f);
	m_pAlpha.m_seeded = false;
	m_pAlpha.m_seedCurve.SetCurve(1.0f);

	m_pUOffset.m_ageCurve.SetCurve(0.0f);
	m_pUOffset.m_seeded = false;
	m_pUOffset.m_seedCurve.SetCurve(1.0f);

	m_pVOffset.m_ageCurve.SetCurve(0.0f);
	m_pVOffset.m_seeded = false;
	m_pVOffset.m_seedCurve.SetCurve(1.0f);

	m_pUSize.m_ageCurve.SetCurve(1.0f);
	m_pUSize.m_seeded = false;
	m_pUSize.m_seedCurve.SetCurve(1.0f);

	m_pVSize.m_ageCurve.SetCurve(1.0f);
	m_pVSize.m_seeded = false;
	m_pVSize.m_seedCurve.SetCurve(1.0f);

	m_maxProfileCount = 2;
	m_profileType = e_Triangle;
	m_insideOut = false;
	CalculateUBias(false);
	BuildTemplate();
}


//------------------------------------------------------------------------------
//
bool gosFX::Tube__Specification::IsDataValid(bool fix_data)
{

    Check_Object(this);
    if(m_maxProfileCount<2) {
        if(fix_data)
        {
            m_maxProfileCount=2;
            PAUSE(("Warning: Value \"maxProfileCount\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

        }
        else
            return false;
    }

    Stuff::Scalar min,max;
    m_pScale.ExpensiveComputeRange(&min,&max);
    if(min<0.0f) {
        if(fix_data)
        {
            m_pScale.m_ageCurve.SetCurve(1.0f);
            m_pScale.m_seeded = false;
            m_pScale.m_seedCurve.SetCurve(1.0f);
            PAUSE(("Warning: Curve \"pScale\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

        }
        else
            return false;
    }

    Stuff::Scalar max_offset, min_offset;
    Stuff::Scalar max_scale, min_scale;
    m_pUSize.ExpensiveComputeRange(&min_scale, &max_scale);
    Stuff::Scalar lower = min_scale;
    if (lower > 0.0f)
        lower = 0.0f;
    Stuff::Scalar upper = max_scale;

    //
    //------------------------------------
    // Calculate the worst case UV offsets
    //------------------------------------
    //
    m_pVOffset.ExpensiveComputeRange(&min_offset, &max_offset);
    lower += min_offset;
    upper += max_offset;

    if (upper > 99.0f || lower < -99.0f)
    {
        if(fix_data)
        {
            m_pVOffset.m_ageCurve.SetCurve(0.0f);
            m_pVOffset.m_seeded = false;
            m_pVOffset.m_seedCurve.SetCurve(1.0f);
            PAUSE(("Warning: Curve \"VOffset\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

        }
        else
            return false;
    }

    m_pVSize.ExpensiveComputeRange(&min_scale, &max_scale);
    lower = min_scale;
    if (lower > 0.0f)
        lower = 0.0f;
    upper = max_scale;

    //
    //------------------------------------
    // Calculate the worst case UV offsets
    //------------------------------------
    //
    //max_offset, min_offset;
    m_pUOffset.ExpensiveComputeRange(&min_offset, &max_offset);
    lower += min_offset;
    upper += max_offset;

    if (upper > 99.0f || lower < -99.0f)
    {
        if(fix_data)
        {
            m_pUOffset.m_ageCurve.SetCurve(0.0f);
            m_pUOffset.m_seeded = false;
            m_pUOffset.m_seedCurve.SetCurve(1.0f);
            PAUSE(("Warning: Curve \"UOffset\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

        }
        else
            return false;
    }


    return Effect__Specification::IsDataValid(fix_data);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube__Specification::Copy(Tube__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	Effect__Specification::Copy(spec);

	//
	//----------------
	// Copy the curves
	//----------------
	//
	gos_PushCurrentHeap(Heap);
	m_profilesPerSecond = spec->m_profilesPerSecond;
	m_pLifeSpan = spec->m_pLifeSpan;
	m_emitterSizeX = spec->m_emitterSizeX;
	m_emitterSizeY = spec->m_emitterSizeY;
	m_emitterSizeZ = spec->m_emitterSizeZ;
	m_minimumDeviation = spec->m_minimumDeviation;
	m_maximumDeviation = spec->m_maximumDeviation;
	m_pDisplacement = spec->m_pDisplacement;
	m_pScale = spec->m_pScale;
	m_pRed = spec->m_pRed;
	m_pGreen = spec->m_pGreen;
	m_pBlue = spec->m_pBlue;
	m_pAlpha = spec->m_pAlpha;
	m_pUOffset = spec->m_pUOffset;
	m_pVOffset = spec->m_pVOffset;
	m_pUSize = spec->m_pUSize;
	m_pVSize = spec->m_pVSize;
	m_maxProfileCount = spec->m_maxProfileCount;
	m_profileType = spec->m_profileType;
	m_vertices = spec->m_vertices;
	m_uvs = spec->m_uvs;
	m_insideOut = spec->m_insideOut;
	m_UBias = spec->m_UBias;
	gos_PopCurrentHeap();
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube__Specification::BuildTemplate()
{
	Check_Object(this);

	switch (m_profileType)
	{
	case e_Ribbon:
	case e_AlignedRibbon:
		m_vertices.SetLength(2);
		m_vertices[0] = Stuff::Vector3D(1.0f, 0.0f, 0.0f);
		m_vertices[1] = Stuff::Vector3D(-1.0f, 0.0f, 0.0f);
		m_uvs.SetLength(2);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;

	case e_VerticalRibbon:
		m_vertices.SetLength(2);
		m_vertices[0] = Stuff::Vector3D(0.0f, 0.0f, -1.0f);
		m_vertices[1] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_uvs.SetLength(2);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;

	case e_Triangle:
		m_vertices.SetLength(4);
		m_vertices[0] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_vertices[1] = Stuff::Vector3D(-0.8660254f, 0.0f, -0.5f);
		m_vertices[2] = Stuff::Vector3D(0.8660254f, 0.0f, -0.5f);
		m_vertices[3] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_uvs.SetLength(4);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f/3.0f);
		m_uvs[2] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 2.0f/3.0f);
		m_uvs[3] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;

	case e_Square:
		m_vertices.SetLength(5);
		m_vertices[0] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_vertices[1] = Stuff::Vector3D(-1.0f, 0.0f, 0.0f);
		m_vertices[2] = Stuff::Vector3D(0.0f, 0.0f, -1.0f);
		m_vertices[3] = Stuff::Vector3D(1.0f, 0.0f, 0.0f);
		m_vertices[4] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_uvs.SetLength(5);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.25f);
		m_uvs[2] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.5f);
		m_uvs[3] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.75f);
		m_uvs[4] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;

	case e_Cross:
		m_vertices.SetLength(5);
		m_vertices[0] = Stuff::Vector3D(0.0f, 0.0f, 0.0f);
		m_vertices[1] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_vertices[2] = Stuff::Vector3D(-1.0f, 0.0f, 0.0f);
		m_vertices[3] = Stuff::Vector3D(0.0f, 0.0f, -1.0f);
		m_vertices[4] = Stuff::Vector3D(1.0f, 0.0f, 0.0f);
		m_uvs.SetLength(5);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		m_uvs[2] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		m_uvs[3] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		m_uvs[4] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;

	case e_Pentagon:
		m_vertices.SetLength(6);
		m_vertices[0] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_vertices[1] = Stuff::Vector3D(-0.9510565f, 0.0f, 0.309017f);
		m_vertices[2] = Stuff::Vector3D(-0.58778525f, 0.0f, -0.8090167f);
		m_vertices[3] = Stuff::Vector3D(0.58778525f, 0.0f, -0.8090167f);
		m_vertices[4] = Stuff::Vector3D(0.9510565f, 0.0f, 0.309017f);
		m_vertices[5] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_uvs.SetLength(6);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.2f);
		m_uvs[2] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.4f);
		m_uvs[3] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.6f);
		m_uvs[4] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.8f);
		m_uvs[5] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;

	case e_Hexagon:
		m_vertices.SetLength(7);
		m_vertices[0] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_vertices[1] = Stuff::Vector3D(-0.8660254f, 0.0f, 0.5f);
		m_vertices[2] = Stuff::Vector3D(-0.8660254f, 0.0f, -0.5f);
		m_vertices[3] = Stuff::Vector3D(0.0f, 0.0f, -1.0f);
		m_vertices[4] = Stuff::Vector3D(0.8660254f, 0.0f, -0.5f);
		m_vertices[5] = Stuff::Vector3D(0.8660254f, 0.0f, 0.5f);
		m_vertices[6] = Stuff::Vector3D(0.0f, 0.0f, 1.0f);
		m_uvs.SetLength(7);
		m_uvs[0] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.0f);
		m_uvs[1] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f/6.0f);
		m_uvs[2] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 2.0f/6.0f);
		m_uvs[3] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 0.5f);
		m_uvs[4] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 4.0f/6.0f);
		m_uvs[5] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 5.0f/6.0f);
		m_uvs[6] = Stuff::Vector2DOf<Stuff::Scalar>(0.0f, 1.0f);
		break;
	}
}

//------------------------------------------------------------------------------
//
bool
	gosFX::Tube__Specification::CalculateUBias(bool adjust)
{
	Check_Object(this);

	//
	//----------------------------------
	// Calculate the worst case UV scale
	//----------------------------------
	//
	int max_index = m_maxProfileCount-1;
	Stuff::Scalar max_scale, min_scale;
Retry:
	m_pUSize.ExpensiveComputeRange(&min_scale, &max_scale);
	Stuff::Scalar lower = min_scale * max_index;
	if (lower > 0.0f)
		lower = 0.0f;
	Stuff::Scalar upper = max_scale * max_index;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	Stuff::Scalar max_offset, min_offset;
	m_pUOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;
	if (upper - lower >= 198.0f)
	{
		if (!adjust)
			return false;
		lower = max_offset - min_offset;
		Stuff::Scalar scale = static_cast<Stuff::Scalar>(floor((198.0f-lower)/max_index));
		m_pUSize.m_ageCurve.SetCurve(scale);
		m_pUSize.m_seeded = false;
		goto Retry;
	}
	lower += 99.0f;
	if (lower < 0.0f)
	{
		m_UBias = static_cast<Stuff::Scalar>(floor(0.5f-lower));
		return true;
	}
	upper -= 99.0f;
	if (upper > 0.0f)
	{
		m_UBias = static_cast<Stuff::Scalar>(floor(0.5f-upper));
		return true;
	}
	m_UBias = 0.0f;
	return true;
}

//############################################################################
//############################  gosFX::Tube  ###############################
//############################################################################

gosFX::Tube::ClassData*
	gosFX::Tube::DefaultData = nullptr;

//------------------------------------------------------------------------------
//
void
	gosFX::Tube::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			TubeClassID,
			"gosFX::Tube",
			Effect::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = nullptr;
}

//------------------------------------------------------------------------------
//
gosFX::Tube::Tube(
	Specification *spec,
	unsigned flags
):
	Effect(DefaultData, spec, flags)
{
	Check_Pointer(this);
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	//
	//
	m_profiles.SetLength(spec->m_maxProfileCount);
	unsigned vertex_count = static_cast<unsigned int>(spec->m_vertices.GetLength());
	Verify(vertex_count < 8);
	unsigned index_count = (vertex_count - 1)*6;
	unsigned size =
		sizeof(Stuff::Point3D)
		 + sizeof(Stuff::RGBAColor)
		 + sizeof(Stuff::Vector2DOf<Stuff::Scalar>);
	size *= vertex_count*spec->m_maxProfileCount;
	size += sizeof(unsigned short)*(spec->m_maxProfileCount-1)*index_count;

	//
	//-----------------------
	// Allocate the tube mesh
	//-----------------------
	//
	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	m_mesh =
		new MidLevelRenderer::MLRIndexedTriangleCloud(
			spec->m_maxProfileCount*(vertex_count-1)*2
		);
	Register_Object(m_mesh);
	gos_PopCurrentHeap();

	//
	//------------------------------------------------
	// Set up the data pointers into the channel block
	//------------------------------------------------
	//
	m_triangleCount = 0;
	m_vertexCount = 0;
	m_data.SetLength(size);
	m_P_vertices = Cast_Pointer(Stuff::Point3D*, &m_data[0]);
	size = spec->m_maxProfileCount*vertex_count*sizeof(Stuff::Point3D);
	m_P_colors = Cast_Pointer(Stuff::RGBAColor*, &m_data[size]);
	size += spec->m_maxProfileCount*vertex_count*sizeof(Stuff::RGBAColor);
	m_P_uvs = Cast_Pointer(Stuff::Vector2DOf<Stuff::Scalar>*, &m_data[size]);
	size += spec->m_maxProfileCount*vertex_count*sizeof(Stuff::Vector2DOf<Stuff::Scalar>);
	unsigned short *mesh_indices = Cast_Pointer(unsigned short*, &m_data[size]);
	// MC2_VFX_ORACLE_TUBE: remember the index stencil for the ribbon bridge.
	m_P_indices = mesh_indices;
	m_mesh->SetData(
		&m_triangleCount,
		&m_vertexCount,
		mesh_indices,
		m_P_vertices,
		m_P_colors,
		m_P_uvs
	);
	BuildMesh(mesh_indices);

	//
	//-------------------------------
	// Set up an empty profile cloud
	//-------------------------------
	//
	m_activeProfileCount = 0;
	m_headProfile = -1;
	m_tailProfile = -1;
	m_birthAccumulator = 0.0f;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube::BuildMesh(unsigned short *indices)
{
	Check_Object(this);
	Check_Pointer(indices);

	//
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	unsigned vertex_count = static_cast<unsigned int>(spec->m_vertices.GetLength());
	Verify(vertex_count < 8);

	//
	//-------------------------------------------------
	// Crosses are built different from everything else
	//-------------------------------------------------
	//
	if (spec->m_profileType != Specification::e_Cross)
	{
		for (unsigned short profile=0; profile<spec->m_maxProfileCount-1; ++profile)
		{
			unsigned short base = static_cast<short>(profile * vertex_count);
			for (unsigned short panel=0; panel<vertex_count-1; ++panel)
			{
				if (spec->m_insideOut)
				{
					*indices++ = static_cast<short>(base+panel+1);
					*indices++ = static_cast<short>(base+panel);
					*indices++ = static_cast<short>(base+panel+vertex_count+1);
					*indices++ = static_cast<short>(base+panel+vertex_count);
					*indices++ = static_cast<short>(base+panel+vertex_count+1);
					*indices++ = static_cast<short>(base+panel);
				}
				else
				{
					*indices++ = static_cast<short>(base+panel);
					*indices++ = static_cast<short>(base+panel+1);
					*indices++ = static_cast<short>(base+panel+vertex_count+1);
					*indices++ = static_cast<short>(base+panel+vertex_count+1);
					*indices++ = static_cast<short>(base+panel+vertex_count);
					*indices++ = static_cast<short>(base+panel);
				}
			}
		}
	}
	else
	{
		Verify(vertex_count==5);
		for (unsigned short profile=0; profile<spec->m_maxProfileCount-1; ++profile)
		{
			unsigned short base = static_cast<short>(profile * vertex_count);
			for (unsigned short panel=0; panel<4; ++panel)
			{
				if (spec->m_insideOut)
				{
					*indices++ = static_cast<short>(base+panel+1);
					*indices++ = base;
					*indices++ = static_cast<short>(base+panel+6);
					*indices++ = static_cast<short>(base+5);
					*indices++ = static_cast<short>(base+panel+6);
					*indices++ = base;
				}
				else
				{
					*indices++ = base;
					*indices++ = static_cast<short>(base+panel+1);
					*indices++ = static_cast<short>(base+panel+6);
					*indices++ = static_cast<short>(base+panel+6);
					*indices++ = static_cast<short>(base+5);
					*indices++ = base;
				}
			}
		}
	}
}

//------------------------------------------------------------------------------
//
gosFX::Tube::~Tube()
{
	Unregister_Object(m_mesh);
	delete m_mesh;
}

//------------------------------------------------------------------------------
//
gosFX::Tube*
	gosFX::Tube::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	Tube *tube = new gosFX::Tube(spec, flags);
	gos_PopCurrentHeap();

	return tube;
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	//
	//----------------------
	// Let effect initialize
	//----------------------
	//
	Effect::Start(info);

	// C17 fix 2026-05-21: ALWAYS run legacy per-instance init
	// (m_birthAccumulator = 1.0f). C8 skipped this under env-on by returning
	// right after Spawn — but legacy Tube::Execute still walked the
	// un-initialized birthAccumulator and tube profile state, producing wild
	// writes that corrupted heap adjacent to child gosFX allocations. Same
	// shape as the C9 fix for Point/Shard: legacy init runs always; Spawn
	// emit happens ADDITIONALLY under env-on. Legacy ::Draw is A2-gated.
	//
	//--------------------------------------------------------------------------
	// If the effect is off, we will create two profiles.  If they effect is on,
	// we just keep doing what we do
	//--------------------------------------------------------------------------
	//
	m_birthAccumulator = 1.0f;

	// GPU spawn moved to Draw() so it fires every frame (not just once at Start).
}

//------------------------------------------------------------------------------
//
bool gosFX::Tube::Execute(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	//
	//----------------------------------------
	// If we aren't supposed to execute, don't
	//----------------------------------------
	//
	if (!IsExecuted())
		return false;

	//
	//----------------------------------------------------------------------
	// Update the head of the tube to the current location, then compute the
	// inverse transformation to keep all particles in their place of origin
	//----------------------------------------------------------------------
	//
	Stuff::LinearMatrix4D new_world_to_local;
	Stuff::LinearMatrix4D local_to_world;
	local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
	mc2::math::fromGlm(new_world_to_local, glm::inverse(mc2::math::toGlm(local_to_world)));

	//
	//--------------------------
	// Figure out the birth rate
	//--------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Stuff::Scalar dT =
		static_cast<Stuff::Scalar>(info->m_time - m_lastRan);
	Verify(dT >= 0.0f);
	Stuff::Scalar prev_age = m_age;
	m_age += dT * m_ageRate;
	if (m_age >= 1.0f)
		m_birthAccumulator = 0.0f;
	else
	{
		Stuff::Scalar new_life =
			spec->m_profilesPerSecond.ComputeValue(m_age, m_seed);
		Min_Clamp(new_life, 0.0f);
		m_birthAccumulator += dT * new_life;

		//
		//------------------------------------------------------------------
		// If it is time for a new child and there is room, move the head of
		// the tube and create the new profile, then clear out the integer
		// part of the accumulator - stacking up would look stupid
		//------------------------------------------------------------------
		//
		if (m_birthAccumulator >= 1.0f && m_activeProfileCount < spec->m_maxProfileCount)
		{
			if (!m_activeProfileCount)
			{
				m_headProfile = 1;
				m_tailProfile = spec->m_maxProfileCount-1;
				CreateNewProfile(0, local_to_world);
				m_activeProfileCount = 2;
			}
			else
			{
				m_profiles[m_headProfile].m_profileToWorld = local_to_world;
				if (++m_headProfile == spec->m_maxProfileCount)
					m_headProfile = 0;
			}
			CreateNewProfile(m_headProfile, local_to_world);
			m_birthAccumulator -= static_cast<Stuff::Scalar>(floor(m_birthAccumulator));
		}
		else
			m_profiles[m_headProfile].m_profileToWorld = local_to_world;
	}

	//
	//--------------------------------------------------------------------
	// If we don't have any profiles, just execute the children and return
	//--------------------------------------------------------------------
	//
	if (!m_activeProfileCount)
	{
		m_age = prev_age;
		return Effect::Execute(info);
	}

	//
	//----------------------------------
	// Deal with all the active profiles
	//----------------------------------
	//
	Stuff::ExtentBox box(Stuff::Point3D::Identity, Stuff::Point3D::Identity);
	int i = m_headProfile;
	int profile_count = 0;
	Verify(i >= 0);
	do
	{
		//
		//----------------------------------------------------------------------
		// Age and animate the profile.  If the profile should die, it becomes
		// the end of the tail
		//----------------------------------------------------------------------
		//
		Profile *profile = GetProfile(i);
		Check_Object(profile);
		Verify (profile->m_age < 1.0f);
		profile->m_age += dT*profile->m_ageRate;
		Stuff::Sphere bounds;
		if (!AnimateProfile(i, profile_count, new_world_to_local, info->m_time, &bounds))
		{
			m_tailProfile = i;
			break;
		}

		//
		//---------------------
		// Deal with the bounds
		//---------------------
		//
		if (i == m_headProfile)
		{
			box.maxX = bounds.center.x + bounds.radius;
			box.minX = bounds.center.x - bounds.radius;
			box.maxY = bounds.center.y + bounds.radius;
			box.minY = bounds.center.y - bounds.radius;
			box.maxZ = bounds.center.z + bounds.radius;
			box.minZ = bounds.center.z - bounds.radius;
		}
		else
		{
			Stuff::ExtentBox local_box;
			local_box.maxX = bounds.center.x + bounds.radius;
			local_box.minX = bounds.center.x - bounds.radius;
			local_box.maxY = bounds.center.y + bounds.radius;
			local_box.minY = bounds.center.y - bounds.radius;
			local_box.maxZ = bounds.center.z + bounds.radius;
			local_box.minZ = bounds.center.z - bounds.radius;
			box.Union(box, local_box);
		}

		//
		//---------------------------------------------------------------------------
		// Move to the previous profile and wrap to the end of the list if necessary
		//---------------------------------------------------------------------------
		//
		++profile_count;
		if (--i < 0)
			i = spec->m_maxProfileCount-1;
	} while (i != m_tailProfile);
	m_activeProfileCount = profile_count;

	//
	//-----------------------------------------------
	// Put the age back and run the base effect stuff
	//-----------------------------------------------
	//
	m_age = prev_age;
	if (!Effect::Execute(info))
		return false;

	//
	//--------------------------------------------
	// Now, build a info->m_bounds around this box
	//--------------------------------------------
	//
	Verify(box.maxX >= box.minX);
	Verify(box.maxY >= box.minY);
	Verify(box.maxZ >= box.minZ);
	Stuff::OBB local_bounds = Stuff::OBB::Identity;
	local_bounds.axisExtents.x = 0.5f * (box.maxX - box.minX);
	local_bounds.axisExtents.y = 0.5f * (box.maxY - box.minY);
	local_bounds.axisExtents.z = 0.5f * (box.maxZ - box.minZ);
	local_bounds.localToParent(3,0) = box.minX + local_bounds.axisExtents.x;
	local_bounds.localToParent(3,1) = box.minY + local_bounds.axisExtents.y;
	local_bounds.localToParent(3,2) = box.minZ + local_bounds.axisExtents.z;
	local_bounds.sphereRadius = local_bounds.axisExtents.GetLength();
	Stuff::OBB parent_bounds;
	parent_bounds.Multiply(local_bounds, m_localToParent);
	info->m_bounds->Union(*info->m_bounds, parent_bounds);
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::Tube::Kill()
{
	Check_Object(this);

	//
	//-------------------------------------------------------------
	// Destroy all the profiles and set up an empty profile cloud
	//-------------------------------------------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	if (m_activeProfileCount>0)
	{
		do
		{
			DestroyProfile(m_headProfile--);
			if (m_headProfile<0)
				m_headProfile = spec->m_maxProfileCount-1;
		} while (m_headProfile != m_tailProfile);
	}
	m_activeProfileCount = 0;
	m_headProfile = -1;
	m_tailProfile = -1;
	m_birthAccumulator = 0.0f;

	//
	//----------------------------------------
	// Now let the base effect handle stopping
	//----------------------------------------
	//
	Effect::Kill();
}

//------------------------------------------------------------------------------
//
bool gosFX::Tube::HasFinished()
{
	Check_Object(this);
	return Effect::HasFinished() && (m_activeProfileCount == 0);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube::CreateNewProfile(
		unsigned index,
		const Stuff::LinearMatrix4D &origin
	)
{
	Check_Object(this);
	Check_Object(&origin);

	//
	//----------------------------------------------------
	// Figure out the age and age rate of the new profile
	//----------------------------------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Profile *profile = GetProfile(index);
	Check_Object(profile);
	profile->m_age = 0.0f;
	Stuff::Scalar min_seed =
		spec->m_minimumChildSeed.ComputeValue(m_age, m_seed);
	Stuff::Scalar seed_range =
		spec->m_maximumChildSeed.ComputeValue(m_age, m_seed) - min_seed;
	Stuff::Scalar seed =
		Stuff::Random::GetFraction()*seed_range + min_seed;
	Clamp(seed, 0.0f, 1.0f);
	profile->m_seed = seed;
	Stuff::Scalar lifetime =
		spec->m_pLifeSpan.ComputeValue(m_age, seed);
	Min_Clamp(lifetime, 0.0333333f);
	profile->m_ageRate = 1.0f / lifetime;

	//
	//------------------------------------------------------------------
	// Establish the base position and figure out the direction of drift
	//------------------------------------------------------------------
	//
	profile->m_profileToWorld = origin;
	Stuff::Scalar pitch_min =
		spec->m_minimumDeviation.ComputeValue(m_age, seed);
	Stuff::Scalar pitch_range =
		spec->m_maximumDeviation.ComputeValue(m_age, seed) - pitch_min;
	if (pitch_range < 0.0f)
		pitch_range = 0.0f;
	Stuff::Radian angle = pitch_min + Stuff::Random::GetFraction() * pitch_range;
	Stuff::SinCosPair xy(angle);
	profile->m_direction.x = (Stuff::Random::GetFraction() >= 0.5f) ? xy.sine : -xy.sine;
	profile->m_direction.y = 0.0f;
	profile->m_direction.z = xy.cosine;
	Check_Object(&profile->m_direction);
}

//------------------------------------------------------------------------------
//
bool
	gosFX::Tube::AnimateProfile(
		unsigned index,
		unsigned profile_index,
		const Stuff::LinearMatrix4D &world_to_new_local,
		Stuff::Time till,
		Stuff::Sphere *bounds
	)
{
	Check_Object(this);

	//
	//----------------------------------------------------
	// If the profile gets too old, don't do anything else
	//----------------------------------------------------
	//
	Profile *profile = GetProfile(index);
	Check_Object(profile);
	Stuff::Scalar age = profile->m_age;
	if (age >= 1.0f)
		return false;

	// Q2-S0 instrumentation only. Per-profile ribbon rebuild cost (the swept-
	// quad CPU rebuild we suspect is hot). Profiles are bounded per tube, so a
	// per-profile Tracy zone is coarse (not per-particle). Placed after the
	// dead-profile early-return so only real rebuilds are timed.
	ZoneScopedN("gosFX.Tube.AnimateProfile");
	mc2::fx_cost_split::Scope _fxcs(mc2::fx_cost_split::B_TUBE_ANIMATE_PROFILE);

	//
	//--------------------------------------------------------------------
	// Figure out the scale and displacement of the profile in world space
	//--------------------------------------------------------------------
	//
	Set_Statistic(Profile_Count, Profile_Count+1);
	Stuff::Scalar seed = profile->m_seed;
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Stuff::Scalar scale = spec->m_pScale.ComputeValue(age, seed);
	Verify(scale >= 0.0f);
	Stuff::Scalar disp = spec->m_pDisplacement.ComputeValue(age, seed);
	Stuff::Point3D offset;
	offset.Multiply(profile->m_direction, disp);

	//
	//-------------------------------------------------
	// Now build the template to new local space matrix
	//-------------------------------------------------
	//
	Stuff::AffineMatrix4D template_to_profile(true);
	template_to_profile(0,0) = scale;
	template_to_profile(1,1) = scale;
	template_to_profile(2,2) = scale;
	template_to_profile.BuildTranslation(offset);
	Stuff::AffineMatrix4D template_to_world;
	template_to_world.Multiply(template_to_profile, profile->m_profileToWorld);
	Stuff::AffineMatrix4D template_to_new_local;
	template_to_new_local.Multiply(template_to_world, world_to_new_local);
	bounds->center = template_to_new_local;
	bounds->radius = scale;

	//
	//------------------------------------------------------------------------
	// Now we just multiply the template through to the vertices unless we are
	// doing the aligned ribbon
	//------------------------------------------------------------------------
	//
	unsigned i;
	unsigned vertex_count = static_cast<unsigned int>(spec->m_vertices.GetLength());
	Verify(vertex_count < 8);
	unsigned vertex_index = profile_index * vertex_count;
	if (spec->m_profileType != Specification::e_AlignedRibbon)
	{
		Check_Pointer(m_P_vertices);
		for (i=0; i<vertex_count; ++i)
		{
			m_P_vertices[vertex_index+i].Multiply(
				spec->m_vertices[i],
				template_to_new_local
			);
		}
	}

	//
	//------------------------------
	// Figure out the UV adjustments
	//------------------------------
	//
	Check_Pointer(m_P_uvs);
	Stuff::Scalar u = spec->m_pUOffset.ComputeValue(age, seed);
	Stuff::Scalar v = spec->m_pVOffset.ComputeValue(age, seed);
	Stuff::Scalar u2 = spec->m_pUSize.ComputeValue(age, seed);
	Stuff::Scalar v2 = spec->m_pVSize.ComputeValue(age, seed);
	u += u2*profile_index + spec->m_UBias;
	for (i=0; i<vertex_count; ++i)
	{
		m_P_uvs[vertex_index+i].x = u;
		m_P_uvs[vertex_index+i].y = spec->m_uvs[i].y*v2 + v;
	}

	//
	//---------------------------
	// Lastly, animate the colors
	//---------------------------
	//
	Check_Pointer(m_P_colors);
	Stuff::RGBAColor color;
	color.red = spec->m_pRed.ComputeValue(age, seed);
	color.green = spec->m_pGreen.ComputeValue(age, seed);
	color.blue = spec->m_pBlue.ComputeValue(age, seed);
	color.alpha = spec->m_pAlpha.ComputeValue(age, seed);
	for (i=0; i<vertex_count; ++i)
		m_P_colors[vertex_index+i] = color;
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::Tube::DestroyProfile(unsigned index)
{
	Profile *profile = GetProfile(index);
	Check_Object(profile);
	profile->m_age = 1.0f;
}

//------------------------------------------------------------------------------
//
void gosFX::Tube::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// The GPU particle path is deliberately not used for tubes: a billboard-per-profile oracle
	// produced a "ladder/fence" artifact (one square card per spine profile instead of a continuous
	// ribbon) on missile smoke and PPC trails, so the CPU MLR swept-mesh stays the tube owner.
	// Re-enabling needs a ribbon oracle (one oriented quad per consecutive profile pair), combat
	// validation (mc2_10 missiles, mc2_24 PPC) and a gpuTrailKindProven() promotion for PpcBolt
	// and MissileSmoke. See docs/vfx-originals-lockdown.md.

	//
	//---------------------------------------------------------
	// If we have active particles, set up the draw information
	//---------------------------------------------------------
	//
	if (m_activeProfileCount>1)
	{
		MidLevelRenderer::DrawEffectInformation dInfo;
		dInfo.effect = m_mesh;
		Specification *spec = GetSpecification();

		// MC2_TUBE_PROFILE_LOG=1: log each tube spec's PEAK m_activeProfileCount.
		// Placed BEFORE the oracle/legacy split so the count is path-independent —
		// run oracle ON vs OFF and compare to test whether enabling the oracle
		// changes profile growth (execution interaction) vs pure rendering diff.
		{
			static const bool s_tpOn = mc2::gate::get<"MC2_TUBE_PROFILE_LOG">();
			if (s_tpOn) {
				static std::map<std::string,int> s_maxBySpec;
				const char* nm = (const char*)spec->m_name;
				std::string key = nm ? nm : "?";
				int& mx = s_maxBySpec[key];
				if (m_activeProfileCount > mx) {
					mx = m_activeProfileCount;
					mc2::fmt::print_err("[TUBE_PROFILE] spec={} type={:d} activeMax={:d} maxProf={:d}\n", key.c_str(), mc2::fmt::as_i32((int)spec->m_profileType), mc2::fmt::as_i32(m_activeProfileCount), mc2::fmt::as_i32(spec->m_maxProfileCount));
					std::fflush(stderr);
				}
			}
		}
		Check_Object(spec);
		dInfo.state.Combine(info->m_state, spec->m_state);
		dInfo.clippingFlags = info->m_clippingFlags;
		Stuff::LinearMatrix4D local_to_world;
		local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
		dInfo.effectToWorld = &local_to_world;
		unsigned vertex_count = static_cast<unsigned int>(spec->m_vertices.GetLength());
		m_vertexCount = m_activeProfileCount * vertex_count;
		m_triangleCount = 2 * (m_activeProfileCount-1) * (vertex_count-1);

		//
		//-------------------------------------------------------------------
		// If we are doing the aligned ribbon, we will have to orient each of
		// the profiles and then compute its vertex positions accordingly
		//-------------------------------------------------------------------
		//
		if (spec->m_profileType == Specification::e_AlignedRibbon)
		{
			int i = m_headProfile;
			int vertex = 0;
			unsigned vertex_count = static_cast<unsigned int>(spec->m_vertices.GetLength());
			Verify(vertex_count < 8);
			Stuff::Point3D
				camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
			Stuff::LinearMatrix4D world_to_local;
			mc2::math::fromGlm(world_to_local, glm::inverse(mc2::math::toGlm(local_to_world)));

			//
			//----------------------------------------------------------------
			// go thru all the profiles and figure out where they are in local
			// space
			//----------------------------------------------------------------
			//
			Verify(i >= 0);
			do
			{
				Profile *profile = GetProfile(i);
				Check_Object(profile);
				Stuff::Point3D camera_in_profile;
				camera_in_profile = mc2::math::toPoint3D(mc2::math::transformPoint(mc2::math::toGlm(camera_in_world), glm::inverse(mc2::math::toGlm(profile->m_profileToWorld))));

				//
				//---------------------------------------------------------
				// Figure out the scale and displacement of the template in
				// profile space
				//---------------------------------------------------------
				//
				Stuff::Scalar age = profile->m_age;
				Stuff::Scalar seed = profile->m_seed;
				Stuff::Scalar scale = spec->m_pScale.ComputeValue(age, seed);
				Stuff::Scalar disp = spec->m_pDisplacement.ComputeValue(age, seed);
				Stuff::Point3D offset_in_profile;
				offset_in_profile.Multiply(profile->m_direction, disp);

				//
				//----------------------------------------------------------
				// Figure out the direction that we want the profile to face
				// and build a rotation vector that does it
				//----------------------------------------------------------
				//
				Stuff::Vector3D direction;
				direction.Subtract(camera_in_profile, offset_in_profile);
				Stuff::LinearMatrix4D template_rotation(true);
				template_rotation.AlignLocalAxisToWorldVector(
					direction,
					Stuff::Z_Axis,
					Stuff::Y_Axis,
					-1
				);

				//
				//-------------------------------------------------
				// Now build the template to new local space matrix
				//-------------------------------------------------
				//
				Stuff::AffineMatrix4D template_to_profile;
				template_to_profile(0,0) = scale*template_rotation(0,0);
				template_to_profile(0,1) = scale*template_rotation(0,1);
				template_to_profile(0,2) = scale*template_rotation(0,2);
				template_to_profile(1,0) = scale*template_rotation(1,0);
				template_to_profile(1,1) = scale*template_rotation(1,1);
				template_to_profile(1,2) = scale*template_rotation(1,2);
				template_to_profile(2,0) = scale*template_rotation(2,0);
				template_to_profile(2,1) = scale*template_rotation(2,1);
				template_to_profile(2,2) = scale*template_rotation(2,2);
				template_to_profile.BuildTranslation(offset_in_profile);
				Stuff::AffineMatrix4D template_to_world;
				template_to_world.Multiply(template_to_profile, profile->m_profileToWorld);
				Stuff::AffineMatrix4D template_to_local;
				template_to_local.Multiply(template_to_world, world_to_local);

				//
				//------------------------------------
				// Multiply the points thru the matrix
				//------------------------------------
				//
				for (int v=0; v<vertex_count; ++v)
				{
					m_P_vertices[vertex++].Multiply(
						spec->m_vertices[v],
						template_to_local
					);
				}

				//
				//---------------------------------------------------------------------------
				// Move to the previous profile and wrap to the end of the list if necessary
				//---------------------------------------------------------------------------
				//
				if (--i < 0)
					i = spec->m_maxProfileCount-1;
			} while (i != m_tailProfile);
		}

		//
		//-----------------------------------------------------------------
		// MC2_VFX_ORACLE_TUBE: if the gate is ON and this Tube is a supported
		// kind, submit the already-built swept-quad/polygon-tube mesh to the
		// GPU ribbon bridge instead of the legacy MLR DrawEffect.
		//
		// Slice 1: alpha-blend ribbon, vertex_count==2 (MissileSmoke).
		// Slice 2 widens coverage to the remaining live Tube kinds, SAME gate:
		//   A. Additive bolt trails (PPC / ER-PPC): OneOneMode / AlphaOneMode.
		//      Carried through as blendMode=1 -> the bridge uses additive GL
		//      blend (GL_SRC_ALPHA, GL_ONE), same indexed ribbon mesh, NO card-
		//      per-profile billboards (the old "white squares" came from the
		//      billboard topology, not the ribbon swept-quad).
		//   B. Polygon profile tubes (vertex_count 3..7 — triangle/square/
		//      pentagon/hexagon/cross). BuildMesh already emits the full ring's
		//      panel quads (incl. the cross special-case) into m_P_indices with
		//      the correct insideOut winding; we just submit the full
		//      m_triangleCount*3 index range that BuildMesh built.
		// Truly unsupported / edge kinds still fall through to legacy MLR.
		//-----------------------------------------------------------------
		//
		bool ribbonSubmitted = false;
		if (mc2::particles::Batcher::is_oracle_tube_enabled())
		{
			const unsigned vc = vertex_count;  // captured above
			const bool isRibbonKind =
				(spec->m_profileType == Specification::e_Ribbon ||
				 spec->m_profileType == Specification::e_AlignedRibbon ||
				 spec->m_profileType == Specification::e_VerticalRibbon);
			const MidLevelRenderer::MLRState::AlphaMode am =
				spec->m_state.GetAlphaMode();
			// Alpha-blend (smoke ribbons) vs additive (PPC bolts). Same
			// classification convention as the billboard bridge / spawn_*.cpp:
			// OneOneMode / AlphaOneMode = additive; everything else = alpha.
			const bool isAdditive =
				(am == MidLevelRenderer::MLRState::OneOneMode ||
				 am == MidLevelRenderer::MLRState::AlphaOneMode);
			const bool isAlphaBlend =
				(am == MidLevelRenderer::MLRState::AlphaInvAlphaMode ||
				 am == MidLevelRenderer::MLRState::OneInvAlphaMode  ||
				 am == MidLevelRenderer::MLRState::OneAlphaMode);

			// Slice 2 widens the kind classifier:
			//  - ribbon kinds (vc==2): alpha OR additive (was alpha-only).
			//  - polygon profiles (vc 3..7): triangle/square/pentagon/hexagon/
			//    cross. m_triangleCount and m_P_indices already encode the full
			//    ring (BuildMesh, incl. the vc==5 cross special-case) with the
			//    spec's insideOut winding — submit exactly what BuildMesh built.
			const bool isPolygonKind = (vc >= 3 && vc <= 7);
			const bool isSupportedBlend = isAlphaBlend || isAdditive;
			// Both ribbon and polygon tubes route through the same indexed mesh
			// submit. (Ribbon == vc 2 swept quad; polygon == vc>=3 ring panels.)
			const bool supported =
				isSupportedBlend && (vc >= 2) && (isRibbonKind || isPolygonKind);
			const int blendMode = isAdditive ? 1 : 0;

			if (supported && m_P_indices)
			{
				// Live, compacted mesh: profiles 0..m_activeProfileCount are
				// contiguous in m_P_vertices/colors/uvs (AnimateProfile writes
				// vertex_index = profile_count*vc). Index range = the first
				// m_triangleCount triangles of the BuildMesh stencil — for both
				// the vc==2 ribbon and the vc>=3 polygon ring (cross included).
				const unsigned numVerts   = m_activeProfileCount * vc;
				const unsigned numIndices = static_cast<unsigned>(m_triangleCount) * 3u;

				const uint32_t gosHandle =
					static_cast<uint32_t>(spec->m_state.GetTextureHandle());

				// Q2-S2: build the world-space vertex copy ONLY when it will
				// actually be enqueued (gosHandle != 0). For untextured tubes
				// (gosHandle==0, e.g. gauss) ribbonSubmitted stays false and the
				// legacy MLR DrawEffect renders from the local-space m_P_vertices
				// instead — so transforming every vert to world here was pure
				// waste (an O(numVerts) matrix-vec pass built then discarded).
				// Behaviour-identical: s_worldPos was never read when gosHandle==0.
				//
				// TUBE-DEFERRED-FLUSH-1: with a real texture handle, enqueue into
				// the deferred ribbon queue (drawn post-renderLists via
				// gos_tube_ribbon_flush_deferred in gamecam.cpp).
				if (gosHandle != 0) {
					// Transform local-space verts to MC2/Stuff world space.
					// (m_P_vertices are effect-local; the ribbon VS applies the GL
					// axis swap, so we hand it MC2 world unswapped.)
					static Stuff::DynamicArrayOf<float> s_worldPos;
					if (static_cast<unsigned>(s_worldPos.GetLength()) < numVerts*3u)
						s_worldPos.SetLength(numVerts*3u);
					for (unsigned vi = 0; vi < numVerts; ++vi)
					{
						Stuff::Point3D wp;
						wp.Multiply(m_P_vertices[vi], local_to_world);
						s_worldPos[vi*3+0] = wp.x;
						s_worldPos[vi*3+1] = wp.y;
						s_worldPos[vi*3+2] = wp.z;
					}

					gos_tube_ribbon_enqueue(
						&s_worldPos[0],
						reinterpret_cast<const float*>(m_P_colors),
						reinterpret_cast<const float*>(m_P_uvs),
						numVerts,
						m_P_indices,
						numIndices,
						gosHandle,
						blendMode);  // 0=alpha, 1=additive (PPC bolts)
					ribbonSubmitted = true;
				}
				// gosHandle==0: ribbonSubmitted stays false → DrawEffect below.
			}
		}

		//
		//--------------------
		// Now draw the effect (legacy MLR) — skipped when the ribbon oracle
		// submitted this Tube above.
		//--------------------
		//
		if (!ribbonSubmitted)
	 		info->m_clipper->DrawEffect(&dInfo);
	}

	Effect::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Tube::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
