// Fixture: pre-c26b1eb2a shadow island pipeline. The pipeline is baked before
// geometry upload, so s->vertexStride is still 0 and the 12-byte fallback wins
// while the shared static-prop VBO stride is 40.
struct S { unsigned vertexStride; };
static void build(S* s) {
    s->vert = load_spv(s->device, spv_dir() + "shadow_prop.vert.spv");
    s->frag = load_spv(s->device, spv_dir() + "shadow_prop.frag.spv");
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = s->vertexStride ? s->vertexStride : sizeof(float) * 3;
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via{};
    via.location = 0; via.binding = 0; via.format = VK_FORMAT_R32G32B32_SFLOAT; via.offset = 0;
}
