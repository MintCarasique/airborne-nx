#include "vulkan_device.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#ifdef __SWITCH__
#include <switch.h>
#include <vulkan/vulkan_vi.h>
extern void nx_runtime_acquire_display();
extern ViDisplay* nx_runtime_default_display();
#endif
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#ifndef __SWITCH__
#include <fstream>
#endif

namespace airborne {
struct VulkanDevice::Impl {
    Log log;
#ifdef _WIN32
    HMODULE library=nullptr;
#endif
    VkInstance instance=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    VkImage color=VK_NULL_HANDLE;
    VkDeviceMemory color_memory=VK_NULL_HANDLE;
    VkImage depth_image=VK_NULL_HANDLE;VkDeviceMemory depth_memory=VK_NULL_HANDLE;
    uint32_t depth_tile=0,depth_width=0,depth_height=0,depth_samples=0;
    bool depth_initialized=false;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkBuffer upload=VK_NULL_HANDLE,readback=VK_NULL_HANDLE;
    VkDeviceMemory upload_memory=VK_NULL_HANDLE,readback_memory=VK_NULL_HANDLE;
    void* upload_map=nullptr;void* readback_map=nullptr;
    VkCommandBuffer commands=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkShaderModule point_shaders[2]{};VkPipelineLayout point_layout=VK_NULL_HANDLE;
    VkPipeline point_pipeline=VK_NULL_HANDLE;VkRenderPass point_pass=VK_NULL_HANDLE;
    VkFramebuffer point_framebuffer=VK_NULL_HANDLE;VkQueryPool point_query=VK_NULL_HANDLE;
    VkBuffer point_constants=VK_NULL_HANDLE;VkDeviceMemory point_memory=VK_NULL_HANDLE;
    void* point_map=nullptr;VkDeviceAddress point_address=0;
    struct Video {
        VkImage images[4]{};VkDeviceMemory memory[4]{};VkImageView views[4]{};
        VkShaderModule shaders[2]{};VkDescriptorSetLayout sets_layout[4]{};
        VkDescriptorPool descriptors=VK_NULL_HANDLE;VkDescriptorSet sets[4]{};
        VkSampler sampler=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;
        VkRenderPass pass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
        VkPipeline pipeline=VK_NULL_HANDLE;VkBuffer buffer=VK_NULL_HANDLE;
        VkDeviceMemory buffer_memory=VK_NULL_HANDLE;void* mapped=nullptr;
        VkDeviceAddress address=0;bool initialized=false;
    } video;
    uint32_t width=0,height=0,family=UINT32_MAX;
    bool transferred=false,transfer_failed=false;
    uint64_t vblanks=0;
#ifdef _WIN32
    std::chrono::steady_clock::time_point next_vblank{};
#endif
#ifdef __SWITCH__
    Event vsync{};bool vsync_open=false;
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkSemaphore acquired=VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;
    std::vector<VkImage> display_images;
    VkExtent2D display_extent{};
#endif
    bool initialized=false;
    PFN_vkGetInstanceProcAddr get_instance=nullptr;
    PFN_vkGetDeviceProcAddr get_device=nullptr;
#define GPU_INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceImageFormatProperties) X(CreateDevice)
#define GPU_DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateImage) \
    X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
    X(BindImageMemory) X(CreateCommandPool) X(DestroyCommandPool) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(AllocateCommandBuffers) X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) \
    X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdClearDepthStencilImage) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateGraphicsPipelines) X(DestroyPipeline) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CmdBindPipeline) X(CmdPushConstants) X(CmdDraw) X(GetBufferDeviceAddress) \
    X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(GetQueryPoolResults) \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers)
#ifdef __SWITCH__
#define GPU_SURFACE_FUNCTIONS(X) X(CreateViSurfaceNN) X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR)
#define GPU_PRESENT_FUNCTIONS(X) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
    X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateSemaphore) X(DestroySemaphore)
#endif
#define DECLARE(name) PFN_vk##name name=nullptr;
    GPU_INSTANCE_FUNCTIONS(DECLARE)
    GPU_DEVICE_FUNCTIONS(DECLARE)
#ifdef __SWITCH__
    GPU_SURFACE_FUNCTIONS(DECLARE)
    GPU_PRESENT_FUNCTIONS(DECLARE)
