#pragma once
#include <stdint.h>
extern uint32_t host_free_heap;   /* the test sets it (the library's sort sizes its work by it) */
static inline uint32_t esp_get_free_heap_size(void) { return host_free_heap; }
