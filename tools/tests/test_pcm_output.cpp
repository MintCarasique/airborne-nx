#include "../../runtime/audio/pcm_output.h"
#include <cstdio>
#include <cstdarg>
static void log_message(const char* format,...) {
    va_list args;va_start(args,format);std::vprintf(format,args);va_end(args);
}
int main() {
    airborne::PcmOutput output(log_message);
    try {
        output.open();output.play({});output.close();
        std::puts("PCM_NATIVE_SILENT_FRAME_PASS frames=1 samples=256 rate=48000 channels=2");return 0;
    }catch(const std::exception& error) {
        std::fprintf(stderr,"PCM_NATIVE_FAIL %s\n",error.what());
        try{output.close();}catch(const std::exception& cleanup){std::fprintf(stderr,"PCM_NATIVE_CLEANUP_FAIL %s\n",cleanup.what());}
        return 1;
    }
}
