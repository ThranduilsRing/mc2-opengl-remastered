#include"gosfxheaders.hpp"

#include "particles/batcher.h"
#include <algorithm>
#include <mlr/mlrindexedprimitivebase.hpp>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <span>
#include "mc2fmt.h"
#include "../../code/render_view.h"
#include "../../RenderCore/FrameArena.h"

// One-way bridge ABI; keep in sync with gos_vfx_mesh_bridge.h.
struct GosVfxMeshUpload {
    uint64_t meshId;
    const float* positions;
    const float* uvs;
    const uint16_t* indices;
    uint32_t vertexCount, indexCount;
};
struct GosVfxMeshInstance {
    uint64_t meshId;
    float modelToWorld[16];
    float scale, rgba[4];
    uint32_t mlrTexHandle;
    int blendMode;
};
extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload*, unsigned,
                                   const GosVfxMeshInstance*, unsigned);

struct gosFX::NativeShapePrimitive {
    GosVfxMeshUpload upload{};
    GosVfxMeshInstance instance{};
    bool needsUpload = false;
    const NativeShapePrimitive* next = nullptr;
};

namespace {
std::unordered_set<uint64_t> uploaded;

template<class T>
const T* freezeArray(RenderCore::FrameArena& arena, std::span<const T> source)
{
    if (source.empty()) return nullptr;
    auto copy = arena.allocArray<T>(static_cast<uint32_t>(source.size()), "NativeShape");
    if (copy.empty()) std::abort();
    std::copy(source.begin(), source.end(), copy.data);
    return copy.data;
}

bool captureNativeShape(const MidLevelRenderer::DrawScalableShapeInformation& draw,
    RenderCore::FrameArena* arena, gosFX::FrozenNativeShape& frozen)
{
    frozen = {};
    if (!mc2::particles::Batcher::is_oracle_shape_enabled() || !draw.shape) return false;
    // Validate before submitting any primitive: fallback must never double draw.
    gosFX::NativeShapePrimitive* tail = nullptr;
    for (int p = 0; p < draw.shape->GetNum(); ++p) {
        auto* prim = draw.shape->Find(p);
        if (!prim || !prim->IsDerivedFrom(MidLevelRenderer::MLRIndexedPrimitiveBase::DefaultData)) return false;
        auto* mesh = static_cast<MidLevelRenderer::MLRIndexedPrimitiveBase*>(prim);
        unsigned short* indices = nullptr; int count = 0;
        mesh->GetIndexData(&indices, &count);
        if (!indices || count <= 0 || std::any_of(indices, indices + count,
            [&](auto i) { return i >= mesh->GetNumVertices(); })) return false;
        unsigned char* lengths = nullptr; int polygons = 0;
        mesh->GetSubprimitiveLengths(&lengths, &polygons);
        int total = 0;
        for (int f = 0; f < polygons; ++f) {
            if (lengths[f] < 3) return false;
            total += lengths[f];
        }
        if (polygons ? total != count : count % 3 != 0) return false;
    }
    for (int p = 0; p < draw.shape->GetNum(); ++p) {
        auto* prim = static_cast<MidLevelRenderer::MLRIndexedPrimitiveBase*>(draw.shape->Find(p));
        const uint64_t id = reinterpret_cast<uintptr_t>(prim);
        std::vector<uint16_t> triangles;
        glm::vec3* coords = nullptr; int count = 0;
        unsigned short* indices = nullptr; int indexCount = 0;
        MidLevelRenderer::Vector2DScalar* texcoords = nullptr; int texCount = 0;
        prim->GetCoordData(&coords, &count);
        prim->GetIndexData(&indices, &indexCount);
        prim->GetTexCoordData(&texcoords, &texCount);
        if (!coords || !indices || count <= 0 || indexCount <= 0) continue;
        const bool upload = !uploaded.contains(id);
        if (upload) {
            unsigned char* lengths = nullptr; int polygons = 0;
            prim->GetSubprimitiveLengths(&lengths, &polygons);
            if (polygons) {
                int offset = 0;
                for (int f = 0; f < polygons; ++f) {
                    const int n = lengths[f];
                    for (int v = 1; v + 1 < n; ++v)
                        triangles.insert(triangles.end(), {indices[offset], indices[offset+v], indices[offset+v+1]});
                    offset += n;
                }
            } else triangles.assign(indices, indices + indexCount);
        }
        // Combine each authored submaterial with this instance's master state.
        MidLevelRenderer::MLRState state;
        state.Combine(draw.state, prim->GetReferenceState());
        using State = MidLevelRenderer::MLRState;
        const auto alpha = state.GetAlphaMode();
        const int blend = alpha == State::OneOneMode ? 2 : alpha == State::AlphaOneMode ? 1 :
            alpha == State::OneZeroMode ? 3 : alpha == State::OneAlphaMode ? 4 :
            alpha == State::OneInvAlphaMode ? 5 : 0;
        GosVfxMeshInstance inst{};
        inst.meshId = id;
        // Legacy matrices store affine translation in [axis][3].
        for (int axis = 0; axis < 3; ++axis)
            for (int k = 0; k < 4; ++k)
                inst.modelToWorld[k*4+axis] = (*draw.shapeToWorld)[axis][k];
        inst.modelToWorld[15] = 1.0f;
        inst.scale = draw.scaling ? draw.scaling->x : 1.0f;
        const Stuff::RGBAColor white(1,1,1,1);
        const auto& color = draw.paintMe ? *draw.paintMe : white;
        inst.rgba[0] = color.red; inst.rgba[1] = color.green; inst.rgba[2] = color.blue;
        inst.rgba[3] = blend == 3 ? 1.0f : color.alpha;
        inst.mlrTexHandle = state.GetTextureHandle();
        inst.blendMode = blend;
        std::vector<float> positions, uvs;
        if (upload) {
            for (int v = 0; v < count; ++v) {
                positions.insert(positions.end(), {coords[v].x, coords[v].y, coords[v].z});
                uvs.push_back(texcoords && v < texCount ? texcoords[v][0] : 0.0f);
                uvs.push_back(texcoords && v < texCount ? texcoords[v][1] : 0.0f);
            }
        }
        GosVfxMeshUpload up{id, positions.data(), uvs.data(),
            triangles.data(), static_cast<uint32_t>(count), static_cast<uint32_t>(triangles.size())};
        if (arena) {
            auto record = arena->allocArray<gosFX::NativeShapePrimitive>(1, "NativeShapePrimitive");
            if (record.empty()) std::abort();
            *record.data = {};
            record.data->instance = inst;
            record.data->needsUpload = upload;
            if (upload) {
                record.data->upload = up;
                record.data->upload.positions = freezeArray(*arena, std::span(up.positions, up.vertexCount * 3));
                record.data->upload.uvs = freezeArray(*arena, std::span(up.uvs, up.vertexCount * 2));
                record.data->upload.indices = freezeArray(*arena, std::span(up.indices, up.indexCount));
            }
            if (tail) tail->next = record.data;
            else frozen = {record.data};
            tail = record.data;
        } else {
            gos_vfx_mesh_flush(upload ? &up : nullptr, upload ? 1u : 0u, &inst, 1u);
            if (upload) uploaded.insert(id);
        }
        if (upload) {
            if (mc2::particles::Batcher::is_oracle_shape_log_enabled())
                mc2::fmt::print_err("[VFX_MESH] prim={} vertices={} indices={} tex={} blend={}\n",
                    p, count, up.indexCount, inst.mlrTexHandle, blend);
        }
    }
    return true;
}
} // namespace

