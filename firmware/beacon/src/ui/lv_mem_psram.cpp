#include "ui/lv_mem_psram.h"
#include <esp_heap_caps.h>

void* lvm_alloc(size_t size)            { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
void  lvm_free(void* p)                 { heap_caps_free(p); }
void* lvm_realloc(void* p, size_t size) { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM); }
