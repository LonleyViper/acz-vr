#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
namespace acz::diagnostics {
// Mutex-owned fixed storage. No eviction, allocation or native dispatch here.
// A held reference prevents observing the game's final Release: upload buffers
// live until process exit or explicit orderly teardown, NOT game retirement.
// Unknown AddRef completion consumes capacity forever and is never retried.
template<class Resource> struct UploadCreationPins {
    static constexpr size_t capacity=32;
    static constexpr uint64_t per_buffer_cap=16ULL*1024*1024;
    // 32 permanent reservations, 16 MiB per object, 512 MiB aggregate logical bytes.
    static constexpr uint64_t retained_byte_cap=512ULL*1024*1024;
    static_assert(retained_byte_cap==512ULL*1024*1024,"diagnostic upload retention budget");
    struct Entry {
        uint64_t address=0,creation=0,lifetime=0,map_generation=0,bytes=0;
        const void* mapped=nullptr;
        Resource* held=nullptr;
        bool reserved=false,finished=false;
    };
    std::array<Entry,capacity> entries{};
    uint64_t retained_bytes=0; // charged at reservation, including unknown AddRef completion
    const char* last_reason="upload-pin-not-created";
    // L3 creation ordinals saturate at UINT32_MAX: never admit that epoch.
    size_t reserve(uint64_t address,uint64_t creation,uint64_t bytes=per_buffer_cap) noexcept {
        for(const auto& e:entries)if(e.reserved&&e.address==address) {
            last_reason="upload-pin-already-reserved";return capacity;
        }
        if(!address||!creation||creation>=UINT32_MAX) {
            last_reason="upload-pin-generation-unavailable";return capacity;
        }
        for(size_t i=0;i<capacity;++i)if(!entries[i].reserved) {
            if(!bytes||bytes>per_buffer_cap) {last_reason="upload-pin-oversize";return capacity;}
            if(retained_bytes>retained_byte_cap||bytes>retained_byte_cap-retained_bytes) {
                last_reason="upload-pin-byte-budget";return capacity;
            }
            auto& e=entries[i];e.address=address;e.creation=creation;e.reserved=true;
            e.bytes=bytes;retained_bytes+=bytes;
            last_reason="upload-pin-pending";return i;
        }
        last_reason="upload-pin-table-full";return capacity;
    }
    void finish(size_t slot,Resource* owned) noexcept {
        if(slot>=capacity||entries[slot].finished)return;
        auto& e=entries[slot];e.finished=true;e.held=owned;
        last_reason=owned?"upload-pin-owned":"upload-pin-addref-fault";
    }
    void bind(uint64_t address,uint64_t creation,uint64_t lifetime) noexcept {
        for(auto& e:entries)if(e.reserved&&e.address==address&&e.creation==creation&&!e.lifetime)
            e.lifetime=lifetime; // one publication only, never adopt a replacement
    }
    void map(uint64_t address,uint64_t creation,uint64_t lifetime,uint64_t generation,const void* pointer) noexcept {
        for(auto& e:entries)if(e.held&&e.address==address&&e.creation==creation&&e.lifetime==lifetime) {
            e.map_generation=generation;e.mapped=pointer;
        }
    }
    const char* lookup(uint64_t address,uint64_t creation,uint64_t lifetime,uint64_t generation,const void* pointer) const noexcept {
        for(const auto& e:entries)if(e.reserved&&e.address==address) {
            if(e.creation!=creation||!lifetime||e.lifetime!=lifetime)return "upload-pin-generation-mismatch";
            if(!e.finished)return "upload-pin-pending";
            if(!e.held)return "upload-pin-addref-fault";
            if(!pointer||!generation||e.map_generation!=generation||e.mapped!=pointer)return "upload-pin-map-mismatch";
            return nullptr;
        }
        return "upload-pin-not-created";
    }
    Resource* take(size_t slot) noexcept {
        auto* owned=entries[slot].held;entries[slot].held=nullptr;return owned;
    }
};
}
