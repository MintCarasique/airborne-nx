#pragma once
#include "pcm_conversion.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#ifndef __SWITCH__
#include <windows.h>
#include <mmsystem.h>
#endif

namespace airborne {
class PcmOutput {
    using Logger=void(*)(const char*,...);
    Logger log_;std::mutex mutex_;bool ready_=false;
#ifdef __SWITCH__
    AudioOutBuffer buffer_{};
#else
    HMODULE library_=nullptr;HWAVEOUT device_=nullptr;WAVEHDR buffer_{};
    decltype(&waveOutOpen) open_=nullptr;decltype(&waveOutPrepareHeader) prepare_=nullptr;
    decltype(&waveOutWrite) write_=nullptr;decltype(&waveOutUnprepareHeader) unprepare_=nullptr;
    decltype(&waveOutReset) reset_=nullptr;decltype(&waveOutClose) close_=nullptr;
#endif
    alignas(4096) std::array<int16_t,2048> samples_{};
    void result(const char* operation,uint32_t code) {
        log_("AUDIO_NATIVE operation=%s rc=%08x source=%p destination=%p bytes=1024 rate=48000 channels=2 format=PCM16\n",operation,code,samples_.data(),samples_.data());
#ifdef __SWITCH__
        if(code) {
            MemoryInfo info{};u32 page=0;const auto address=reinterpret_cast<uintptr_t>(samples_.data());
            const auto queried=svcQueryMemory(&info,&page,address);
            log_("AUDIO_MEMORY_QUERY address=%016llx bytes=4096 rc=%08x base=%016llx size=%016llx state=%08x attributes=%08x permissions=%08x page=%08x\n",
                (unsigned long long)address,queried,(unsigned long long)info.addr,(unsigned long long)info.size,info.type,info.attr,info.perm,page);
        }
#endif
        if(code)throw std::runtime_error(std::string("Audio operation failed: ")+operation);
    }
public:
    explicit PcmOutput(Logger logger):log_(logger){}
    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
#ifdef __SWITCH__
        result("audoutInitialize",audoutInitialize());ready_=true;
        if(audoutGetSampleRate()!=48000 || audoutGetChannelCount()!=2 || audoutGetPcmFormat()!=PcmFormat_Int16)
            throw std::runtime_error("Unexpected Horizon audio output format");
        buffer_.buffer=samples_.data();buffer_.buffer_size=4096;buffer_.data_size=1024;
        result("audoutStartAudioOut",audoutStartAudioOut());
#else
        library_=LoadLibraryW(L"winmm.dll");if(!library_)throw std::runtime_error("Windows audio library unavailable");
#define AUDIO_PROC(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(library_,name));if(!member)throw std::runtime_error("Missing Windows audio API " name)
        AUDIO_PROC(open_,"waveOutOpen");AUDIO_PROC(prepare_,"waveOutPrepareHeader");AUDIO_PROC(write_,"waveOutWrite");
        AUDIO_PROC(unprepare_,"waveOutUnprepareHeader");AUDIO_PROC(reset_,"waveOutReset");AUDIO_PROC(close_,"waveOutClose");
#undef AUDIO_PROC
        WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=48000;
        format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=192000;
        result("waveOutOpen",open_(&device_,WAVE_MAPPER,&format,0,0,CALLBACK_NULL));ready_=true;
        buffer_.lpData=reinterpret_cast<char*>(samples_.data());buffer_.dwBufferLength=1024;
        result("waveOutPrepareHeader",prepare_(device_,&buffer_,sizeof(buffer_)));
#endif
    }
    void play(const std::array<int16_t,512>& pcm) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!ready_)throw std::runtime_error("PCM output not open");
        std::copy(pcm.begin(),pcm.end(),samples_.begin());
#ifdef __SWITCH__
        armDCacheFlush(samples_.data(),4096);
        result("audoutAppendAudioOutBuffer",audoutAppendAudioOutBuffer(&buffer_));
        AudioOutBuffer* released=nullptr;u32 count=0;
        result("audoutWaitPlayFinish",audoutWaitPlayFinish(&released,&count,100000000));
        if(count!=1 || released!=&buffer_)throw std::runtime_error("Unexpected released PCM buffer");
#else
        result("waveOutWrite",write_(device_,&buffer_,sizeof(buffer_)));
        const auto started=GetTickCount64();
        while(!(buffer_.dwFlags&WHDR_DONE)) {
            if(GetTickCount64()-started>=100)throw std::runtime_error("Windows PCM playback timed out");
            Sleep(1);
        }
#endif
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(ready_) {
#ifdef __SWITCH__
            result("audoutStopAudioOut/cleanup",audoutStopAudioOut());audoutExit();log_("AUDIO_NATIVE operation=audoutExit/cleanup rc=00000000\n");
#else
            result("waveOutReset/cleanup",reset_(device_));
            if(buffer_.dwFlags&WHDR_PREPARED)result("waveOutUnprepareHeader/cleanup",unprepare_(device_,&buffer_,sizeof(buffer_)));
            result("waveOutClose/cleanup",close_(device_));device_=nullptr;
#endif
            ready_=false;
        }
#ifndef __SWITCH__
        if(library_){FreeLibrary(library_);library_=nullptr;}
#endif
    }
};
}
