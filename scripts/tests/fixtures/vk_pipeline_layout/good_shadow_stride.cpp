// Fixture: the post-c26b1eb2a fix (accessor-provided stride) -- must be clean.
static void build(S* s) {
    s->vert = load_spv(s->device, spv_dir() + "shadow_prop.vert.spv");
    s->frag = load_spv(s->device, spv_dir() + "shadow_prop.frag.spv");
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = mc2_static_prop_vertex_stride();
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via{};
    via.location = 0; via.binding = 0; via.format = VK_FORMAT_R32G32B32_SFLOAT; via.offset = 0;
}
