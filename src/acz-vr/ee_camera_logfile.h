#pragma once
// J-w1-ee-camera: dedicated bounded ee-camera log (D-245/D-247). Header-only,
// Windows. Opened with _SH_DENYNO (FILE_SHARE_READ|FILE_SHARE_WRITE) so an
// external reader can copy it mid-run (run 3: the exclusive lock blocked the
// copy). Cap 32 MiB, no rotation; flushed at most once per second by the
// writer, by `flush_if_due` from the worker loop, and by `final_flush`
// (try-lock only: safe on the factory-release / process-exit path even if a
// thread that held the lock was terminated).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <share.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::ee_camera {
class EeLogFile {
public:
    static constexpr uint64_t default_cap=32ull<<20;
    explicit EeLogFile(uint64_t cap=default_cap) noexcept:cap_(cap) {}
    // `prev` (optional) receives the previous run's file.
    bool open(const wchar_t* path,const wchar_t* prev) noexcept {
        if(!path)return false;
        if(prev)MoveFileExW(path,prev,MOVEFILE_REPLACE_EXISTING);
        AcquireSRWLockExclusive(&lock_);
        f_=_wfsopen(path,L"wb",_SH_DENYNO);
        if(f_)setvbuf(f_,nullptr,_IOFBF,1<<16);
        ReleaseSRWLockExclusive(&lock_);
        return f_!=nullptr;
    }
    // ms: a caller-supplied timestamp (QPC milliseconds in production).
    void line(const char* t,double ms) noexcept {
        if(!f_||!t)return;
        AcquireSRWLockExclusive(&lock_);
        const size_t len=std::strlen(t);
        if(written_+len+32>cap_) {
            if(!capped_){std::fputs("ee-camera;event=log-cap-reached\n",f_);std::fflush(f_);capped_=true;}
        } else {
            const int w=std::fprintf(f_,"%.3f|%s\n",ms,t);
            if(w>0)written_+=uint64_t(w);
            if(ms-lastFlush_>=1000.0){std::fflush(f_);lastFlush_=ms;}
        }
        ReleaseSRWLockExclusive(&lock_);
    }
    // Try-lock variant for the final-report path (never blocks).
    bool try_line(const char* t,double ms) noexcept {
        if(!f_||!t||!TryAcquireSRWLockExclusive(&lock_))return false;
        if(written_+std::strlen(t)+32<=cap_){const int w=std::fprintf(f_,"%.3f|%s\n",ms,t);if(w>0)written_+=uint64_t(w);}
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }
    void flush_if_due(double ms) noexcept {
        if(!f_||ms-lastFlush_<1000.0)return;
        if(!TryAcquireSRWLockExclusive(&lock_))return;
        std::fflush(f_);lastFlush_=ms;
        ReleaseSRWLockExclusive(&lock_);
    }
    bool final_flush() noexcept {
        if(!f_||!TryAcquireSRWLockExclusive(&lock_))return false;
        std::fflush(f_);
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }
    // Fixtures only (production keeps the file for the process lifetime).
    void close() noexcept {AcquireSRWLockExclusive(&lock_);if(f_){std::fclose(f_);f_=nullptr;}ReleaseSRWLockExclusive(&lock_);}
    uint64_t written() const noexcept {return written_;}
    bool capped() const noexcept {return capped_;}
private:
    SRWLOCK lock_=SRWLOCK_INIT;FILE* f_=nullptr;uint64_t cap_,written_=0;double lastFlush_=-1e300;bool capped_=false;
};
} // namespace acz::ee_camera