bool gosFX::FreezeNativeShape(const MidLevelRenderer::DrawScalableShapeInformation& draw,
    RenderCore::FrameArena& arena, FrozenNativeShape& frozen)
{
    return captureNativeShape(draw, &arena, frozen);
}

void gosFX::DrawFrozenNativeShape(const FrozenNativeShape& frozen)
{
    const auto [head] = frozen;
    for (auto* record = head; record; record = record->next) {
        gos_vfx_mesh_flush(record->needsUpload ? &record->upload : nullptr,
            record->needsUpload ? 1u : 0u, &record->instance, 1u);
        if (record->needsUpload) uploaded.insert(record->instance.meshId);
    }
}

bool gosFX::DrawNativeShape(const MidLevelRenderer::DrawScalableShapeInformation& draw)
{
    FrozenNativeShape frozen;
    if (auto* arena = mc2::render_view::drawArena()) {
        if (!FreezeNativeShape(draw, *arena, frozen)) return false;
        DrawFrozenNativeShape(frozen);
        return true;
    }
    return captureNativeShape(draw, nullptr, frozen);
}

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
			glm::vec3 *points;
			int vertex_count;
			primitive->GetCoordData(&points, &vertex_count);
			for (int v=0; v<vertex_count; ++v)
			{
				Stuff::Scalar len = mc2::math::lengthSquared(points[v]);
				m_radius = std::max(m_radius, len);
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
			effectFactoryThunk<&Make>,
			specificationFactoryThunk<&Specification::Make>
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
void gosFX::Shape::Draw(DrawInfo *info) const
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
	const Specification *spec = GetSpecification();
	Check_Object(spec);
	dinfo.state.Combine(info->m_state, spec->m_state);
	dinfo.activeLights = nullptr;
	dinfo.nrOfActiveLights = 0;
	dinfo.shape = shape;
	glm::vec3 scale(m_scale, m_scale, m_scale);
	dinfo.scaling = &scale;
	dinfo.paintMe = &m_color;
	glm::mat4 local_to_world;
	local_to_world = m_localToParent * *info->m_parentToWorld;
	dinfo.shapeToWorld = &local_to_world;

	//
	//--------------------------------------------------------------
	// Check the orientation mode.  The first case is XY orientation
	//--------------------------------------------------------------
	//
	if (spec->m_alignZUsingX)
	{
		glm::vec3
			camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
		glm::vec3 card_in_world = mc2::math::getTranslation(local_to_world);
		glm::vec3 look_at;
		look_at = camera_in_world - card_in_world;
		if (spec->m_alignZUsingY)
			mc2::math::alignLocalAxisToWorldVector(local_to_world, look_at, Stuff::Z_Axis, Stuff::Y_Axis, Stuff::X_Axis);
		else
			mc2::math::alignLocalAxisToWorldVector(local_to_world, look_at, Stuff::Z_Axis, Stuff::X_Axis, -1);
	}

	//
	//-------------------------------------------------------
	// Each matrix needs to be aligned to the camera around Y
	//-------------------------------------------------------
	//
	else if (spec->m_alignZUsingY)
	{
		glm::vec3
			camera_in_world = mc2::math::getTranslation(info->m_clipper->GetCameraToWorldMatrix());
		glm::vec3 card_in_world = mc2::math::getTranslation(local_to_world);
		glm::vec3 look_at;
		look_at = camera_in_world - card_in_world;
		mc2::math::alignLocalAxisToWorldVector(local_to_world, look_at, Stuff::Z_Axis, Stuff::Y_Axis, -1);
	}

	DrawNativeShape(dinfo);
    Singleton::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::Shape::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}
