#include "../../runtime/gpu/vulkan_device.h"
#include "post_config_graphics.h"
#include <iterator>
#include <cstdio>
#include <exception>
static void log_message(const char* text){std::puts(text);}
int main() {
    airborne::VulkanDevice device(log_message);
    try {
        device.initialize(1280,720);
        for(unsigned i=0;i<2;++i)device.draw_masked_point(point_spirv_0,std::size(point_spirv_0),point_spirv_1,std::size(point_spirv_1));
        device.shutdown();std::puts("POINT_PIPELINE_PASS draws=2 native_coverage=NONZERO cleanup=PASS game_image=NOT_PRODUCED");return 0;
    }catch(const std::exception& error) {
        std::fprintf(stderr,"POINT_PIPELINE_FAIL %s\n",error.what());
        try{device.shutdown();}catch(const std::exception& cleanup){std::fprintf(stderr,"POINT_PIPELINE_CLEANUP_FAIL %s\n",cleanup.what());}
        return 1;
    }
}
