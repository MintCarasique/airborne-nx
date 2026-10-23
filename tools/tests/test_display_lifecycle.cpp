#include <switch/services/vi.h>
#include <cstdio>
#include <stdexcept>
#include <string>
extern "C" Result __wrap_viOpenDisplay(const char*,ViDisplay*);
extern "C" Result __wrap_viCloseDisplay(ViDisplay*);
ViDisplay* nx_runtime_default_display();
static Result open_result=0,close_result=0;
static unsigned opens=0,closes=0;
extern "C" Result __real_viOpenDisplay(const char*,ViDisplay* display) {
    ++opens;*display={};if(!open_result){display->display_id=42;display->initialized=true;}return open_result;
}
extern "C" Result __real_viCloseDisplay(ViDisplay* display) {
    ++closes;if(!close_result)*display={};return close_result;
}
int main() {
    unsigned checks=0;
    auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("Display lifecycle check "+std::to_string(checks));};
    try {
        ViDisplay owned{},other{},failed{};
        check(nx_runtime_default_display()==nullptr);
        open_result=0x1272;check(__wrap_viOpenDisplay("Default",&failed)==0x1272);check(nx_runtime_default_display()==nullptr);
        open_result=0;check(__wrap_viOpenDisplay("External",&other)==0);check(nx_runtime_default_display()==nullptr);
        check(__wrap_viOpenDisplay("Default",&owned)==0);check(nx_runtime_default_display()==&owned);
        const auto before=opens;
        for(unsigned i=0;i<100;++i){auto* borrowed=nx_runtime_default_display();check(borrowed==&owned && borrowed->display_id==42);}
        check(opens==before && closes==0); // Repeated vblank access never opens/closes the owner.
        open_result=0x1272;check(__wrap_viOpenDisplay("Default",&failed)==0x1272);check(nx_runtime_default_display()==&owned);
        check(__wrap_viCloseDisplay(&other)==0 && nx_runtime_default_display()==&owned);
        close_result=7;check(__wrap_viCloseDisplay(&owned)==7);check(nx_runtime_default_display()==&owned);
        close_result=0;check(__wrap_viCloseDisplay(&owned)==0);check(nx_runtime_default_display()==nullptr);
        std::printf("DISPLAY_LIFECYCLE_PASS checks=%u repeated_borrows=100 extra_opens=0 borrowed_closes=0 failed_open_preserves_owner=1 owner_close_invalidates=1\n",checks);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"DISPLAY_LIFECYCLE_FAIL %s\n",e.what());return 1;}
}
