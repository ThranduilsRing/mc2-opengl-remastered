// Per-draw packet data for the GPU-driven static-prop pass. One row per indirect draw, indexed
// packetBase + gl_DrawID (the VS forwards the row index to the FS as the flat vPacketIdx, which the
// including stage declares before this include). The rows were the 32-byte push block of the old
// per-packet draw loop; the field names are unchanged so every uPacket.<field> reader is untouched.
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
layout(set = 0, binding = 15, std430) readonly buffer PropPackets {
    PropPacket p[];
} uPacketTbl;
layout(push_constant) uniform PacketBasePC {
    uint packetBase;   // first row of the list this vkCmdDrawIndexedIndirectCount call draws
} uPacketBase;
#define uPacket (uPacketTbl.p[vPacketIdx])
