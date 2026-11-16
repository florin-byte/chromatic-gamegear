#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
typedef int esp_err_t;
typedef struct { const char *command, *help; int (*func)(int, char **); void *argtable; } esp_console_cmd_t;
static inline int esp_console_cmd_register(const esp_console_cmd_t *c) { (void)c; return 0; }
typedef struct { unsigned len, size, count, head; uint8_t *buffer; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
static inline QueueHandle_t xQueueCreateStatic(unsigned len, unsigned size, uint8_t *buf, StaticQueue_t *q)
{ *q = (StaticQueue_t){ .len = len, .size = size, .buffer = buf }; return q; }
static inline int xQueueSend(QueueHandle_t q, const void *v, unsigned ticks)
{ (void)ticks; if (q->count == q->len) return 0; memcpy(q->buffer + ((q->head + q->count) % q->len) * q->size, v, q->size); q->count++; return 1; }
static inline int xQueueReceive(QueueHandle_t q, void *v, unsigned ticks)
{ (void)ticks; if (!q->count) return 0; memcpy(v, q->buffer + q->head * q->size, q->size); q->head = (q->head + 1) % q->len; q->count--; return 1; }
static bool visible;
static inline bool OSD_IsVisible(void) { return visible; }
static inline unsigned esp_log_timestamp(void) { return 0; }
static inline void vTaskDelay(unsigned ticks) { (void)ticks; }
