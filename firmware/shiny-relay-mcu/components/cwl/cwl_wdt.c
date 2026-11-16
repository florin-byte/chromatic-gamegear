// See cwl_wdt.h.
#include "cwl_wdt.h"

static uint32_t Mask(const CwlWdtState_t *S)
{
    uint32_t m = S->BaseMask;
    for (int c = 0; c < 2; c++)
        if (S->Depth[c]) m &= ~(1u << c);
    return m;
}

int32_t CwlWdt_Enter(CwlWdtState_t *S, int Core)
{
    if (Core < 0 || Core > 1 || S->Depth[Core] == 255) return -1;
    const uint32_t was = Mask(S);
    S->Depth[Core]++;
    const uint32_t now = Mask(S);
    return now == was ? -1 : (int32_t)now;
}

int32_t CwlWdt_Leave(CwlWdtState_t *S, int Core)
{
    if (Core < 0 || Core > 1 || S->Depth[Core] == 0) return -1;
    const uint32_t was = Mask(S);
    S->Depth[Core]--;
    const uint32_t now = Mask(S);
    return now == was ? -1 : (int32_t)now;
}

#ifndef CWLWDT_HOST
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_TASK_WDT_INIT
static CwlWdtState_t _S = {
    .BaseMask = 0
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
                | 1u
#endif
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
                | 2u
#endif
};
static SemaphoreHandle_t _Lock;   // one reconfigure at a time, in order

static void Apply(int32_t m)
{
    if (m < 0) return;
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (uint32_t)m,
#ifdef CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };
    (void)esp_task_wdt_reconfigure(&cfg);
}

void CwlWdt_Init(void)
{
    static StaticSemaphore_t buf;
    if (_Lock == NULL) _Lock = xSemaphoreCreateMutexStatic(&buf);
}

int CwlWdt_LongBegin(void)
{
    const int core = xPortGetCoreID();
    if (_Lock == NULL) return core;   // CwlWdt_Init not called: no exemption (the watchdog just reports)
    xSemaphoreTake(_Lock, portMAX_DELAY);
    Apply(CwlWdt_Enter(&_S, core));
    xSemaphoreGive(_Lock);
    return core;
}

void CwlWdt_LongEnd(int Core)
{
    if (_Lock == NULL) return;
    xSemaphoreTake(_Lock, portMAX_DELAY);
    Apply(CwlWdt_Leave(&_S, Core));
    xSemaphoreGive(_Lock);
}
#else
void CwlWdt_Init(void) {}
int CwlWdt_LongBegin(void) { return 0; }
void CwlWdt_LongEnd(int Core) { (void)Core; }
#endif
#endif
