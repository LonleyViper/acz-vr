// stage_detect_win.cpp -- see stage_detect_win.h. Windows glue only; the matching logic is stage_fingerprint.cpp and the bounded scan
// loop is stage_scan.cpp (both offline-tested).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "stage_detect_win.h"
#include "ee_camera_guarded.h"
#include <windows.h>
#include <psapi.h>
#include <vector>

namespace acz::stagefp {
namespace {
bool readable(DWORD pr) noexcept {
    if (pr & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD b = pr & 0xFF;
    return b == PAGE_READONLY || b == PAGE_READWRITE || b == PAGE_WRITECOPY || b == PAGE_EXECUTE_READ || b == PAGE_EXECUTE_READWRITE ||
           b == PAGE_EXECUTE_WRITECOPY;
}
bool read_write(DWORD pr) noexcept { const DWORD b = pr & 0xFF; return b == PAGE_READWRITE || b == PAGE_EXECUTE_READWRITE; }
uint64_t own_module_base() noexcept {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&own_module_base), &h);
    return uint64_t(uintptr_t(h));
}

class WinSource final : public PageSource {
public:
    explicit WinSource(PSAPI_WORKING_SET_EX_INFORMATION* ws) : ws_(ws) {}
    bool resident(uint64_t addr, uint32_t pages, uint8_t* flags) noexcept override {
        if (!ws_ || pages > kMaxChunk / kPage) return false;
        for (uint32_t i = 0; i < pages; ++i) {
            ws_[i].VirtualAddress = reinterpret_cast<PVOID>(uintptr_t(addr + uint64_t(i) * kPage));
            ws_[i].VirtualAttributes.Flags = 0;
        }
        // Reads only the working-set database: never touches (or faults in) the pages themselves.
        if (!QueryWorkingSetEx(GetCurrentProcess(), ws_, DWORD(pages * sizeof(PSAPI_WORKING_SET_EX_INFORMATION)))) return false;
        for (uint32_t i = 0; i < pages; ++i) flags[i] = ws_[i].VirtualAttributes.Valid ? 1 : 0;
        return true;
    }
    bool read(uint64_t addr, void* out, size_t n) noexcept override {
        return acz::ee_camera::guarded_copy(out, reinterpret_cast<const void*>(uintptr_t(addr)), n);
    }
    uint64_t now_ms() noexcept override { return GetTickCount64(); }
    void yield(uint32_t ms) noexcept override { Sleep(ms); }
private:
    PSAPI_WORKING_SET_EX_INFORMATION* ws_;
};
} // namespace

void scan_process(Matcher& m, const ScanLimits& lim, ScanStats& st) noexcept {
    st = ScanStats{};
    const ULONGLONG t0 = GetTickCount64();
    constexpr size_t kWsBytes = (kMaxChunk / kPage) * sizeof(PSAPI_WORKING_SET_EX_INFORMATION);
    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMaxChunk + kWsBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) { st.read_failures = 1; st.stop = "no-buffer"; st.truncated = true; return; }
    auto* ws = reinterpret_cast<PSAPI_WORKING_SET_EX_INFORMATION*>(buf + kMaxChunk);
    const uint64_t bufAlloc = uint64_t(uintptr_t(buf)), ownMod = own_module_base();
    try {
        std::vector<Region> regions;
        regions.reserve(4096);
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        uint64_t p = uint64_t(uintptr_t(si.lpMinimumApplicationAddress));
        const uint64_t end = uint64_t(uintptr_t(si.lpMaximumApplicationAddress));
        // Allocation span: regions of one allocation are contiguous in the walk; the span is filled in when the allocation ends.
        uint64_t curAlloc = 0, curAllocEnd = 0; size_t curFirst = 0;
        auto flush = [&] { for (size_t i = curFirst; i < regions.size(); ++i) regions[i].alloc_span = curAllocEnd - curAlloc; };
        for (unsigned guard = 0; p < end && guard < 4000000; ++guard) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(uintptr_t(p)), &mbi, sizeof mbi) != sizeof mbi) break;
            const uint64_t base = uint64_t(uintptr_t(mbi.BaseAddress)), next = base + mbi.RegionSize;
            if (next <= p) break;
            p = next;
            if (mbi.State == MEM_FREE) continue;
            const uint64_t alloc = uint64_t(uintptr_t(mbi.AllocationBase));
            if (alloc != curAlloc) { flush(); curAlloc = alloc; curFirst = regions.size(); }
            curAllocEnd = next;
            if (mbi.State != MEM_COMMIT || !readable(mbi.Protect)) continue;
            if (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) { ++st.regions_skipped_gpu; continue; }   // GPU upload/readback mappings
            if (mbi.Type == MEM_IMAGE) { ++st.regions_skipped_image; continue; }                          // executables/DLLs: no stage data
            if (alloc == bufAlloc || (ownMod && alloc == ownMod)) { ++st.regions_skipped_own; continue; }
            Region r; r.base = base; r.size = mbi.RegionSize; r.alloc_base = alloc;
            r.cls = uint8_t((mbi.Type == MEM_PRIVATE ? 1 : 0) | (read_write(mbi.Protect) ? 2 : 0));   // bits; classified once the span is known
            regions.push_back(r);
        }
        flush();
        for (auto& r : regions) r.cls = classify((r.cls & 1) != 0, (r.cls & 2) != 0, r.alloc_span);
        order_regions(regions);
        WinSource src(ws);
        scan_regions(m, regions, src, buf, lim, st);   // keeps the enumerator's skip counters
    } catch (...) {   // bad_alloc of the region list: no scan this attempt
        ++st.read_failures; st.stop = "no-memory"; st.truncated = true;
    }
    SecureZeroMemory(buf, kMaxChunk);
    VirtualFree(buf, 0, MEM_RELEASE);
    st.ms = uint32_t(GetTickCount64() - t0);
}

} // namespace acz::stagefp
