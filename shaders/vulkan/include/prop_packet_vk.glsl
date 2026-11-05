// Per-draw packet data for the GPU-driven static-prop pass. One row per indirect draw, indexed
// packetBase + gl_DrawID (the VS forwards the row index to the FS as the flat vPacketIdx, which the
// including stage declares before this include). The rows were the 32-byte push block of the old
// per-packet draw loop; the field names are unchanged so every uPacket.<field> reader is untouched.
//
// The rows are read through buffer device address: the push block carries the row array's address, no descriptor binds them.
// GL_EXT_buffer_reference compiles under both --target-env vulkan1.0 and vulkan1.2 (glslang emits SPV_KHR_physical_storage_buffer below SPIR-V 1.5).
// The including shader declares `#extension GL_EXT_buffer_reference : require` at the top (extension directives precede all code).
struct PropPacket {
    int   packetLayerOff;
    int   packetLayerOn;
    float packetUvScaleX;
    float packetUvScaleY;
    int   packetAlphaTest;      // GL's live per-packet ALPHA_TEST bit (the FS reads it)
    int   coverageFringePass;   // PROP-COVERAGE-1 (FS only): 0 core, 1 fringe
    int   materialId;
    int   pad;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PropPackets {
    PropPacket p[];
};
layout(push_constant) uniform PacketBasePC {   // vertex and fragment stages (16 B)
    PropPackets rows;   // device address of row 0 of the packet-row buffer
    uint packetBase;    // first row of the list this vkCmdDrawIndexedIndirectCount call draws
} uPacketBase;
#define uPacket (uPacketBase.rows.p[vPacketIdx])
