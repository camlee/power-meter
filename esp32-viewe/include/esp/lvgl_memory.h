#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* lvglMemoryAlloc(size_t size);
void lvglMemoryFree(void* pointer);
void* lvglMemoryRealloc(void* pointer, size_t size);

#ifdef __cplusplus
}
#endif
