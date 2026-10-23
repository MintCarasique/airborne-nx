#pragma once
#include <array>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cerrno>

namespace airborne {
// Normal tracing is buffered; first errors and explicit shutdown boundaries
// flush immediately. The FILE remains owned by the caller and outlives this
// writer's attachment. A sink failure is latched, including its first errno.
class BufferedReport {
public:
    struct Stats {uint64_t writes=0,bytes=0,flushes=0;int first_error=0;};
    using Flush=int(*)(FILE*);
    bool attach(FILE* stream,Flush flush=std::fflush) {
        std::lock_guard<std::mutex> guard(mutex_);stream_=stream;flush_=flush;stats_={};last_={};
        if(!stream_ || std::setvbuf(stream_,buffer_.data(),_IOFBF,buffer_.size())){fail();return false;}return true;
    }
    bool write(bool immediate,const char* format,va_list args) {
        std::lock_guard<std::mutex> guard(mutex_);
        if(!stream_){fail();return false;}
        const int bytes=std::vfprintf(stream_,format,args);++stats_.writes;
        if(bytes<0)fail();else stats_.bytes+=uint64_t(bytes);
        flush_locked(immediate);return !stats_.first_error;
    }
    bool flush(bool immediate=true) {
        std::lock_guard<std::mutex> guard(mutex_);flush_locked(immediate);return !stats_.first_error;
    }
    Stats stats(){std::lock_guard<std::mutex> guard(mutex_);return stats_;}
    void detach(){std::lock_guard<std::mutex> guard(mutex_);stream_=nullptr;}
private:
    void fail(){if(!stats_.first_error)stats_.first_error=errno?errno:EIO;}
    void flush_locked(bool immediate) {
        if(!stream_){fail();return;}
        const auto now=std::chrono::steady_clock::now();
        if(immediate || now-last_>=std::chrono::milliseconds(250)) {
            ++stats_.flushes;if(flush_(stream_))fail();last_=now;
        }
    }
    std::mutex mutex_;std::array<char,65536> buffer_{};FILE* stream_=nullptr;
    Flush flush_=std::fflush;Stats stats_{};std::chrono::steady_clock::time_point last_{};
};
}
