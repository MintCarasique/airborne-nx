// libnx owns the default display for the entire native-window lifetime. Capture
// that successful open instead of attempting a second VI OpenDisplay request.
// --wrap redirects both application and static libnx references at link time.
#include <switch/services/vi.h>
#include <cstring>

namespace { ViDisplay* default_display=nullptr; }
extern "C" Result __real_viOpenDisplay(const char*,ViDisplay*);
extern "C" Result __real_viCloseDisplay(ViDisplay*);
extern "C" Result __wrap_viOpenDisplay(const char* name,ViDisplay* display) {
    const Result result=__real_viOpenDisplay(name,display);
    if(R_SUCCEEDED(result) && name && !std::strcmp(name,"Default") && display && display->initialized)
        default_display=display;
    return result;
}
extern "C" Result __wrap_viCloseDisplay(ViDisplay* display) {
    const Result result=__real_viCloseDisplay(display);
    if(R_SUCCEEDED(result) && default_display==display)default_display=nullptr;
    return result;
}
ViDisplay* nx_runtime_default_display() {
    return default_display && default_display->initialized?default_display:nullptr;
}
