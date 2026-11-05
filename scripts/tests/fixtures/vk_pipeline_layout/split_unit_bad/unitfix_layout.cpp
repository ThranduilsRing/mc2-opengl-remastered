// Fixture piece 1 of 2: the descriptor-set layout and pipeline layout (bad: binding 0 lacks VS, binding 1 is a UBO, push range 8B).
void build_layout(S* s) {
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.offset = 0; pcr.size = 2;
    VkPipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
}
