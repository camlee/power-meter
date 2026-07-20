#include "esp/lvgl_memory.h"

#include <esp_heap_caps.h>
#include "memory/heap_policy.h"

void* lvglMemoryAlloc(size_t size) {
    // LVGL's object metadata and label strings are CPU-only. Prefer PSRAM for
    // all of them, including the many sub-64-byte allocations; draw buffers
    // that need DMA-capable memory are allocated separately by the display
    // port. Reserving internal RAM here keeps the network stack healthy.
    return heap_policy::mallocPreferred(size);
}

void lvglMemoryFree(void* pointer) { heap_caps_free(pointer); }

void* lvglMemoryRealloc(void* pointer, size_t size) {
    return heap_policy::reallocPreferred(pointer, size);
}
