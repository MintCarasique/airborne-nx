#pragma once
#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <string>
#include <system_error>
#include <sys/stat.h>
namespace airborne {
struct GamePath {std::filesystem::path native;uint32_t status=0;};
inline std::string ascii_lower(std::string text) {
    for(auto& c:text)if(c>='A' && c<='Z')c+=32;return text;
}
inline GamePath game_path(const std::filesystem::path& root,std::string guest) {
    for(auto& c:guest)if(c=='\\')c='/';
    if(guest.find('\0')!=std::string::npos)return {{},0xc0000033};
    const auto colon=guest.find(':');
    if(colon==std::string::npos)return {{},0xc0000033};
    const auto drive=ascii_lower(guest.substr(0,colon));
    if(drive!="game" && drive!="dvd" && drive!="d")return {{},0xc000000f};
    guest.erase(0,colon+1);auto path=root;
    size_t offset=0;
    while(offset<guest.size()) {
        auto end=guest.find('/',offset);if(end==std::string::npos)end=guest.size();
        const auto component=guest.substr(offset,end-offset);offset=end+1;
        if(component.empty() || component==".")continue;
        if(component==".." || component.find(':')!=std::string::npos)return {{},0xc0000033};
        std::error_code error;bool found=false;
        for(const auto& entry:std::filesystem::directory_iterator(path,error)) {
            if(ascii_lower(entry.path().filename().string())!=ascii_lower(component))continue;
            if(found)return {{},0xc0000033}; // Ambiguous case aliases cannot pick arbitrary data.
            path=entry.path();found=true;
        }
        if(error || !found)return {{},0xc000000f};
    }
    return {path,0};
}
}
