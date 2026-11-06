// Fixture: the post-c26b1eb2a fix (accessor-provided stride) -- must be clean.
// shadow_prop.vert carries a DrawPush block since the BDA shadow caster (f057b598b): the layout names its range.
static void build(S* s) {
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    s->vert = load_spv(s->device, spv_dir() + "shadow_prop.vert.spv");
    s->frag = load_spv(s->device, spv_dir() + "shadow_prop.frag.spv");
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = mc2_static_prop_vertex_stride();
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[2]{};
    via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
    via[1].location = 2; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32_SFLOAT;    via[1].offset = 24;
}
