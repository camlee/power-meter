#include "esp/lvgl_memory.h"

#include <esp_heap_caps.h>

void* lvglMemoryAlloc(size_t size) {
    // LVGL's object metadata and label strings are CPU-only. Prefer PSRAM for
    // all of them, including the many sub-64-byte allocations; draw buffers
    // that need DMA-capable memory are allocated separately by the display
    // port. Reserving internal RAM here keeps the network stack healthy.
    if (void* pointer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return pointer;
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lvglMemoryFree(void* pointer) { heap_caps_free(pointer); }

void* lvglMemoryRealloc(void* pointer, size_t size) {
    if (size == 0) {
        heap_caps_free(pointer);
        return nullptr;
    }
    if (void* resized = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        return resized;
    }
    return heap_caps_realloc(pointer, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
