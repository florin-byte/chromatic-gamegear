#pragma once
#define ESP_LOGE(t, ...) do { fprintf(stderr, "E %s: ", t); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define ESP_LOGI(t, ...) do { fprintf(stderr, "I %s: ", t); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define ESP_LOGD(t, ...) do { } while (0)
#define ESP_LOGW ESP_LOGE
