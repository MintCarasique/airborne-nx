#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#ifndef __SWITCH__
#include <winsock2.h>
#endif
namespace airborne {
class SocketRuntime {
public:
    struct Data {uint16_t version=0x202,highest=0x202,max_sockets=0,max_udp=0;std::array<char,257> description{};std::array<char,129> status{};};
    using Logger=void(*)(const char*,...);
private:
    Logger log_;uint32_t references_=0;
#ifndef __SWITCH__
    HMODULE library_=nullptr;
    decltype(&WSAStartup) startup_=nullptr;decltype(&WSACleanup) cleanup_=nullptr;
    decltype(&WSAGetLastError) last_error_=nullptr;
#endif
public:
    explicit SocketRuntime(Logger log):log_(log){}
    uint32_t start(uint32_t version,Data& out) {
        if(version!=0x202)return 10092; // This adapter exposes only the exercised Winsock 2.2 ABI.
#ifdef __SWITCH__
        if(!references_) {
            const auto rc=socketInitializeDefault();
            log_("SOCKET_NATIVE operation=socketInitializeDefault rc=%08x service=BSD_USER connections=0\n",rc);
            if(R_FAILED(rc))return 10050;
        }
        std::strcpy(out.description.data(),"AirborneNX Horizon BSD socket adapter");std::strcpy(out.status.data(),"Initialized");
#else
        if(!library_) {
            library_=LoadLibraryW(L"ws2_32.dll");
            if(!library_){log_("SOCKET_NATIVE operation=LoadLibrary/ws2_32 rc=%lu\n",GetLastError());return 10091;}
            startup_=reinterpret_cast<decltype(startup_)>(GetProcAddress(library_,"WSAStartup"));
            cleanup_=reinterpret_cast<decltype(cleanup_)>(GetProcAddress(library_,"WSACleanup"));
            last_error_=reinterpret_cast<decltype(last_error_)>(GetProcAddress(library_,"WSAGetLastError"));
            if(!startup_ || !cleanup_ || !last_error_){log_("SOCKET_NATIVE operation=GetProcAddress/ws2_32 rc=%lu\n",GetLastError());FreeLibrary(library_);library_=nullptr;return 10091;}
        }
        WSADATA native{};const int rc=startup_(uint16_t(version),&native);
        log_("SOCKET_NATIVE operation=WSAStartup version=%04x rc=%d connections=0\n",version,rc);
        if(rc)return uint32_t(rc);
        out.version=native.wVersion;out.highest=native.wHighVersion;out.max_sockets=native.iMaxSockets;out.max_udp=native.iMaxUdpDg;
        std::memcpy(out.description.data(),native.szDescription,256);std::memcpy(out.status.data(),native.szSystemStatus,128);
#endif
        ++references_;return 0;
    }
    uint32_t cleanup() {
        if(!references_)return 10093;
#ifdef __SWITCH__
        if(references_==1){socketExit();log_("SOCKET_NATIVE operation=socketExit/cleanup rc=00000000\n");}
#else
        const int rc=cleanup_();const int error=rc?last_error_():0;log_("SOCKET_NATIVE operation=WSACleanup rc=%d error=%d\n",rc,error);if(rc)return uint32_t(error);
#endif
        --references_;return 0;
    }
    bool close() {
        while(references_)if(cleanup())return false;
#ifndef __SWITCH__
        if(library_){FreeLibrary(library_);library_=nullptr;startup_=nullptr;cleanup_=nullptr;last_error_=nullptr;}
#endif
        return true;
    }
};
}
