layout(push_constant) uniform PacketLayerPC {
    int   packetLayerOff;
    int   packetLayerOn;
    float packetUvScaleX;
    float packetUvScaleY;
    int   packetAlphaTest;   // GL's live per-packet ALPHA_TEST bit (the FS reads it)
    int   coverageFringePass;   // PROP-COVERAGE-1 (FS only)
    int   hzbEnabled;
    int   materialId;
    int   colorDescriptor;
} uPacket;
