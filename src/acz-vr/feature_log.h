#pragma once
// J-3b9c: a tiny shared facility for DEDICATED per-feature evidence logs.
//   %LOCALAPPDATA%\acz-vr\logs\<name>.log (the previous run's file kept as <name>.prev.log)
//   * opened with FILE_SHARE_READ|FILE_SHARE_WRITE (readable while the game runs);
//   * unrotated, capped at 128 MiB per file (one `feature-log;event=cap-reached` line, then dropped);
//   * write() only copies the line into a bounded in-memory queue (no I/O; overflow is
//     counted and reported as `feature-log;event=dropped;n=`); flush() does the I/O;
//   * every line is prefixed with a millisecond tick (`<ms>|`).
// Callers: worker/owner contexts only (never a Draw path). The shared diagnostics log
// keeps its own copies where the caller mirrors them; the dedicated file is the
// evidence of record. Header-only (inline) on purpose: d3d12_hook.cpp is compiled
// into the DLL and directly into many offline test executables.
//
// API: open(name) -> Handle, write(handle, line), flush(); plus tick(now_ms) for a
// periodic (5 s) flush and fixture-only set_directory / set_cap / stats.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "perf_counters.h"

namespace acz::feature_log {
using Handle=int;
constexpr Handle invalid=-1;
constexpr int max_files=8;
constexpr size_t line_cap=480;          // bytes of one line (longer lines are truncated)
constexpr size_t queue_lines=512;       // bounded queue shared by all files
constexpr uint64_t file_cap=128ull<<20; // 128 MiB per file, unrotated (32 MiB ended a 2 h headset session at 50 min)
constexpr uint64_t flush_every_ms=5000;
struct Stats {uint64_t queued=0,written_lines=0,written_bytes=0,dropped=0;uint64_t write_errors=0,short_writes=0;bool incomplete=false,capped=false,open=false;};
// Sink returns success and the actual byte count. Installed only while no drain is active.
using Sink=bool (*)(void*,HANDLE,const char*,DWORD,DWORD*) noexcept;
namespace detail {
inline bool disk_sink(void*,HANDLE h,const char* p,DWORD n,DWORD* w) noexcept {return WriteFile(h,p,n,w,nullptr)!=FALSE;}
struct File {char name[48]{};HANDLE h=INVALID_HANDLE_VALUE;Stats st{};uint64_t droppedReported=0;bool errorReported=false;};
struct Line {int file=0;uint64_t ms=0;uint16_t len=0;char text[line_cap]{};};
struct State {
    SRWLOCK lock=SRWLOCK_INIT;SRWLOCK io=SRWLOCK_INIT;Sink sink=&disk_sink;void* sink_context=nullptr;File files[max_files]{};int count=0;
    Line q[queue_lines]{};size_t head=0,tail=0;uint64_t lastFlush=0,cap=file_cap;wchar_t dir[MAX_PATH]{};
};
inline State& state() noexcept {static State s;return s;} // static storage (no heap, no stack)
inline bool directory(const wchar_t* configured,wchar_t* out) noexcept {
    if(configured[0]){wcsncpy_s(out,MAX_PATH,configured,_TRUNCATE);return true;}
    wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(!n||n>=MAX_PATH-32)return false;
    wchar_t d[MAX_PATH];_snwprintf_s(d,MAX_PATH,_TRUNCATE,L"%s\\acz-vr",local);CreateDirectoryW(d,nullptr);
    _snwprintf_s(out,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\logs",local);CreateDirectoryW(out,nullptr);
    return true;
}
inline bool bytes(File& f,const char* text,size_t len,Sink sink,void* context) noexcept {
    size_t done=0;
    while(done<len) {
        DWORD w=0;const DWORD remaining=DWORD(len-done);
        const bool ok=sink(context,f.h,text+done,remaining,&w);
        if(w>remaining){w=0;f.st.incomplete=true;++f.st.write_errors;return false;}
        f.st.written_bytes+=w;done+=w;
        if(w<remaining){++f.st.short_writes;f.st.incomplete=true;}
        if(!ok||!w){++f.st.write_errors;f.st.incomplete=true;return false;}
    }
    return true;
}
inline void raw(File& f,const char* text,size_t len,uint64_t cap,Sink sink,void* context) noexcept {
    if(f.h==INVALID_HANDLE_VALUE)return;
    if(f.st.written_bytes>cap||len>cap-f.st.written_bytes) {
        if(!f.st.capped){static const char m[]="feature-log;event=cap-reached\n";
            bytes(f,m,sizeof m-1,sink,context);f.st.capped=true;}
        ++f.st.dropped;f.st.incomplete=true;return;
    }
    if(bytes(f,text,len,sink,context))++f.st.written_lines;
}
} // namespace detail
// Open (or return the already open) dedicated log `name` (ASCII, no path separators).
inline Handle open(const char* name) noexcept {
    if(!name||!*name||std::strlen(name)>=40||std::strpbrk(name,"\\/:"))return invalid;
    auto& s=detail::state();AcquireSRWLockExclusive(&s.io);AcquireSRWLockExclusive(&s.lock);
    Handle h=invalid;wchar_t configured[MAX_PATH];wcsncpy_s(configured,s.dir,_TRUNCATE);
    for(int i=0;i<s.count;++i)if(std::strcmp(s.files[i].name,name)==0){h=i;break;}
    const bool room=s.count<max_files;
    ReleaseSRWLockExclusive(&s.lock);
    if(h==invalid&&room) {
        wchar_t dir[MAX_PATH],path[MAX_PATH],prev[MAX_PATH];
        if(detail::directory(configured,dir)) {
            _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\%S.log",dir,name);_snwprintf_s(prev,MAX_PATH,_TRUNCATE,L"%s\\%S.prev.log",dir,name);
            MoveFileExW(path,prev,MOVEFILE_REPLACE_EXISTING);
            detail::File f;strncpy_s(f.name,name,_TRUNCATE);
            f.h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            f.st.open=f.h!=INVALID_HANDLE_VALUE;
            if(f.st.open){AcquireSRWLockExclusive(&s.lock);h=s.count;s.files[s.count++]=f;ReleaseSRWLockExclusive(&s.lock);}
        }
    }
    ReleaseSRWLockExclusive(&s.io);return h;
}
// Queue one line (no I/O). Never blocks beyond the short queue lock.
inline void write(Handle h,const char* line) noexcept {
    if(h<0||h>=max_files||!line)return;
    perf::Scope cost(perf::log); // FPS investigation (observe only)
    auto& s=detail::state();AcquireSRWLockExclusive(&s.lock);
    if(h<s.count) {
        auto& f=s.files[h];
        if(s.head-s.tail>=queue_lines){++f.st.dropped;f.st.incomplete=true;}
        else {
            auto& q=s.q[s.head%queue_lines];q.file=h;q.ms=GetTickCount64();
            size_t n=std::strlen(line);if(n>line_cap-1){n=line_cap-1;f.st.incomplete=true;}
            std::memcpy(q.text,line,n);q.text[n]=0;q.len=uint16_t(n);++s.head;++f.st.queued;
        }
    }
    ReleaseSRWLockExclusive(&s.lock);
}
// Detach one bounded batch. The I/O lock serializes drains; no queue/producer
// lock is held during sink calls. New arrivals wait for the next drain.
inline void flush() noexcept {
    auto& s=detail::state();AcquireSRWLockExclusive(&s.io);
    // Heap-free, process-lifetime scratch; serialized by io, not on a small worker stack.
    static detail::Line batch[queue_lines];
    detail::File files[max_files];uint64_t dropped[max_files]{};size_t count=0;int fileCount=0;uint64_t cap=0;Sink sink;void* context;
    AcquireSRWLockExclusive(&s.lock);
    while(s.tail!=s.head&&count<queue_lines){batch[count++]=s.q[s.tail%queue_lines];++s.tail;}
    fileCount=s.count;cap=s.cap;sink=s.sink;context=s.sink_context;
    for(int i=0;i<fileCount;++i){files[i]=s.files[i];dropped[i]=files[i].st.dropped;files[i].st.written_lines=0;files[i].st.written_bytes=0;
        files[i].st.write_errors=0;files[i].st.short_writes=0;files[i].st.dropped=0;}
    // Keep the existing byte totals separately for cap accounting.
    uint64_t previous[max_files]{};
    for(int i=0;i<fileCount;++i){previous[i]=s.files[i].st.written_bytes;files[i].st.written_bytes=previous[i];}
    ReleaseSRWLockExclusive(&s.lock);
    char buf[line_cap+32];
    for(size_t i=0;i<count;++i){const auto& q=batch[i];
        const int n=std::snprintf(buf,sizeof buf,"%llu|%s\n",static_cast<unsigned long long>(q.ms),q.text);
        if(n>0)detail::raw(files[q.file],buf,size_t(n)<sizeof buf?size_t(n):sizeof buf-1,cap,sink,context);
    }
    for(int i=0;i<fileCount;++i){auto& f=files[i];
        if(dropped[i]!=f.droppedReported&&!f.st.capped){
            const int n=std::snprintf(buf,sizeof buf,"%llu|feature-log;event=dropped;n=%llu\n",
                static_cast<unsigned long long>(GetTickCount64()),static_cast<unsigned long long>(dropped[i]));
            if(n>0)detail::raw(f,buf,size_t(n),cap,sink,context);
            f.droppedReported=dropped[i];
        }
        // An error remains queryable even if the sink cannot accept this marker.
        if(f.st.incomplete&&!f.errorReported){
            const int n=std::snprintf(buf,sizeof buf,"%llu|feature-log;event=incomplete-evidence\n",
                static_cast<unsigned long long>(GetTickCount64()));
            f.errorReported=n>0&&detail::bytes(f,buf,size_t(n),sink,context);
        }
    }
    AcquireSRWLockExclusive(&s.lock);
    for(int i=0;i<fileCount;++i){auto& dst=s.files[i];const auto& f=files[i];
        dst.st.written_bytes+=f.st.written_bytes-previous[i];dst.st.written_lines+=f.st.written_lines;
        dst.st.write_errors+=f.st.write_errors;dst.st.short_writes+=f.st.short_writes;dst.st.dropped+=f.st.dropped;
        dst.st.capped|=f.st.capped;dst.st.incomplete|=f.st.incomplete;dst.errorReported=f.errorReported;dst.droppedReported=f.droppedReported;
    }
    s.lastFlush=GetTickCount64();ReleaseSRWLockExclusive(&s.lock);ReleaseSRWLockExclusive(&s.io);
}
inline void set_sink(Sink sink,void* context=nullptr) noexcept {
    auto& s=detail::state();AcquireSRWLockExclusive(&s.io);AcquireSRWLockExclusive(&s.lock);
    s.sink=sink?sink:&detail::disk_sink;s.sink_context=context;
    ReleaseSRWLockExclusive(&s.lock);ReleaseSRWLockExclusive(&s.io);
}
// Periodic flush (every 5 s) from a worker tick.
inline void tick(uint64_t now_ms) noexcept {
    auto& s=detail::state();
    AcquireSRWLockShared(&s.lock);const bool due=now_ms-s.lastFlush>=flush_every_ms||s.head-s.tail>=queue_lines/2;ReleaseSRWLockShared(&s.lock);
    if(due)flush();
}
inline Stats stats(Handle h) noexcept {
    auto& s=detail::state();Stats out;AcquireSRWLockShared(&s.lock);if(h>=0&&h<s.count)out=s.files[h].st;ReleaseSRWLockShared(&s.lock);return out;
}
// Fixture/diagnostic only: the directory (before open) and the per-file cap.
inline void set_directory(const wchar_t* dir) noexcept {auto& s=detail::state();AcquireSRWLockExclusive(&s.lock);
    wcsncpy_s(s.dir,MAX_PATH,dir?dir:L"",_TRUNCATE);ReleaseSRWLockExclusive(&s.lock);}
inline void set_cap(uint64_t bytes) noexcept {auto& s=detail::state();AcquireSRWLockExclusive(&s.lock);s.cap=bytes?bytes:file_cap;ReleaseSRWLockExclusive(&s.lock);}
} // namespace acz::feature_log