#endif
#undef DECLARE
    void message(const char* operation, int64_t code=0, uint64_t size=0) {
        char line[256];
        std::snprintf(line,sizeof(line),"GPU_NATIVE operation=%s rc=%lld bytes=%llu",
                      operation,(long long)code,(unsigned long long)size);
        log(line);
    }
    void require(VkResult result,const char* operation,uint64_t size=0) {
        message(operation,result,size);
        if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" failed: VkResult="+std::to_string(result));
    }
    void close() {
        initialized=false;
        if(device) {
            if(!DestroyDevice || !DeviceWaitIdle)
                throw std::runtime_error("Vulkan cleanup blocked: device dispatch incomplete");
            // Do not release resources whose GPU ownership is unknown.
            const auto result=DeviceWaitIdle(device);
            message("vkDeviceWaitIdle/cleanup",result);
            if(result!=VK_SUCCESS && result!=VK_ERROR_DEVICE_LOST)
                throw std::runtime_error("Vulkan cleanup blocked: device completion unknown");
            auto& v=video;
            const bool video_resources=v.buffer || v.images[0] || v.pipeline;
            if(v.pipeline)DestroyPipeline(device,v.pipeline,nullptr);
            if(v.framebuffer)DestroyFramebuffer(device,v.framebuffer,nullptr);
            if(v.pass)DestroyRenderPass(device,v.pass,nullptr);
            if(v.layout)DestroyPipelineLayout(device,v.layout,nullptr);
            if(v.descriptors)DestroyDescriptorPool(device,v.descriptors,nullptr);
            for(auto item:v.sets_layout)if(item)DestroyDescriptorSetLayout(device,item,nullptr);
            for(auto item:v.shaders)if(item)DestroyShaderModule(device,item,nullptr);
            if(v.sampler)DestroySampler(device,v.sampler,nullptr);
            for(auto item:v.views)if(item)DestroyImageView(device,item,nullptr);
            for(auto item:v.images)if(item)DestroyImage(device,item,nullptr);
            for(auto item:v.memory)if(item)FreeMemory(device,item,nullptr);
            if(v.mapped)UnmapMemory(device,v.buffer_memory);
            if(v.buffer)DestroyBuffer(device,v.buffer,nullptr);
            if(v.buffer_memory)FreeMemory(device,v.buffer_memory,nullptr);
            v={};if(video_resources)message("GPU_VIDEO_RESOURCES_DESTROYED");
            const bool point_resources=point_pipeline || point_memory || point_shaders[0] || point_shaders[1];
            if(point_pipeline){DestroyPipeline(device,point_pipeline,nullptr);point_pipeline=VK_NULL_HANDLE;}
            if(point_framebuffer){DestroyFramebuffer(device,point_framebuffer,nullptr);point_framebuffer=VK_NULL_HANDLE;}
            if(point_pass){DestroyRenderPass(device,point_pass,nullptr);point_pass=VK_NULL_HANDLE;}
            if(point_layout){DestroyPipelineLayout(device,point_layout,nullptr);point_layout=VK_NULL_HANDLE;}
            for(auto& shader:point_shaders)if(shader){DestroyShaderModule(device,shader,nullptr);shader=VK_NULL_HANDLE;}
            if(point_query){DestroyQueryPool(device,point_query,nullptr);point_query=VK_NULL_HANDLE;}
            if(point_map){UnmapMemory(device,point_memory);point_map=nullptr;}
            if(point_constants){DestroyBuffer(device,point_constants,nullptr);point_constants=VK_NULL_HANDLE;}
            if(point_memory){FreeMemory(device,point_memory,nullptr);point_memory=VK_NULL_HANDLE;}
            point_address=0;
            if(point_resources)message("GPU_POINT_RESOURCES_DESTROYED");
#ifdef __SWITCH__
            for(auto semaphore:rendered)if(semaphore)DestroySemaphore(device,semaphore,nullptr);
            rendered.clear();display_images.clear();
            if(acquired){DestroySemaphore(device,acquired,nullptr);acquired=VK_NULL_HANDLE;}
            if(swapchain){DestroySwapchainKHR(device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;}
#endif
            if(fence){DestroyFence(device,fence,nullptr);fence=VK_NULL_HANDLE;}
            if(upload_map){UnmapMemory(device,upload_memory);upload_map=nullptr;}
            if(readback_map){UnmapMemory(device,readback_memory);readback_map=nullptr;}
            if(upload){DestroyBuffer(device,upload,nullptr);upload=VK_NULL_HANDLE;}
            if(readback){DestroyBuffer(device,readback,nullptr);readback=VK_NULL_HANDLE;}
            if(upload_memory){FreeMemory(device,upload_memory,nullptr);upload_memory=VK_NULL_HANDLE;}
            if(readback_memory){FreeMemory(device,readback_memory,nullptr);readback_memory=VK_NULL_HANDLE;}
            if(depth_image){DestroyImage(device,depth_image,nullptr);depth_image=VK_NULL_HANDLE;}
            if(depth_memory){FreeMemory(device,depth_memory,nullptr);depth_memory=VK_NULL_HANDLE;}
            depth_initialized=false;
            if(pool){DestroyCommandPool(device,pool,nullptr);pool=VK_NULL_HANDLE;}
            if(color){DestroyImage(device,color,nullptr);color=VK_NULL_HANDLE;}
            if(color_memory){FreeMemory(device,color_memory,nullptr);color_memory=VK_NULL_HANDLE;}
            DestroyDevice(device,nullptr);device=VK_NULL_HANDLE;queue=VK_NULL_HANDLE;
            message("vkDestroyDevice/cleanup");
        }
#ifdef __SWITCH__
        if(surface){DestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;}
#endif
        if(instance){
            if(!DestroyInstance)throw std::runtime_error("Vulkan cleanup blocked: instance dispatch incomplete");
            DestroyInstance(instance,nullptr);instance=VK_NULL_HANDLE;message("vkDestroyInstance/cleanup");
        }
        physical=VK_NULL_HANDLE;
#ifdef __SWITCH__
        if(vsync_open){
            char line[192];std::snprintf(line,sizeof(line),"VBLANK_EVENT_CLOSE read_handle=%08x write_handle=%08x autoclear=%u",vsync.revent,vsync.wevent,vsync.autoclear);log(line);
            eventClose(&vsync);vsync_open=false;
            message("VBLANK_EVENT_CLOSED_HANDLES_INVALIDATED",eventActive(&vsync)?1:0);
        }
        message("VBLANK_DISPLAY_RELEASED_BORROW_ONLY_NO_CLOSE");
#endif
        transferred=false;transfer_failed=false;commands=VK_NULL_HANDLE;
#ifdef _WIN32
        if(library){FreeLibrary(library);library=nullptr;}
#endif
    }
    void create_transfer_buffer(VkBuffer& buffer,VkDeviceMemory& allocation,void*& mapped) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size=uint64_t(width)*height*4;info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        require(CreateBuffer(device,&info,nullptr,&buffer),"vkCreateBuffer/transfer",info.size);
        VkMemoryRequirements requirements{};GetBufferMemoryRequirements(device,buffer,&requirements);
        VkPhysicalDeviceMemoryProperties memory{};GetPhysicalDeviceMemoryProperties(physical,&memory);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) &&
            (memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
        if(type==UINT32_MAX)throw std::runtime_error("No coherent host-visible transfer memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
        require(AllocateMemory(device,&ai,nullptr,&allocation),"vkAllocateMemory/transfer",requirements.size);
        require(BindBufferMemory(device,buffer,allocation,0),"vkBindBufferMemory/transfer",requirements.size);
        require(MapMemory(device,allocation,0,VK_WHOLE_SIZE,0,&mapped),"vkMapMemory/transfer",requirements.size);
        char line[192];std::snprintf(line,sizeof(line),"GPU_TRANSFER_MEMORY type=%u flags=%08x bytes=%llu mapped=%016llx",type,
            memory.memoryTypes[type].propertyFlags,(unsigned long long)requirements.size,(unsigned long long)uintptr_t(mapped));log(line);
    }
#ifdef __SWITCH__
    void create_display() {
        if(swapchain)return;
        nx_runtime_acquire_display();
        VkViSurfaceCreateInfoNN vi{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};vi.window=nwindowGetDefault();
        require(CreateViSurfaceNN(instance,&vi,nullptr,&surface),"vkCreateViSurfaceNN");
        VkBool32 supported=VK_FALSE;
        require(GetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported),"vkGetPhysicalDeviceSurfaceSupportKHR");
        if(!supported)throw std::runtime_error("Graphics queue cannot present to VI");
        VkSurfaceCapabilitiesKHR caps{};
        require(GetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        char capabilities[256];std::snprintf(capabilities,sizeof(capabilities),"GPU_VI_CAPS width=%u height=%u min_images=%u max_images=%u usage=%08x transforms=%08x current_transform=%08x alpha=%08x",
            caps.currentExtent.width,caps.currentExtent.height,caps.minImageCount,caps.maxImageCount,caps.supportedUsageFlags,caps.supportedTransforms,caps.currentTransform,caps.supportedCompositeAlpha);log(capabilities);
        uint32_t count=0;require(GetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"vkGetPhysicalDeviceSurfaceFormatsKHR/count");
        std::vector<VkSurfaceFormatKHR> formats(count);
        require(GetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"vkGetPhysicalDeviceSurfaceFormatsKHR/list");
        bool found=false;VkSurfaceFormatKHR format{};
        for(auto candidate:formats) {
            char description[128];std::snprintf(description,sizeof(description),"GPU_VI_FORMAT format=%u colorspace=%u",candidate.format,candidate.colorSpace);log(description);
            if(candidate.format==VK_FORMAT_R8G8B8A8_UNORM && candidate.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR){format=candidate;found=true;}
        }
        if(!found || !(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("VI surface lacks RGBA8 transfer presentation");
        display_extent=caps.currentExtent;
        if(display_extent.width==UINT32_MAX)display_extent={std::clamp(width,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(height,caps.minImageExtent.height,caps.maxImageExtent.height)};
        if(display_extent.width!=width || display_extent.height!=height)throw std::runtime_error("VI surface size differs from guest front buffer");
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface=surface;info.minImageCount=std::max(2u,caps.minImageCount);
        if(caps.maxImageCount)info.minImageCount=std::min(info.minImageCount,caps.maxImageCount);
        info.imageFormat=format.format;info.imageColorSpace=format.colorSpace;info.imageExtent=display_extent;
        info.imageArrayLayers=1;info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;info.preTransform=caps.currentTransform;
        for(auto bit:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})if(caps.supportedCompositeAlpha&bit){info.compositeAlpha=bit;break;}
        info.presentMode=VK_PRESENT_MODE_FIFO_KHR;info.clipped=VK_TRUE;
        require(CreateSwapchainKHR(device,&info,nullptr,&swapchain),"vkCreateSwapchainKHR");
        require(GetSwapchainImagesKHR(device,swapchain,&count,nullptr),"vkGetSwapchainImagesKHR/count");
        display_images.resize(count);rendered.resize(count,VK_NULL_HANDLE);
        require(GetSwapchainImagesKHR(device,swapchain,&count,display_images.data()),"vkGetSwapchainImagesKHR/list");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        require(CreateSemaphore(device,&semaphore,nullptr,&acquired),"vkCreateSemaphore/acquire");
        for(auto& handle:rendered)require(CreateSemaphore(device,&semaphore,nullptr,&handle),"vkCreateSemaphore/present");
        message("DISPLAY_READY",0,display_images.size());
    }
#endif
};

VulkanDevice::VulkanDevice(Log log):impl_(new Impl{}){impl_->log=log;}
VulkanDevice::~VulkanDevice(){try{impl_->close();}catch(...){/* retain ownership until process exit */}}
bool VulkanDevice::ready() const{return impl_->initialized;}
bool VulkanDevice::has_display_frame() const{return impl_->initialized && impl_->transferred;}
void VulkanDevice::wait_vblank() {
    auto& s=*impl_;
    if(!has_display_frame())throw std::runtime_error("Vblank requested before a completed display transfer");
#ifdef __SWITCH__
    auto* display=nx_runtime_default_display();
    if(!display)throw std::runtime_error("Default native-window display unavailable for vblank");
    if(!s.vsync_open){
        char line[192];std::snprintf(line,sizeof(line),"VBLANK_DISPLAY_BORROW owner=LIBNX_DEFAULT_WINDOW display_id=%llu initialized=%u address=%016llx",(unsigned long long)display->display_id,display->initialized,(unsigned long long)uintptr_t(display));s.log(line);
        const Result rc=viGetDisplayVsyncEvent(display,&s.vsync);s.message("viGetDisplayVsyncEvent",rc);
        if(R_FAILED(rc))throw std::runtime_error("VI vsync event unavailable");s.vsync_open=true;
        std::snprintf(line,sizeof(line),"VBLANK_EVENT_CONTRACT read_handle=%08x write_handle=%08x autoclear=%u active=%u timeout_ns=100000000",s.vsync.revent,s.vsync.wevent,s.vsync.autoclear,eventActive(&s.vsync));s.log(line);
        if(!eventActive(&s.vsync) || !s.vsync.autoclear || s.vsync.wevent!=INVALID_HANDLE)
            throw std::runtime_error("VI remote autoclear event contract mismatch");
    }
    // viGetDisplayVsyncEvent loads a remote autoclear Event. eventWait waits
    // for a signal before resetting it and handles a competing reset race.
    // eventClear on an unsignalled read handle returns InvalidState; never
    // manually reset this event (libnx event.h explicitly forbids that use).
    const auto started=armGetSystemTick();
    const Result waited=eventWait(&s.vsync,100000000);s.message("eventWait/vblank",waited);
    char waited_state[256];std::snprintf(waited_state,sizeof(waited_state),"VBLANK_EVENT_WAIT rc=%08x module=%u description=%u read_handle=%08x autoclear=%u elapsed_ns=%llu timeout_ns=100000000",waited,R_MODULE(waited),R_DESCRIPTION(waited),s.vsync.revent,s.vsync.autoclear,(unsigned long long)armTicksToNs(armGetSystemTick()-started));s.log(waited_state);
    if(R_FAILED(waited))throw std::runtime_error("VI vsync event wait failed");
    s.message("VBLANK_SOURCE_HORIZON_VI_EVENT",0,++s.vblanks);
#else
    const auto now=std::chrono::steady_clock::now();const auto period=std::chrono::nanoseconds(16666667);
    if(s.next_vblank.time_since_epoch().count()==0 || s.next_vblank<now)s.next_vblank=now+period;
    std::this_thread::sleep_until(s.next_vblank);s.next_vblank+=period;
    s.message("VBLANK_SOURCE_HOST_VIRTUAL_60HZ",0,++s.vblanks);
#endif
}
void VulkanDevice::shutdown(){impl_->close();}
void VulkanDevice::draw_masked_point(const uint32_t* vertex,size_t vertex_words,const uint32_t* pixel,size_t pixel_words) {
    auto& s=*impl_;
    if(!ready() || s.transfer_failed || !vertex || !pixel || vertex_words<5 || pixel_words<5 || s.width!=1280 || s.height!=720)
        throw std::runtime_error("Invalid masked-point pipeline state");
    try {
        if(!s.point_pipeline) {
            const uint32_t* code[2]={vertex,pixel};const size_t words[2]={vertex_words,pixel_words};
            for(unsigned stage=0;stage<2;++stage) {
                VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words[stage]*4;ci.pCode=code[stage];
                s.require(s.CreateShaderModule(s.device,&ci,nullptr,&s.point_shaders[stage]),"vkCreateShaderModule/point",ci.codeSize);
            }
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,24};
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;
            s.require(s.CreatePipelineLayout(s.device,&li,nullptr,&s.point_layout),"vkCreatePipelineLayout/point");
            // This title draw has no color, depth, stencil, memory export or
            // texture side effects. A render pass with no attachments preserves
            // existing EDRAM while still executing the vertex/fragment stages.
            VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.subpassCount=1;rp.pSubpasses=&subpass;
            s.require(s.CreateRenderPass(s.device,&rp,nullptr,&s.point_pass),"vkCreateRenderPass/point");
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=s.point_pass;fb.width=s.width;fb.height=s.height;fb.layers=1;
            s.require(s.CreateFramebuffer(s.device,&fb,nullptr,&s.point_framebuffer),"vkCreateFramebuffer/point");
            VkPipelineShaderStageCreateInfo stages[2]{};
            for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=s.point_shaders[i];stages[i].pName="main";}
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            // The exact shader outputs screen position (0,0). Viewport transform
            // is disabled on Xenos, whose integer pixel centre adds 0.5. This
            // viewport maps that fixed NDC zero to native pixel centre (0.5,0.5).
            VkViewport viewport{0.5f-float(s.width)/2,0.5f-float(s.height)/2,float(s.width),float(s.height),0,1};
            VkRect2D scissor{{0,0},{16,16}};
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
            pi.pViewportState=&vp;pi.pRasterizationState=&raster;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;pi.pColorBlendState=&blend;pi.layout=s.point_layout;pi.renderPass=s.point_pass;
            s.require(s.CreateGraphicsPipelines(s.device,VK_NULL_HANDLE,1,&pi,nullptr,&s.point_pipeline),"vkCreateGraphicsPipelines/point");
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=512;bi.usage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            s.require(s.CreateBuffer(s.device,&bi,nullptr,&s.point_constants),"vkCreateBuffer/point-constants",512);
            VkMemoryRequirements requirements{};s.GetBufferMemoryRequirements(s.device,s.point_constants,&requirements);
            VkPhysicalDeviceMemoryProperties memory{};s.GetPhysicalDeviceMemoryProperties(s.physical,&memory);uint32_t type=UINT32_MAX;
            for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) &&
                (memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
            if(type==UINT32_MAX)throw std::runtime_error("No coherent point constant memory");
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.pNext=&flags;ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
            s.require(s.AllocateMemory(s.device,&ai,nullptr,&s.point_memory),"vkAllocateMemory/point-constants",requirements.size);
            s.require(s.BindBufferMemory(s.device,s.point_constants,s.point_memory,0),"vkBindBufferMemory/point-constants");
            s.require(s.MapMemory(s.device,s.point_memory,0,VK_WHOLE_SIZE,0,&s.point_map),"vkMapMemory/point-constants",512);std::memset(s.point_map,0,512);
            VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};address.buffer=s.point_constants;s.point_address=s.GetBufferDeviceAddress(s.device,&address);
            if(!s.point_address)throw std::runtime_error("Missing point constant device address");
            char location[192];std::snprintf(location,sizeof(location),"GPU_POINT_CONSTANTS host=%016llx device=%016llx bytes=512 type=%u flags=%08x",(unsigned long long)uintptr_t(s.point_map),(unsigned long long)s.point_address,type,memory.memoryTypes[type].propertyFlags);s.log(location);
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_OCCLUSION;qi.queryCount=1;
            s.require(s.CreateQueryPool(s.device,&qi,nullptr,&s.point_query),"vkCreateQueryPool/point");
        }
        if(!s.commands) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=s.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
            s.require(s.AllocateCommandBuffers(s.device,&ai,&s.commands),"vkAllocateCommandBuffers/point");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};s.require(s.CreateFence(s.device,&fi,nullptr,&s.fence),"vkCreateFence/point");
        }
        s.require(s.ResetCommandPool(s.device,s.pool,0),"vkResetCommandPool/point");s.require(s.ResetFences(s.device,1,&s.fence),"vkResetFences/point");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        s.require(s.BeginCommandBuffer(s.commands,&begin),"vkBeginCommandBuffer/point");s.CmdResetQueryPool(s.commands,s.point_query,0,1);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=s.point_constants;host.size=VK_WHOLE_SIZE;
        s.CmdPipelineBarrier(s.commands,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&host,0,nullptr);
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=s.point_pass;rp.framebuffer=s.point_framebuffer;rp.renderArea={{0,0},{s.width,s.height}};
        s.CmdBeginRenderPass(s.commands,&rp,VK_SUBPASS_CONTENTS_INLINE);s.CmdBindPipeline(s.commands,VK_PIPELINE_BIND_POINT_GRAPHICS,s.point_pipeline);
        const uint64_t constants[3]={0,0,s.point_address};s.CmdPushConstants(s.commands,s.point_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,24,constants);
        s.CmdBeginQuery(s.commands,s.point_query,0,0);s.CmdDraw(s.commands,1,1,0,0);s.CmdEndQuery(s.commands,s.point_query,0);s.CmdEndRenderPass(s.commands);
        s.require(s.EndCommandBuffer(s.commands),"vkEndCommandBuffer/point");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&s.commands;
        s.require(s.QueueSubmit(s.queue,1,&submit,s.fence),"vkQueueSubmit/point");s.require(s.WaitForFences(s.device,1,&s.fence,VK_TRUE,3000000000ull),"vkWaitForFences/point");
        uint64_t coverage=0;s.require(s.GetQueryPoolResults(s.device,s.point_query,0,1,sizeof(coverage),&coverage,sizeof(coverage),VK_QUERY_RESULT_64_BIT),"vkGetQueryPoolResults/point",sizeof(coverage));
        if(!coverage)throw std::runtime_error("Masked point produced no native coverage");
        s.message("GPU_MASKED_POINT_COMPLETE_NO_ATTACHMENT_WRITES",0,coverage);
    }catch(...){s.transfer_failed=true;throw;}
}
#include "video_pipeline.inc"
void VulkanDevice::synchronize(){
    if(!ready())throw std::runtime_error("Native Vulkan device not ready for synchronization");
    impl_->require(impl_->DeviceWaitIdle(impl_->device),"vkDeviceWaitIdle/synchronize");
}

