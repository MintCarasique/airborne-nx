// Included inside namespace airborne after VulkanDevice::Impl is defined.
std::vector<uint8_t> VulkanDevice::draw_video(const uint32_t* vertex,size_t vertex_words,
    const uint32_t* pixel,size_t pixel_words,const std::vector<uint8_t> (&planes)[3]) {
    auto& s=*impl_;auto& v=s.video;
    constexpr uint32_t widths[4]={1280,640,640,1280},heights[4]={720,360,360,720};
    constexpr uint32_t offsets[3]={0,921600,1152000},shared=0x180000,vertices=shared+512;
    constexpr uint32_t buffer_bytes=vertices+120;
    if(!ready() || s.transfer_failed || s.width!=1280 || s.height!=720 || !vertex || !pixel || vertex_words<5 || pixel_words<5)
        throw std::runtime_error("Invalid native movie pipeline state");
    for(unsigned i=0;i<3;++i)if(planes[i].size()!=size_t(widths[i])*heights[i])throw std::runtime_error("Invalid native movie plane size");
    try {
        if(!v.pipeline) {
            VkPhysicalDeviceMemoryProperties memory{};s.GetPhysicalDeviceMemoryProperties(s.physical,&memory);
            auto memory_type=[&](uint32_t bits,VkMemoryPropertyFlags flags) {
                for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
                throw std::runtime_error("No native movie memory type");
            };
            for(unsigned i=0;i<4;++i) {
                const VkFormat format=i==3?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R8_UNORM;
                VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=format;
                image.extent={widths[i],heights[i],1};image.mipLevels=1;image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
                image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=i==3?(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT):(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
                s.require(s.CreateImage(s.device,&image,nullptr,&v.images[i]),"vkCreateImage/video",size_t(widths[i])*heights[i]*(i==3?4:1));
                VkMemoryRequirements requirements{};s.GetImageMemoryRequirements(s.device,v.images[i],&requirements);
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
                allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                s.require(s.AllocateMemory(s.device,&allocation,nullptr,&v.memory[i]),"vkAllocateMemory/video-image",requirements.size);
                s.require(s.BindImageMemory(s.device,v.images[i],v.memory[i],0),"vkBindImageMemory/video");
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=v.images[i];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;
                view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                s.require(s.CreateImageView(s.device,&view,nullptr,&v.views[i]),"vkCreateImageView/video");
            }
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=buffer_bytes;
            bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            s.require(s.CreateBuffer(s.device,&bi,nullptr,&v.buffer),"vkCreateBuffer/video",bi.size);
            VkMemoryRequirements requirements{};s.GetBufferMemoryRequirements(s.device,v.buffer,&requirements);
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.pNext=&flags;allocation.allocationSize=requirements.size;
            allocation.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            s.require(s.AllocateMemory(s.device,&allocation,nullptr,&v.buffer_memory),"vkAllocateMemory/video-buffer",requirements.size);
            s.require(s.BindBufferMemory(s.device,v.buffer,v.buffer_memory,0),"vkBindBufferMemory/video");
            s.require(s.MapMemory(s.device,v.buffer_memory,0,VK_WHOLE_SIZE,0,&v.mapped),"vkMapMemory/video",requirements.size);
            VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};address.buffer=v.buffer;v.address=s.GetBufferDeviceAddress(s.device,&address);
            if(!v.address)throw std::runtime_error("Missing movie constant device address");
            char location[256];std::snprintf(location,sizeof(location),"GPU_VIDEO_MEMORY host=%016llx device=%016llx bytes=%u type=%u flags=%08x",(unsigned long long)uintptr_t(v.mapped),(unsigned long long)v.address,buffer_bytes,allocation.memoryTypeIndex,memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags);s.log(location);
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=VK_FILTER_LINEAR;sampler.minFilter=VK_FILTER_LINEAR;
            sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            s.require(s.CreateSampler(s.device,&sampler,nullptr,&v.sampler),"vkCreateSampler/video");
            for(unsigned i=0;i<4;++i) {
                VkDescriptorSetLayoutBinding binding{};binding.binding=0;binding.descriptorCount=3;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
                binding.descriptorType=i==3?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                if(i==0 || i==3){layout.bindingCount=1;layout.pBindings=&binding;}
                s.require(s.CreateDescriptorSetLayout(s.device,&layout,nullptr,&v.sets_layout[i]),"vkCreateDescriptorSetLayout/video");
            }
            VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_SAMPLER,3}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=4;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
            s.require(s.CreateDescriptorPool(s.device,&pool,nullptr,&v.descriptors),"vkCreateDescriptorPool/video");
            VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sets.descriptorPool=v.descriptors;sets.descriptorSetCount=4;sets.pSetLayouts=v.sets_layout;
            s.require(s.AllocateDescriptorSets(s.device,&sets,v.sets),"vkAllocateDescriptorSets/video");
            VkDescriptorImageInfo images[3]{},samplers[3]{};VkWriteDescriptorSet writes[2]{};
            for(unsigned i=0;i<3;++i){images[i].imageView=v.views[i];images[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;samplers[i].sampler=v.sampler;}
            for(unsigned i=0;i<2;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=v.sets[i?3:0];writes[i].descriptorCount=3;writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[i].pImageInfo=i?samplers:images;}
            s.UpdateDescriptorSets(s.device,2,writes,0,nullptr);
            const uint32_t* code[2]={vertex,pixel};const size_t words[2]={vertex_words,pixel_words};
            for(unsigned i=0;i<2;++i){VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=words[i]*4;shader.pCode=code[i];s.require(s.CreateShaderModule(s.device,&shader,nullptr,&v.shaders[i]),"vkCreateShaderModule/video",shader.codeSize);}
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,24};
            VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=4;layout.pSetLayouts=v.sets_layout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&range;
            s.require(s.CreatePipelineLayout(s.device,&layout,nullptr,&v.layout),"vkCreatePipelineLayout/video");
            VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R8G8B8A8_UNORM;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
            VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;dependency.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependency.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;pass.dependencyCount=1;pass.pDependencies=&dependency;
            s.require(s.CreateRenderPass(s.device,&pass,nullptr,&v.pass),"vkCreateRenderPass/video");
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=v.pass;fb.attachmentCount=1;fb.pAttachments=&v.views[3];fb.width=1280;fb.height=720;fb.layers=1;
            s.require(s.CreateFramebuffer(s.device,&fb,nullptr,&v.framebuffer),"vkCreateFramebuffer/video");
            VkPipelineShaderStageCreateInfo stages[2]{};for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=v.shaders[i];stages[i].pName="main";}
            VkVertexInputBindingDescription binding{0,20,VK_VERTEX_INPUT_RATE_VERTEX};VkVertexInputAttributeDescription attributes[2]={{0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,0,VK_FORMAT_R32G32_SFLOAT,12}};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;vi.vertexAttributeDescriptionCount=2;vi.pVertexAttributeDescriptions=attributes;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport viewport{0,0,1280,720,0,1};VkRect2D scissor{{0,0},{1280,720}};
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend_attachment{};blend_attachment.colorWriteMask=15;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&blend_attachment;
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipeline.stageCount=2;pipeline.pStages=stages;pipeline.pVertexInputState=&vi;pipeline.pInputAssemblyState=&ia;pipeline.pViewportState=&vp;pipeline.pRasterizationState=&raster;pipeline.pMultisampleState=&ms;pipeline.pColorBlendState=&blend;pipeline.layout=v.layout;pipeline.renderPass=v.pass;
            s.require(s.CreateGraphicsPipelines(s.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&v.pipeline),"vkCreateGraphicsPipelines/video");
        }
        auto* mapped=static_cast<uint8_t*>(v.mapped);
        for(unsigned i=0;i<3;++i)std::memcpy(mapped+offsets[i],planes[i].data(),planes[i].size());
        std::memset(mapped+shared,0,512);
        for(uint32_t i=0;i<3;++i){std::memcpy(mapped+shared+4*i,&i,4);std::memcpy(mapped+shared+192+4*i,&i,4);}
        // The guarded guest program exports screen coordinates with viewport
        // transform disabled. Convert its exact quad to Vulkan NDC. DXC flips
        // Y; native sample centres then reproduce Xenos integer pixel centres.
        const float quad[30]={-1,1,0,0,0, 1,1,0,1,0, -1,-1,0,0,1,
                              -1,-1,0,0,1, 1,1,0,1,0, 1,-1,0,1,1};
        std::memcpy(mapped+vertices,quad,sizeof(quad));
        // Xenos integer centres: sample (x,y) maps to UV (x/W,y/H).
        const float half_pixel[2]={1.0f/1280,-1.0f/720};std::memcpy(mapped+shared+264,half_pixel,8);
        if(!s.readback)s.create_transfer_buffer(s.readback,s.readback_memory,s.readback_map);
        if(!s.commands){VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=s.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;s.require(s.AllocateCommandBuffers(s.device,&ai,&s.commands),"vkAllocateCommandBuffers/video");VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};s.require(s.CreateFence(s.device,&fi,nullptr,&s.fence),"vkCreateFence/video");}
        s.require(s.ResetCommandPool(s.device,s.pool,0),"vkResetCommandPool/video");s.require(s.ResetFences(s.device,1,&s.fence),"vkResetFences/video");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;s.require(s.BeginCommandBuffer(s.commands,&begin),"vkBeginCommandBuffer/video");
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_SHADER_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=v.buffer;host.size=VK_WHOLE_SIZE;
        s.CmdPipelineBarrier(s.commands,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&host,0,nullptr);
        for(unsigned i=0;i<3;++i) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=v.images[i];barrier.oldLayout=v.initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcAccessMask=v.initialized?VK_ACCESS_SHADER_READ_BIT:0;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            s.CmdPipelineBarrier(s.commands,v.initialized?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy copy{};copy.bufferOffset=offsets[i];copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={widths[i],heights[i],1};s.CmdCopyBufferToImage(s.commands,v.buffer,v.images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;s.CmdPipelineBarrier(s.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        }
        VkClearValue clear{};VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=v.pass;pass.framebuffer=v.framebuffer;pass.renderArea={{0,0},{1280,720}};pass.clearValueCount=1;pass.pClearValues=&clear;
        s.CmdBeginRenderPass(s.commands,&pass,VK_SUBPASS_CONTENTS_INLINE);s.CmdBindPipeline(s.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,v.pipeline);s.CmdBindDescriptorSets(s.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,v.layout,0,4,v.sets,0,nullptr);
        const uint64_t constants[3]={0,0,v.address+shared};s.CmdPushConstants(s.commands,v.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,24,constants);
        const VkDeviceSize offset=vertices;s.CmdBindVertexBuffers(s.commands,0,1,&v.buffer,&offset);s.CmdDraw(s.commands,6,1,0,0);s.CmdEndRenderPass(s.commands);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1280,720,1};s.CmdCopyImageToBuffer(s.commands,v.images[3],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.readback,1,&copy);
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.buffer=s.readback;s.CmdPipelineBarrier(s.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        s.require(s.EndCommandBuffer(s.commands),"vkEndCommandBuffer/video");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&s.commands;s.require(s.QueueSubmit(s.queue,1,&submit,s.fence),"vkQueueSubmit/video");s.require(s.WaitForFences(s.device,1,&s.fence,VK_TRUE,3000000000ull),"vkWaitForFences/video");
        v.initialized=true;const auto* output=static_cast<const uint8_t*>(s.readback_map);s.message("GPU_VIDEO_NATIVE_DRAW_COMPLETE",0,1280*720*4);return {output,output+1280*720*4};
    }catch(...){s.transfer_failed=true;throw;}
}
