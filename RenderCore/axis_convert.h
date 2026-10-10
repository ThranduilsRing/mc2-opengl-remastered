// RenderCore/axis_convert.h -- the one C++ spelling of the MC2 <-> render-space axis conversion.
//
// MC2/Stuff space (x = east, y = north, z = elevation) and render (GL/VK) space (x = -east, y = up,
// z = forward) differ by the permutation (x, y, z) -> (-x, z, y).
//
// WHY one function for both directions: the permutation is an involution. Applying it twice gives
// (-(-x), y, z) = (x, y, z), so the spelling that takes a Stuff vector to render space also brings it back.
// mc2ToRender and renderToMc2 share a body and exist so that a call site says which way it is going.
//
// The same answer is held by three spellings that a doctest (tests/unit/test_xform_convention.cpp) keeps equal:
// this header, the matrix mc2xform::makeAxisSwapMC2toGL() (mclib/xform_conventions.cpp, row-vector convention)
// and the GLSL include shaders/include/axis.hglsl. Do not re-type the triple at a call site;
// tools/axis_swizzle_ratchet.py counts hand-written copies and lets the count only fall.
//
// The helpers are templates over any 3-component type with x/y/z members (Stuff::Vector3D, Stuff::Point3D, a plain
// struct), so this header pulls in no engine header. A source that needs a DIFFERENT component in the middle slot
// (a ground/water elevation substituted for the swapped z) keeps its hand code: it is not this conversion.
#ifndef RENDERCORE_AXIS_CONVERT_H
#define RENDERCORE_AXIS_CONVERT_H

// out = swap(in). Reads every input component before writing, so `in` and `out` may be the same object.
template <class Out, class In>
inline void mc2ToRender(Out& out, const In& in)
{
    const auto x = -in.x;
    const auto y = in.z;
    const auto z = in.y;
    out.x = x;
    out.y = y;
    out.z = z;
}

// The inverse direction: the same permutation, see the involution note above.
template <class Out, class In>
inline void renderToMc2(Out& out, const In& in)
{
    mc2ToRender(out, in);
}

// Value form: the converted copy of `v` (same type).
template <class V>
inline V mc2ToRender(const V& v)
{
    V r = v;
    mc2ToRender(r, v);
    return r;
}

template <class V>
inline V renderToMc2(const V& v)
{
    V r = v;
    renderToMc2(r, v);
    return r;
}

#endif // RENDERCORE_AXIS_CONVERT_H
