// mclib/render_contract_pipeline.h
//
// PIPELINE-DESC-SCAFFOLD-1: GL-free adapter that lowers a PassStateContract
// (mclib/render_contract.h -- the *requirements* surface, "what a pass needs")
// into a RenderCore::PipelineDesc (RenderCore/PipelineDesc.h -- the
// *engine-owned render-intent* surface).
//
// Track R item 12 (render contracts): until now the two surfaces existed side
// by side but nothing connected them. This is the one place they are
// reconciled. It adds NO new state and replaces NO draw dispatch.
//
// The adapter is header-only and independent of driver handles.
//
// Layering: lives mclib-side. mclib MAY depend on RenderCore (e.g. mech3d.h
// includes ../RenderCore/Handle.h); RenderCore must NOT depend on mclib --
// that is why ColorAttachmentMask is duplicated rather than shared, and why
// this adapter cannot live in RenderCore.
//
// SCOPE (slice A): conversion + tests only. There is no production caller yet;
// this is substrate. Wiring passes through it / populating
// RenderPassContract.pipelineDescRegistered is a deferred follow-up (slice B).
//
// Only the four fields PassStateContract actually carries are derived:
//   requiresDepthTest  -> depthTestEnable
//   requiresDepthWrite -> depthWriteEnable
//   blend              -> blend            (value-identical enums; see asserts)
//   attachments        -> colorAttachments (field-for-field)
// Everything else is an explicit argument with a documented current-MC2
// default, because PassStateContract has no data for it:
//   depthFunc            GreaterEqual = MC2 reverse-Z default
//   cullMode             Back = standard opaque-geometry cull
//   objectIdWriteEnabled lives in ShaderOutputContract.writesLocation2, not here
//   ssboBindingsMask     per-pass resource requirement; caller supplies

#pragma once

#include <cstdint>

#include "render_contract.h"
#include "../RenderCore/PipelineDesc.h"

namespace render_contract {

// Enum-parity guard. PipelineDesc.h documents that these two BlendMode enums
// must stay value-identical "so callers can cast freely", but no static_assert
// actually enforced it (the comment claims one lives in render_contract.cpp;
// it does not). Enforce it here -- a future reorder of either enum now breaks
// the build instead of silently miscompiling the blend mode.
static_assert(static_cast<int>(PassStateContract::BlendMode::Opaque)
                  == static_cast<int>(RenderCore::BlendMode::Opaque),
              "PassStateContract::BlendMode::Opaque must match RenderCore::BlendMode::Opaque");
static_assert(static_cast<int>(PassStateContract::BlendMode::AlphaBlend)
                  == static_cast<int>(RenderCore::BlendMode::AlphaBlend),
              "PassStateContract::BlendMode::AlphaBlend must match RenderCore::BlendMode::AlphaBlend");
static_assert(static_cast<int>(PassStateContract::BlendMode::AlphaTest)
                  == static_cast<int>(RenderCore::BlendMode::AlphaTest),
              "PassStateContract::BlendMode::AlphaTest must match RenderCore::BlendMode::AlphaTest");
static_assert(static_cast<int>(PassStateContract::BlendMode::Additive)
                  == static_cast<int>(RenderCore::BlendMode::Additive),
              "PassStateContract::BlendMode::Additive must match RenderCore::BlendMode::Additive");

// Lower a PassStateContract's requirements into an engine-owned PipelineDesc.
// Pure; safe to call before renderer initialization.
inline RenderCore::PipelineDesc pipelineDescFromPassContract(
    const PassStateContract& contract,
    RenderCore::DepthFunc    depthFunc            = RenderCore::DepthFunc::GreaterEqual,
    RenderCore::CullMode     cullMode             = RenderCore::CullMode::Back,
    bool                     objectIdWriteEnabled = false,
    std::uint32_t            ssboBindingsMask     = 0u)
{
    RenderCore::PipelineDesc desc{};

    // Explicit switch rather than a raw cast: the static_asserts above prove
    // the values match today, but the switch keeps the mapping auditable and
    // survives a future BlendMode that is NOT a pure 1:1 relabel.
    switch (contract.blend) {
        case PassStateContract::BlendMode::Opaque:
            desc.blend = RenderCore::BlendMode::Opaque;     break;
        case PassStateContract::BlendMode::AlphaBlend:
            desc.blend = RenderCore::BlendMode::AlphaBlend; break;
        case PassStateContract::BlendMode::AlphaTest:
            desc.blend = RenderCore::BlendMode::AlphaTest;  break;
        case PassStateContract::BlendMode::Additive:
            desc.blend = RenderCore::BlendMode::Additive;   break;
    }

    desc.depthTestEnable  = contract.requiresDepthTest;
    desc.depthWriteEnable = contract.requiresDepthWrite;
    desc.depthFunc        = depthFunc;
    desc.cullMode         = cullMode;

    // RequiredAttachments -> ColorAttachmentMask, field-for-field. The two
    // structs are intentional mirrors kept separate to avoid the mclib ->
    // RenderCore include cycle (see PipelineDesc.h).
    desc.colorAttachments.color0 = contract.attachments.color0;
    desc.colorAttachments.color1 = contract.attachments.color1;
    desc.colorAttachments.color2 = contract.attachments.color2;

    desc.objectIdWriteEnabled = objectIdWriteEnabled;
    desc.ssboBindingsMask     = ssboBindingsMask;

    return desc;
}

} // namespace render_contract
