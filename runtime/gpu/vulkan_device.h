#pragma once
#include <memory>
#include <cstdint>
#include <vector>
#include <cstddef>

namespace airborne {
// Native Vulkan resources shared by the Windows development runtime and NVK on
// Horizon. Xenos packet/shader translation is deliberately outside this class.
class VulkanDevice {
public:
    using Log = void (*)(const char*);
    explicit VulkanDevice(Log log);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    void initialize(uint32_t width, uint32_t height);
    void shutdown();
    void synchronize();
    // Transfer actual decoded guest pixels; Windows verifies via GPU readback,
    // Horizon additionally queues the image to the native display.
    void display_rgba(const std::vector<uint8_t>& pixels,uint32_t width,uint32_t height);
    void clear_depth(uint32_t edram_tile,uint32_t width,uint32_t height,uint32_t samples,float depth,uint32_t stencil);
    // Fixed-position one-pixel point with all color/depth/stencil writes off.
    // Runs the actual shader pair; no EDRAM ownership transition is required.
    void draw_masked_point(const uint32_t* vertex,size_t vertex_words,const uint32_t* pixel,size_t pixel_words);
    // Exact six-vertex, planar R8 movie path. Returns the completed native
    // RGBA attachment for EDRAM ownership/resolve; this does not present early.
    std::vector<uint8_t> draw_video(const uint32_t* vertex,size_t vertex_words,
        const uint32_t* pixel,size_t pixel_words,const std::vector<uint8_t> (&planes)[3]);
    bool has_display_frame() const;
    void wait_vblank();
    bool ready() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
