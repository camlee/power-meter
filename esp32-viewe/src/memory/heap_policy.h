#pragma once

#include <cstddef>
#include <esp_heap_caps.h>

#ifndef POWER_METER_HAS_PSRAM
#define POWER_METER_HAS_PSRAM 0
#endif

namespace heap_policy {

constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

// Long-lived and potentially large application buffers prefer PSRAM on
// targets that provide it. Internal 8-bit RAM remains a deliberate fallback
// so the same service code can run on targets without external RAM.
inline void* mallocPreferred(size_t size) {
#if POWER_METER_HAS_PSRAM
    if (void* pointer = heap_caps_malloc(size, kPsramCaps)) return pointer;
#endif
    return heap_caps_malloc(size, kInternalCaps);
}

// For optional buffers whose size would make an internal-RAM fallback unsafe.
// Callers must degrade the feature when this returns null.
inline void* mallocPsramOnly(size_t size) {
#if POWER_METER_HAS_PSRAM
    return heap_caps_malloc(size, kPsramCaps);
#else
    (void)size;
    return nullptr;
#endif
}

inline void* callocPreferred(size_t count, size_t size) {
#if POWER_METER_HAS_PSRAM
    if (void* pointer = heap_caps_calloc(count, size, kPsramCaps)) return pointer;
#endif
    return heap_caps_calloc(count, size, kInternalCaps);
}

inline void* reallocPreferred(void* pointer, size_t size) {
    if (size == 0) {
        heap_caps_free(pointer);
        return nullptr;
    }
#if POWER_METER_HAS_PSRAM
    if (void* resized = heap_caps_realloc(pointer, size, kPsramCaps)) return resized;
#endif
    return heap_caps_realloc(pointer, size, kInternalCaps);
}

} // namespace heap_policy
