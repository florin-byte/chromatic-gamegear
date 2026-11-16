#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return malloc(n); }
static inline void *heap_caps_calloc(size_t a, size_t n, int c) { (void)c; return calloc(a, n); }
static inline void heap_caps_free(void *p) { free(p); }
