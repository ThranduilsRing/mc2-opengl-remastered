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
#include <cctype>

namespace mc2 {
namespace particles {

namespace {
// Shutdown must run before the gosFX heap/registries, never during static teardown.
SpecLibrary* g_instance = nullptr;
}

SpecLibrary* SpecLibrary::Instance()
{
    if (!g_instance) {
        gos_PushCurrentHeap(gosFX::Heap);
        g_instance = new SpecLibrary();
        gos_PopCurrentHeap();
    }
    return g_instance;
}

void SpecLibrary::Shutdown()
{
    if (g_instance) {
        delete g_instance;
        g_instance = nullptr;
    }
}

SpecLibrary::SpecLibrary()
{
}

SpecLibrary::~SpecLibrary()
{
    m_nameIndex.clear();
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
    m_nameIndex.clear();
    m_nameIndex.reserve(m_effects.size());
    for (unsigned i = 0; i < m_effects.size(); ++i) {
        if (m_effects[i]) {
            // An unset/empty MString is a null C-string (mstring.cpp:467), the same
            // state the scan's S_stricmp could not accept; index only named specs.
            if (const auto* name = static_cast<const char*>(m_effects[i]->m_name)) {
                m_nameIndex.try_emplace(name, i);
            }
        }
    }
}

size_t SpecLibrary::NameHash::operator()(std::string_view name) const
{
    // Fold matches S_stricmp (_stricmp/strcasecmp): lowercase bytes in LC_CTYPE.
    // The engine never changes LC_CTYPE from its initial C locale.
    size_t hash = 0;
    for (unsigned char byte : name) {
        hash = hash * 131 + static_cast<unsigned>(std::tolower(byte));
    }
    return hash;
}

bool SpecLibrary::NameEqual::operator()(std::string_view left, std::string_view right) const
{
    // Same ASCII/C-locale fold as NameHash, over the view length (no NUL assumption).
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i])))
            return false;
    }
    return true;
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

    gosFX::Effect::Specification* spec = nullptr;
    const auto found = (!name || m_nameIndex.empty()) ? m_nameIndex.end()
                                                      : m_nameIndex.find(std::string_view(name));
    if (found != m_nameIndex.end()) {
        const auto& [key, i] = *found;
        spec = m_effects[i].get();
        Check_Object(spec);
        Verify(spec->m_effectID == i);
    }
    return spec;
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
