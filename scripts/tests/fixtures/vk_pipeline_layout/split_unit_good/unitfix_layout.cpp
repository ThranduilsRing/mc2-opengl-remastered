// Fixture piece 1 of 2: the descriptor-set layout and pipeline layout (good).
void build_layout(S* s) {
    VkDescriptorSetLayoutBinding b[7]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[3].binding = 4; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[4].binding = 5; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[5].binding = 7; b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[6].binding = 9; b[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[6].descriptorCount = 1; b[6].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    vkCreateDescriptorSetLayout(s->device, &lci, nullptr, &s->dsl);
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; pcr.offset = 0; pcr.size = 28;
    VkPipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1; plci.pSetLayouts = &s->dsl;
}
