#pragma once
#include <stdint.h>
#define ESP_OK 0
typedef struct host_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
int esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
int esp_timer_start_once(esp_timer_handle_t, uint64_t);
int esp_timer_stop(esp_timer_handle_t);