void VulkanDevice::clear_depth(uint32_t tile,uint32_t width,uint32_t height,uint32_t samples,float depth,uint32_t stencil) {
    auto& s=*impl_;
    if(!ready() || s.transfer_failed || !width || !height || width>4096 || height>4096 || samples!=4 || depth!=0.0f || stencil)
        throw std::runtime_error("Unsupported native startup depth clear");
    if(s.depth_image && (tile!=s.depth_tile || width!=s.depth_width || height!=s.depth_height || samples!=s.depth_samples))
        throw std::runtime_error("Multiple native EDRAM depth surfaces require ownership/alias tracking");
    try {
        if(!s.depth_image) {
            VkImageFormatProperties supported{};
            s.require(s.GetPhysicalDeviceImageFormatProperties(s.physical,VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&supported),"vkGetPhysicalDeviceImageFormatProperties/depth");
            if(!(supported.sampleCounts&VK_SAMPLE_COUNT_4_BIT) || width>supported.maxExtent.width || height>supported.maxExtent.height)
                throw std::runtime_error("Native D24S8 image lacks required 4x MSAA geometry");
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_D24_UNORM_S8_UINT;
            info.extent={width,height,1};info.mipLevels=1;info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_4_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
            info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            s.require(s.CreateImage(s.device,&info,nullptr,&s.depth_image),"vkCreateImage/edram-depth");
            s.depth_tile=tile;s.depth_width=width;s.depth_height=height;s.depth_samples=samples;
            VkMemoryRequirements requirements{};s.GetImageMemoryRequirements(s.device,s.depth_image,&requirements);
            VkPhysicalDeviceMemoryProperties memory{};s.GetPhysicalDeviceMemoryProperties(s.physical,&memory);uint32_t type=UINT32_MAX;
            for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
            if(type==UINT32_MAX)throw std::runtime_error("No native EDRAM depth memory type");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=type;
            s.require(s.AllocateMemory(s.device,&ai,nullptr,&s.depth_memory),"vkAllocateMemory/edram-depth",requirements.size);
            s.require(s.BindImageMemory(s.device,s.depth_image,s.depth_memory,0),"vkBindImageMemory/edram-depth",requirements.size);
        }
        if(!s.commands) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=s.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
            s.require(s.AllocateCommandBuffers(s.device,&ai,&s.commands),"vkAllocateCommandBuffers/depth");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};s.require(s.CreateFence(s.device,&fi,nullptr,&s.fence),"vkCreateFence/depth");
        }
        s.require(s.ResetCommandPool(s.device,s.pool,0),"vkResetCommandPool/depth");s.require(s.ResetFences(s.device,1,&s.fence),"vkResetFences/depth");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        s.require(s.BeginCommandBuffer(s.commands,&begin),"vkBeginCommandBuffer/depth");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=s.depth_image;
        barrier.oldLayout=s.depth_initialized?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask=s.depth_initialized?VK_ACCESS_TRANSFER_WRITE_BIT:0;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
        s.CmdPipelineBarrier(s.commands,s.depth_initialized?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkClearDepthStencilValue clear{depth,stencil};s.CmdClearDepthStencilImage(s.commands,s.depth_image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&barrier.subresourceRange);
        s.require(s.EndCommandBuffer(s.commands),"vkEndCommandBuffer/depth");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&s.commands;
        s.require(s.QueueSubmit(s.queue,1,&submit,s.fence),"vkQueueSubmit/edram-depth");
        s.require(s.WaitForFences(s.device,1,&s.fence,VK_TRUE,3000000000ull),"vkWaitForFences/edram-depth");s.depth_initialized=true;
        char line[256];std::snprintf(line,sizeof(line),"GPU_EDRAM_DEPTH_CLEAR_COMPLETE tile=%u width=%u height=%u samples=%u depth=%g stencil=%u ownership=NATIVE_DEPTH_IMAGE",tile,width,height,samples,depth,stencil);s.log(line);
    }catch(...){s.transfer_failed=true;throw;}
}

