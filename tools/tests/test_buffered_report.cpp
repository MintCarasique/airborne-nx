#include "../../runtime/io/buffered_report.h"
#include <stdexcept>
#include <string>
#include <cstring>
#include <thread>

static bool emit(airborne::BufferedReport& writer,bool urgent,const char* format,...) {
    va_list args;va_start(args,format);const bool ok=writer.write(urgent,format,args);va_end(args);return ok;
}
static int injected_errno=0;
static int sink_flush(FILE* file){if(injected_errno){errno=injected_errno;return EOF;}return std::fflush(file);}
int main() {
    try {
        auto check=[](bool value){if(!value)throw std::runtime_error("Buffered report contract failed");};
        airborne::BufferedReport writer;FILE* file=std::tmpfile();check(file && writer.attach(file));
        for(unsigned i=0;i<50000;++i){check(emit(writer,false,"GPU_PACKET physical=%08x payload=%u\n",i*4,i%17));check(writer.flush(false));}
        check(emit(writer,true,"FIRST_ERROR operation=game_resource_read rc=1234\n"));
        const auto stats=writer.stats();check(stats.writes==50001 && stats.flushes<500 && !stats.first_error);
        std::rewind(file);char line[256];
        for(unsigned i=0;i<50000;++i){char expected[128];std::snprintf(expected,sizeof(expected),"GPU_PACKET physical=%08x payload=%u\n",i*4,i%17);check(std::fgets(line,sizeof(line),file) && !std::strcmp(line,expected));}
        check(std::fgets(line,sizeof(line),file) && !std::strcmp(line,"FIRST_ERROR operation=game_resource_read rc=1234\n"));
        check(std::fgetc(file)==EOF);writer.detach();std::fclose(file);
        std::printf("REPORT_REPLAY_PASS ordered_rows=50001 explicit_flushes=%llu critical_error_visible=1\n",(unsigned long long)stats.flushes);
        FILE* failure=std::tmpfile();check(failure && writer.attach(failure,sink_flush));
        injected_errno=EIO;check(!emit(writer,true,"ERROR_A\n"));check(writer.stats().first_error==EIO);
        injected_errno=ENOSPC;check(!writer.flush(true));check(writer.stats().first_error==EIO);
        injected_errno=0;check(!writer.flush(true));check(writer.stats().first_error==EIO);
        writer.detach();std::fclose(failure);std::puts("REPORT_FAILURE_PASS first_errno_preserved=1 later_success_does_not_erase_failure=1");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"REPORT_TEST_FAIL %s\n",e.what());return 1;}
}
