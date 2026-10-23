#include "../../runtime/gpu/front_buffer.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <initializer_list>
uint32_t reference(const airborne::FrontBuffer& f){uint32_t n=0;for(uint32_t y=0;y<f.height;++y)for(uint32_t x=0;x<f.width;++x)n=std::max(n,f.offset(x,y)+4);return n;}
int main(){
 unsigned cases=0;auto check=[&](uint32_t w,uint32_t h,uint32_t pitch,bool tiled){airborne::FrontBuffer f{w,h,pitch,0,tiled};if(f.required_bytes()!=reference(f))throw std::runtime_error("extent mismatch");++cases;};
 for(bool tiled:{false,true})for(uint32_t w=1;w<=97;++w)for(uint32_t h=1;h<=97;++h)for(uint32_t pad:{0u,32u})check(w,h,((w+31)&~31u)+pad,tiled);
 for(bool tiled:{false,true})for(uint32_t w:{1279u,1280u,1281u,4096u})for(uint32_t h:{719u,720u,721u,4096u})check(w,h,(w+31)&~31u,tiled);
 airborne::FrontBuffer frame{1280,720,1280,0,true};volatile uint32_t sink=0;
 auto a=std::chrono::steady_clock::now();for(int i=0;i<50;++i)sink=reference(frame);auto b=std::chrono::steady_clock::now();for(int i=0;i<50;++i)sink=frame.required_bytes();auto c=std::chrono::steady_clock::now();
 std::printf("FRONT_EXTENT_PASS cases=%u exact_bytes=%u reference_50_us=%lld optimized_50_us=%lld PC_ONLY\n",cases,(uint32_t)sink,(long long)std::chrono::duration_cast<std::chrono::microseconds>(b-a).count(),(long long)std::chrono::duration_cast<std::chrono::microseconds>(c-b).count());
}
