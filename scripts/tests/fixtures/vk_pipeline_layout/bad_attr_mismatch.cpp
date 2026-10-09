// Fixture: mech_tier1.vert needs locations 0..6. Here location 3 is missing,
// location 4 has the wrong class (float vs uvec4 expected at 3 / vec4 at 4 ok),
// location 2 is float-class but declared integer, and location 6 overruns the stride.
static void build(S* s) {
    s->vert = load_spv(s->device, spv_dir() + "mech_tier1.vert.spv");
    s->frag = load_spv(s->device, spv_dir() + "mech_tier1.frag.spv");
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = 48; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[6]{};
    via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
    via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
    via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_UINT;      via[2].offset = 24;
    via[3].location = 4; via[3].binding = 0; via[3].format = VK_FORMAT_R8G8B8A8_UNORM;   via[3].offset = 36;
    via[4].location = 5; via[4].binding = 0; via[4].format = VK_FORMAT_R16G16_SNORM;     via[4].offset = 40;
    via[5].location = 6; via[5].binding = 0; via[5].format = VK_FORMAT_R32_UINT;         via[5].offset = 46;
}
