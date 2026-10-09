// Fixture: color_prop_tier1.vert expects set0 UBO@0 (VS|FS), SSBO@1, push block 16B.
// Here binding 0 is VS-less, binding 1 is a UBO instead of an SSBO, push range is 8B.
static void build(S* s) {
    s->vert = load_spv(s->device, spv_dir() + "color_prop_tier1.vert.spv");
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.offset = 0; pcr.size = 8;
    VkPipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
    VkVertexInputBindingDescription vib{};
    vib.binding = 0; vib.stride = 40; vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via[5]{};
    via[0].location = 0; via[0].binding = 0; via[0].format = VK_FORMAT_R32G32B32_SFLOAT; via[0].offset = 0;
    via[1].location = 1; via[1].binding = 0; via[1].format = VK_FORMAT_R32G32B32_SFLOAT; via[1].offset = 12;
    via[2].location = 2; via[2].binding = 0; via[2].format = VK_FORMAT_R32G32_SFLOAT;    via[2].offset = 24;
    via[3].location = 3; via[3].binding = 0; via[3].format = VK_FORMAT_R32_UINT;         via[3].offset = 32;
    via[4].location = 4; via[4].binding = 0; via[4].format = VK_FORMAT_R32_UINT;         via[4].offset = 36;
}