void VulkanDevice::display_rgba(const std::vector<uint8_t>& pixels,uint32_t width,uint32_t height) {
    auto& s=*impl_;
    if(!ready() || s.transfer_failed || width!=s.width || height!=s.height || pixels.size()!=uint64_t(width)*height*4)
        throw std::runtime_error("Invalid native display transfer state/geometry");
    // Host validation checks every frame. On Switch the same transfer path
    // was already measured; retain a first-frame check, then send pixels
    // directly to VI without an unused offscreen image and CPU readback.
#ifdef __SWITCH__
    const bool verify_transfer=!s.transferred;
#else
    const bool verify_transfer=true;
#endif
    try {
        if(!s.upload)s.create_transfer_buffer(s.upload,s.upload_memory,s.upload_map);
        if(verify_transfer && !s.readback)s.create_transfer_buffer(s.readback,s.readback_memory,s.readback_map);
        if(!s.commands) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool=s.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
            s.require(s.AllocateCommandBuffers(s.device,&ai,&s.commands),"vkAllocateCommandBuffers/display");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            s.require(s.CreateFence(s.device,&fi,nullptr,&s.fence),"vkCreateFence/display");
        }
#ifdef __SWITCH__
        s.create_display();uint32_t image_index=0;
        // Bound a stalled native compositor; do not block shutdown forever.
        const auto acquire=s.AcquireNextImageKHR(s.device,s.swapchain,3000000000ull,s.acquired,VK_NULL_HANDLE,&image_index);
        s.message("vkAcquireNextImageKHR",acquire);
        if(acquire!=VK_SUCCESS && acquire!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("VI image acquisition failed: "+std::to_string(acquire));
        if(image_index>=s.display_images.size())throw std::runtime_error("VI image index outside swapchain");
#endif
        std::memcpy(s.upload_map,pixels.data(),pixels.size());
        s.require(s.ResetCommandPool(s.device,s.pool,0),"vkResetCommandPool/display");
        s.require(s.ResetFences(s.device,1,&s.fence),"vkResetFences/display");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        s.require(s.BeginCommandBuffer(s.commands,&begin),"vkBeginCommandBuffer/display");
        auto transition=[&](VkImage image,VkImageLayout old_layout,VkImageLayout new_layout,VkAccessFlags source,VkAccessFlags destination,
                            VkPipelineStageFlags source_stage,VkPipelineStageFlags destination_stage) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=source;barrier.dstAccessMask=destination;
            barrier.oldLayout=old_layout;barrier.newLayout=new_layout;barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            s.CmdPipelineBarrier(s.commands,source_stage,destination_stage,0,0,nullptr,0,nullptr,1,&barrier);
        };
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={width,height,1};
        if(verify_transfer) {
        transition(s.color,s.transferred?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            s.transferred?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT,s.transferred?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);

        s.CmdCopyBufferToImage(s.commands,s.upload,s.color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        transition(s.color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        s.CmdCopyImageToBuffer(s.commands,s.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.readback,1,&region);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=s.readback;host.size=VK_WHOLE_SIZE;
        s.CmdPipelineBarrier(s.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        }
#ifdef __SWITCH__
        transition(s.display_images[image_index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        // The source and VI images have the same size and format. A buffer copy
        // avoids blit format/filter feature requirements on this presentation path.
        s.CmdCopyBufferToImage(s.commands,s.upload,s.display_images[image_index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        transition(s.display_images[image_index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
#endif
        s.require(s.EndCommandBuffer(s.commands),"vkEndCommandBuffer/display");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&s.commands;
#ifdef __SWITCH__
        const VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&s.acquired;submit.pWaitDstStageMask=&wait_stage;
        submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&s.rendered[image_index];
#endif
        s.require(s.QueueSubmit(s.queue,1,&submit,s.fence),"vkQueueSubmit/display",pixels.size());
        s.require(s.WaitForFences(s.device,1,&s.fence,VK_TRUE,3000000000ull),"vkWaitForFences/display");
        if(verify_transfer && std::memcmp(s.readback_map,pixels.data(),pixels.size())) {
            const auto actual=static_cast<const uint8_t*>(s.readback_map);size_t at=0;
            while(at<pixels.size() && actual[at]==pixels[at])++at;
            char error[256];std::snprintf(error,sizeof(error),"GPU_TRANSFER_MISMATCH offset=%llu expected=%02x actual=%02x source=%016llx destination=%016llx bytes=%llu",
                (unsigned long long)at,pixels[at],actual[at],(unsigned long long)uintptr_t(s.upload_map),(unsigned long long)uintptr_t(s.readback_map),(unsigned long long)pixels.size());
            s.log(error);throw std::runtime_error("GPU display readback differs from guest pixels");
        }
#ifdef __SWITCH__
        if(verify_transfer)s.message("DISPLAY_READBACK_FIRST_FRAME_MATCH_FOLLOWING_FRAMES_DIRECT_TO_VI",0,pixels.size());
#endif
        s.transferred=true;
#ifndef __SWITCH__
        // A private capture of real, completed guest presentation, never a
        // replacement image. Wait past the initial black movie frames.
        static bool captured=false;
        if(!captured && std::count_if(pixels.begin(),pixels.end(),[](uint8_t b){return b>64;})>size_t(width)*height+30000) {
            std::ofstream frame("generated/captures/video/presented.ppm",std::ios::binary);
            frame<<"P6\n"<<width<<' '<<height<<"\n255\n";
            for(size_t i=0;i<pixels.size();i+=4)frame.write(reinterpret_cast<const char*>(pixels.data()+i),3);
            if(!frame)throw std::runtime_error("Actual presented-frame capture failed");
            captured=true;s.message("GPU_ACTUAL_PRESENTED_FRAME_CAPTURED",0,pixels.size());
        }
#endif
#ifdef __SWITCH__
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&s.rendered[image_index];
        present.swapchainCount=1;present.pSwapchains=&s.swapchain;present.pImageIndices=&image_index;
        const auto result=s.QueuePresentKHR(s.queue,&present);s.message("vkQueuePresentKHR",result);
        if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR)throw std::runtime_error("VI presentation failed: "+std::to_string(result));
        s.message("DISPLAY_QUEUED_GUEST_PIXELS",0,pixels.size());
#else
        s.message("DISPLAY_OFFSCREEN_READBACK_MATCH_NO_WINDOW",0,pixels.size());
#endif
    }catch(...){s.transfer_failed=true;throw;}
}

void VulkanDevice::initialize(uint32_t width,uint32_t height) {
    auto& s=*impl_;
    if(s.device || s.instance)throw std::runtime_error("Graphics engine already initialized");
    if(!width || !height || width>4096 || height>4096)throw std::runtime_error("Invalid native framebuffer size");
    try {
#ifdef _WIN32
        // Use the system loader; do not search the current game's directory.
        s.library=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!s.library)throw std::runtime_error("System Vulkan loader unavailable");
        s.get_instance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(s.library,"vkGetInstanceProcAddr"));
#else
        s.get_instance=vkGetInstanceProcAddr; // Loaderless Mesa NVK static archive.
#endif
        if(!s.get_instance)throw std::runtime_error("vkGetInstanceProcAddr unavailable");
        auto create=reinterpret_cast<PFN_vkCreateInstance>(s.get_instance(VK_NULL_HANDLE,"vkCreateInstance"));
        if(!create)throw std::runtime_error("vkCreateInstance unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="AirborneNX";app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
#ifdef __SWITCH__
        const char* instance_extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_NN_VI_SURFACE_EXTENSION_NAME};
        info.enabledExtensionCount=2;info.ppEnabledExtensionNames=instance_extensions;
#endif
        s.require(create(&info,nullptr,&s.instance),"vkCreateInstance");
#define LOAD_INSTANCE(name) s.name=reinterpret_cast<PFN_vk##name>(s.get_instance(s.instance,"vk" #name)); \
        if(!s.name)throw std::runtime_error("Missing Vulkan instance function: " #name);
        GPU_INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#ifdef __SWITCH__
        GPU_SURFACE_FUNCTIONS(LOAD_INSTANCE)
#endif
#undef LOAD_INSTANCE
        s.get_device=reinterpret_cast<PFN_vkGetDeviceProcAddr>(s.get_instance(s.instance,"vkGetDeviceProcAddr"));
        if(!s.get_device)throw std::runtime_error("vkGetDeviceProcAddr unavailable");
        uint32_t count=0;
        s.require(s.EnumeratePhysicalDevices(s.instance,&count,nullptr),"vkEnumeratePhysicalDevices/count");
        if(!count)throw std::runtime_error("No Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(count);
        s.require(s.EnumeratePhysicalDevices(s.instance,&count,devices.data()),"vkEnumeratePhysicalDevices/list");
        uint32_t family=UINT32_MAX;
        for(auto candidate:devices) {
            VkPhysicalDeviceProperties properties{};s.GetPhysicalDeviceProperties(candidate,&properties);
            VkPhysicalDeviceVulkan12Features extra{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&extra;
            s.GetPhysicalDeviceFeatures2(candidate,&features);
            char line[512];std::snprintf(line,sizeof(line),
                "GPU_ADAPTER name=%s api=%u vendor=%04x device=%04x int64=%u bda=%u runtime_array=%u gather=%u sampled_dynamic=%u",
                properties.deviceName,properties.apiVersion,properties.vendorID,properties.deviceID,
                features.features.shaderInt64,extra.bufferDeviceAddress,extra.runtimeDescriptorArray,
                features.features.shaderImageGatherExtended,features.features.shaderSampledImageArrayDynamicIndexing);s.log(line);
            if(properties.apiVersion<VK_API_VERSION_1_2 || !features.features.shaderInt64 ||
               !features.features.shaderImageGatherExtended || !features.features.shaderSampledImageArrayDynamicIndexing ||
               !extra.bufferDeviceAddress || !extra.runtimeDescriptorArray)continue;
            uint32_t families=0;s.GetPhysicalDeviceQueueFamilyProperties(candidate,&families,nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            s.GetPhysicalDeviceQueueFamilyProperties(candidate,&families,queues.data());
            for(uint32_t index=0;index<families;++index)if(queues[index].queueCount && (queues[index].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {
                s.physical=candidate;family=index;break;
            }
            if(s.physical)break;
        }
        if(!s.physical)throw std::runtime_error("No Vulkan device supports current translated shader ABI");
        s.family=family;s.width=width;s.height=height;
        float priority=1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan12Features extra{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        extra.bufferDeviceAddress=VK_TRUE;extra.runtimeDescriptorArray=VK_TRUE;
        VkPhysicalDeviceFeatures features{};features.shaderInt64=VK_TRUE;
        features.shaderImageGatherExtended=VK_TRUE;features.shaderSampledImageArrayDynamicIndexing=VK_TRUE;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.pNext=&extra;
        device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queue;device.pEnabledFeatures=&features;
#ifdef __SWITCH__
        const char* device_extensions[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        device.enabledExtensionCount=1;device.ppEnabledExtensionNames=device_extensions;
#endif
        s.require(s.CreateDevice(s.physical,&device,nullptr,&s.device),"vkCreateDevice");
#define LOAD_DEVICE(name) s.name=reinterpret_cast<PFN_vk##name>(s.get_device(s.device,"vk" #name)); \
        if(!s.name)throw std::runtime_error("Missing Vulkan device function: " #name);
        GPU_DEVICE_FUNCTIONS(LOAD_DEVICE)
#ifdef __SWITCH__
        GPU_PRESENT_FUNCTIONS(LOAD_DEVICE)
#endif
#undef LOAD_DEVICE
        s.GetDeviceQueue(s.device,family,0,&s.queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex=family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        s.require(s.CreateCommandPool(s.device,&pool,nullptr,&s.pool),"vkCreateCommandPool");
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R8G8B8A8_UNORM;
        image.extent={width,height,1};image.mipLevels=1;image.arrayLayers=1;
        image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        s.require(s.CreateImage(s.device,&image,nullptr,&s.color),"vkCreateImage");
        VkMemoryRequirements requirements{};s.GetImageMemoryRequirements(s.device,s.color,&requirements);
        VkPhysicalDeviceMemoryProperties memory{};s.GetPhysicalDeviceMemoryProperties(s.physical,&memory);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) &&
            (memory.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
        if(type==UINT32_MAX)throw std::runtime_error("No native framebuffer memory type");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
        s.require(s.AllocateMemory(s.device,&allocation,nullptr,&s.color_memory),"vkAllocateMemory/color",requirements.size);
        s.require(s.BindImageMemory(s.device,s.color,s.color_memory,0),"vkBindImageMemory/color",requirements.size);
        s.initialized=true;
        char ready[192];std::snprintf(ready,sizeof(ready),
#ifdef __SWITCH__
            "GPU_NATIVE_READY api=Vulkan target=%ux%u presentation=VI_LAZY_UNTESTED xenos_draws=NOT_IMPLEMENTED",
#else
            "GPU_NATIVE_READY api=Vulkan target=%ux%u presentation=OFFSCREEN_READBACK xenos_draws=NOT_IMPLEMENTED",
#endif
            width,height);s.log(ready);
    } catch(...) {
        // Preserve the original initialization failure if cleanup also fails.
        try{s.close();}catch(const std::exception& error){s.log(error.what());}
        throw;
    }
}
}
