#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// LVGL custom allocator backed by external PSRAM (LV_MEM_CUSTOM=1). Keeps the LVGL
// object pool out of scarce internal SRAM; DMA draw buffers stay internal (lvgl_port).
void* lvm_alloc(size_t size);
void  lvm_free(void* p);
void* lvm_realloc(void* p, size_t size);
#ifdef __cplusplus
}
#endif
