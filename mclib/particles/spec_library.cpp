//==========================================================================//
// File:    spec_library.cpp                                                 //
// Contents: SpecLibrary implementation - parser body moved verbatim from    //
//           mclib/gosfx/effectlibrary.cpp per plan §5.4 B1 Stage 2' C1.     //
//                                                                           //
//           FX_TRACE_SPAWN(name) preserved at the Find() entry point so the //
//           Stage 0' instrumentation continues firing through the adapter.  //
//===========================================================================//

#include "mc2_czstring.h"
#include "gosfx/gosfxheaders.hpp"
#include "spec_library.h"
#include "fx_trace/fx_trace.h"

namespace mc2 {
namespace particles {

namespace {
std::unique_ptr<SpecLibrary> g_instance;
}

SpecLibrary* SpecLibrary::Instance()
{
    if (!g_instance) {
        gos_PushCurrentHeap(gosFX::Heap);
        g_instance.reset(new SpecLibrary()); // constructor is private
        gos_PopCurrentHeap();
    }
    return g_instance.get();
}

void SpecLibrary::Shutdown()
{
    if (g_instance) {
        g_instance.reset();
    }
}

SpecLibrary::SpecLibrary()
{
}

SpecLibrary::~SpecLibrary()
{
    for (unsigned i = 0; i < m_effects.size(); ++i) {
        if (m_effects[i]) {
            Unregister_Object(m_effects[i].get());
            m_effects[i].reset();
        }
    }
}

void SpecLibrary::Load(Stuff::MemoryStream* stream)
{
    Verify(gos_GetCurrentHeap() == gosFX::Heap);
    Verify(m_effects.empty());
    int version = gosFX::ReadGFXVersion(stream);
    unsigned len;
    *stream >> len;
    m_effects.resize(len);
    for (unsigned i = 0; i < len; ++i) {
        gosFX::Effect::Specification* pspec =
            gosFX::Effect::Specification::Create(stream, version);
        m_effects[i].reset(pspec);
        Check_Object(m_effects[i].get());
        m_effects[i]->m_effectID = i;
    }
}

void SpecLibrary::Save(Stuff::MemoryStream* stream)
{
    gosFX::WriteGFXVersion(stream);
    // 64-bit port fix: GetLength() returns size_t (8 bytes on x64), but the
    // read side (Load: `*stream >> len` with `unsigned len`) consumes only 4.
    // The original 32-bit MC2 had size_t==unsigned so this was symmetric; on
    // x64 the unchecked 8-byte write injected a stray high-dword of zeros after
    // the count, desyncing every following spec and producing an unloadable
    // blob. Cast to the same 32-bit width the loader reads.
    *stream << static_cast<unsigned>(m_effects.size());
    for (unsigned i = 0; i < m_effects.size(); ++i) {
        Check_Object(m_effects[i].get());
        m_effects[i]->Save(stream);
    }
}

gosFX::Effect::Specification* SpecLibrary::Find(mc2::czstring name)
{
    // fx_trace v1: per-name spawn-event counter (env-gated, default-off).
    // Preserved verbatim from gosFX::EffectLibrary::Find at B1 Stage 2' C1.
    FX_TRACE_SPAWN(name);

    for (unsigned i = 0; i < m_effects.size(); ++i) {
        gosFX::Effect::Specification* spec = m_effects[i].get();
        if (spec) {
            Check_Object(spec);
            if (!S_stricmp(spec->m_name, name)) {
                Verify(spec->m_effectID == i);
                return spec;
            }
        }
    }
    return nullptr;
}

gosFX::Effect::Specification* SpecLibrary::At(unsigned index)
{
    return m_effects[index].get();
}

unsigned SpecLibrary::Count() const
{
    return static_cast<unsigned int>(m_effects.size());
}

}  // namespace particles
}  // namespace mc2
