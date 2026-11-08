#pragma once
// J-w1-ee-camera: SEH-guarded memory primitives (Windows, MSVC). No
// IsBadReadPtr: ranges are checked with VirtualQuery, and the copy/store itself
// runs under __try/__except so a page that changes underneath cannot crash the
// game. These frames hold no C++ objects with destructors (C2712).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace acz::ee_camera {
inline bool guarded_copy(void* dst,const void* src,size_t n) noexcept {
    __try {std::memcpy(dst,src,n);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
inline bool guarded_store(volatile float* p,float v) noexcept {
    __try {*p=v;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
inline bool rw_protect(DWORD p) noexcept {
    if(p&(PAGE_GUARD|PAGE_NOACCESS))return false;
    const DWORD b=p&0xFF;return b==PAGE_READWRITE||b==PAGE_EXECUTE_READWRITE;
}
// Every page of [a, a+n) committed and read-write.
inline bool range_ok(uint64_t a,uint64_t n) noexcept {
    uint64_t p=a;const uint64_t end=a+n;
    if(end<a||!n)return false;
    while(p<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p)),&mbi,sizeof mbi)!=sizeof mbi)return false;
        if(mbi.State!=MEM_COMMIT||!rw_protect(mbi.Protect))return false;
        const uint64_t next=uint64_t(uintptr_t(mbi.BaseAddress))+mbi.RegionSize;
        if(next<=p)return false;
        p=next;
    }
    return true;
}
} // namespace acz::ee_camera
