#include "../../runtime/gpu/vulkan_device.h"
#include "video_graphics.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <stdexcept>

static float sample(const std::vector<uint8_t>& p,int w,int h,float u,float v) {
    float x=u*w-.5f,y=v*h-.5f;int a=int(std::floor(x)),b=int(std::floor(y));float fx=x-a,fy=y-b;
    auto at=[&](int i,int j){return p[std::clamp(j,0,h-1)*w+std::clamp(i,0,w-1)]/255.f;};
    return (at(a,b)*(1-fx)+at(a+1,b)*fx)*(1-fy)+(at(a,b+1)*(1-fx)+at(a+1,b+1)*fx)*fy;
}
int main() {
    try {
        airborne::VulkanDevice device([](const char* line){std::puts(line);});device.initialize(1280,720);
        std::vector<uint8_t> planes[3];
        for(int run=0;run<3;++run) {
            for(int i=0;i<3;++i) {
                int w=i?640:1280,h=i?360:720,pitch=i?768:1280;planes[i].resize(w*h);
                if(!run){std::ifstream f("generated/captures/video/plane"+std::to_string(i)+".bin",std::ios::binary);if(!f)throw std::runtime_error("Missing actual movie capture");for(int y=0;y<h;++y){f.read((char*)planes[i].data()+y*w,w);f.seekg(pitch-w,std::ios::cur);}if(!f)throw std::runtime_error("Truncated actual movie capture");}
                else for(int y=0;y<h;++y)for(int x=0;x<w;++x)planes[i][y*w+x]=uint8_t(run==1?(i==0?16+(x*219/w):(i==1?32+y*192/h:224-y*192/h)):(i==0?235:128));
            }
            const auto output=device.draw_video(video_spirv_0,std::size(video_spirv_0),video_spirv_1,std::size(video_spirv_1),planes);
            int worst=0;size_t bad=0;int first_x=-1,first_y=-1,first_c=-1,first_expected=0,first_actual=0;
            for(int y=0;y<720;++y)for(int x=0;x<1280;++x) {
                float u=x/1280.f,v=y/720.f;
                const float yy=sample(planes[0],1280,720,u,v)-.0625f,uu=sample(planes[1],640,360,u,v)-.5f,vv=sample(planes[2],640,360,u,v)-.5f;
                const float expected[4]={yy*1.16438281536f+vv*1.7926523685f,vv*-.5330039263f+uu*-.213238284f+yy*1.16438281536f,yy*1.16438281536f+uu*2.1124179363f,1};
                for(int c=0;c<4;++c){int a=output[(y*1280+x)*4+c],e=int(std::lround(std::clamp(expected[c],0.f,1.f)*255));int difference=std::abs(a-e);worst=std::max(worst,difference);if(difference>2){if(!bad){first_x=x;first_y=y;first_c=c;first_expected=e;first_actual=a;}++bad;}}
            }
            std::printf("VIDEO_COMPARE case=%s pixels=921600 worst_byte_error=%d mismatched_components=%zu first=%d,%d,%d expected=%d actual=%d\n",run==0?"ACTUAL_GAME_FRAME":run==1?"CHROMA_LUMA_GRADIENT":"FULL_WHITE_REUSE",worst,bad,first_x,first_y,first_c,first_expected,first_actual);
            if(bad)throw std::runtime_error("Native movie pixels differ from shader reference");
        }
        device.shutdown();std::puts("VIDEO_PIPELINE_PASS actual_shaders=2 frames=3 pixels_compared=2764800 cleanup=COMPLETE");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"VIDEO_PIPELINE_FAIL %s\n",e.what());return 1;}
}
