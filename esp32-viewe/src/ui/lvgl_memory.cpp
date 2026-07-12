#include "esp/lvgl_memory.h"

#include <esp_heap_caps.h>

namespace {
constexpr size_t kPsramThreshold = 64;
}

void* lvglMemoryAlloc(size_t size) {
    if (size >= kPsramThreshold) {
        if (void* pointer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return pointer;
    }
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lvglMemoryFree(void* pointer) { heap_caps_free(pointer); }

void* lvglMemoryRealloc(void* pointer, size_t size) {
    const uint32_t caps = size >= kPsramThreshold
        ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    return heap_caps_realloc(pointer, size, caps);
}
