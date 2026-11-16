#include "pwrmgr.h"
#include "pwrmgr_logic.h"

#include "board.h"
#include "cwl.h"
#include "button.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "fpga_common.h"
#include "fpga_rx.h"
#include "fpga_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "osd.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kIdleTime_ms = 100,
    kConsoleAwake_ms = 10000,   // after a console keystroke woke the MCU
    kConsoleWakeEdges = 3,      // UART edges that wake (the first char is lost)
};

typedef enum LowPowerMode {
    kLowPowerMode_Inactive,
    kLowPowerMode_Active,
} LowPowerMode_t;

static const char *TAG = "PwrMgr";
static TaskHandle_t PwrMgrTaskHandle = NULL;
static TimerHandle_t IdleSystemTimer = NULL;
static StaticTimer_t IdleTimerBuffer;
static LowPowerMode_t LPM_Status;
static SemaphoreHandle_t xSemaphore;
static StaticSemaphore_t xMutexBuffer;
// The RAM cart's timer wake while a game plays (cf_rc_dev.c); 0 where it is not built in.
__attribute__((weak)) uint32_t CfRc_SleepTimerMs(void) { return 0; }

static volatile int64_t ConsoleAwakeUntil_us = 0;
static int64_t OperationAwakeUntil_us;
static portMUX_TYPE OperationAwakeLock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t ConsoleAwake_ms = kConsoleAwake_ms;
static volatile uint32_t SleepCount = 0, SleepMs = 0, WakeFpga = 0, WakeConsole = 0, WakeTimer = 0, WakeOther = 0;

// The UART receive line is selected so the MCU wakes up any time the FPGA sends a message
static const gpio_config_t WakeUpPin = {
    .pin_bit_mask = BIT64(PIN_NUM_UART_FROM_FPGA),
    .mode = GPIO_MODE_INPUT,
    .pull_down_en = false,
    .pull_up_en = false,
    .intr_type = GPIO_INTR_DISABLE
};

static void IdleTimerCB( TimerHandle_t xTimer );

void PwrMgr_Task(void *arg)
{
    (void)arg;

    PwrMgrTaskHandle = xTaskGetCurrentTaskHandle();

    IdleSystemTimer = xTimerCreateStatic("tmr-idle-sys", pdMS_TO_TICKS(kIdleTime_ms), pdFAIL, NULL, IdleTimerCB, &IdleTimerBuffer);
    if (xTimerStart(IdleSystemTimer, portMAX_DELAY) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to start idle timer");
    }

    xSemaphore = xSemaphoreCreateMutexStatic( &xMutexBuffer );
    if (xSemaphore == NULL)
    {
        ESP_LOGE(TAG, "Failed to create semaphore");
    }

    while(1)
    {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY))
        {
            // Pause both tasks to ensure that they finish up any work in progress. Since the Sleep task is of higher priority,
            // the delays below are taken advantage of to permit a context switch.
            FPGA_Tx_Pause();
            FPGA_Rx_Pause();

            // Clear out any pending data that the FPGA may have sent
            uart_flush(UART_NUM_1);
            Button_ResetAll();
            OSD_SetVisiblityState(false);

            // Configure the UART pin for wake up
            gpio_config(&WakeUpPin);
            gpio_wakeup_enable(PIN_NUM_UART_FROM_FPGA, GPIO_INTR_LOW_LEVEL);
            vTaskDelay(pdMS_TO_TICKS(10));
            esp_sleep_enable_gpio_wakeup();
            // A keystroke on the console (USB serial) wakes too, so the
            // console stays usable while the system sleeps between games.
            (void) uart_set_wakeup_threshold(CONFIG_ESP_CONSOLE_UART_NUM, kConsoleWakeEdges);
            (void) esp_sleep_enable_uart_wakeup(CONFIG_ESP_CONSOLE_UART_NUM);
            // The relay seat keeper (cwl) pings the relay every few minutes
            // while Wi-Fi is off.
            // A game playing from RAM wakes the unit on its own (savefix1): its save write-back poll
            // (cf_rc_dev.c) is frozen asleep, and nothing else wakes a unit with Wi-Fi dormant.
            const uint32_t Timer_ms = PwrMgr_PickTimerMs(CWL_SleepTimerMs(), CfRc_SleepTimerMs());
            if (Timer_ms > 0) (void) esp_sleep_enable_timer_wakeup((uint64_t)Timer_ms * 1000u);
            else (void) esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
            vTaskDelay(pdMS_TO_TICKS(10));

            // Tell the world we are sleeping and wait for the message to go out...
            ESP_LOGD(TAG, "Sleeping");
            uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, portMAX_DELAY);

            const int64_t Slept_us = esp_timer_get_time();
            esp_light_sleep_start();
            const esp_sleep_wakeup_cause_t Cause = esp_sleep_get_wakeup_cause();
            const uint32_t Slept_ms = (uint32_t)((esp_timer_get_time() - Slept_us) / 1000);
            SleepCount++;
            SleepMs += Slept_ms;
            if (Cause == ESP_SLEEP_WAKEUP_GPIO) WakeFpga++;
            else if (Cause == ESP_SLEEP_WAKEUP_UART) WakeConsole++;
            else if (Cause == ESP_SLEEP_WAKEUP_TIMER) WakeTimer++;
            else WakeOther++;
            CWL_NoteLightSleep(Slept_ms,
                               Cause == ESP_SLEEP_WAKEUP_GPIO ? kCwlWake_Fpga :
                               Cause == ESP_SLEEP_WAKEUP_UART ? kCwlWake_Console : kCwlWake_Other);
            if (Cause == ESP_SLEEP_WAKEUP_UART)
            {
                ConsoleAwakeUntil_us = esp_timer_get_time() + (int64_t)ConsoleAwake_ms * 1000;
                // Console commands (e.g. poke) go out through the TX task,
                // which otherwise waits for an FPGA brightness readback that
                // only comes with the menu or a hotkey.
                FPGA_Tx_Resume();
            }

            ESP_LOGD(TAG, "Awake");
            uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, portMAX_DELAY);

            // Update the brightness in the event hot-keys were used to adjust it.
            // The Rx task will resume the Tx task once it receives the brightness level
            FPGA_Rx_UseBrightnessReadback();
            FPGA_Rx_Resume();

            // Sleep again after the idle time unless something keeps us awake
            // (a wake that brings no FPGA traffic, e.g. the console, would
            // otherwise never re-arm the idle timer).
            PwrMgr_IdleTimerPet();
        }
    }

    ESP_LOGE(TAG, "Sleep loop exited");
}

void PwrMgr_TriggerLightSleep(void)
{
    // Wait for the sleep task to start
    while(PwrMgrTaskHandle == NULL)
    {
        vTaskDelay(1);
    }

    (void) xTaskNotifyGive(PwrMgrTaskHandle);
}

void PwrMgr_IdleTimerPet(void)
{
    // Wait for the task to start
    if (IdleSystemTimer == NULL)
    {
        return;
    }

    (void) xTimerReset(IdleSystemTimer, portMAX_DELAY);
}

// The cart console session (cf_dev.c) keeps the MCU awake: a console keystroke that wakes it
// loses its first characters, so the first `cart usb` command after a sleep got no answer
// (unit 1, usb-cart-5, the page). False outside a session: no cost then.
__attribute__((weak)) bool CfDev_KeepAwake(void) { return false; }

void PwrMgr_KeepAwakeFor(uint32_t Duration_ms)
{
    const int64_t until = esp_timer_get_time() + (int64_t)Duration_ms * 1000;
    portENTER_CRITICAL(&OperationAwakeLock);
    if (OperationAwakeUntil_us < until) OperationAwakeUntil_us = until;
    portEXIT_CRITICAL(&OperationAwakeLock);
    PwrMgr_IdleTimerPet();
}

static void IdleTimerCB( TimerHandle_t xTimer )
{
    portENTER_CRITICAL(&OperationAwakeLock);
    const int64_t operationUntil = OperationAwakeUntil_us;
    portEXIT_CRITICAL(&OperationAwakeLock);
    // The Wi-Fi link keeps the MCU awake only while Wi-Fi is on (the link is
    // in use or in its standby window); check again after the idle time, so
    // the system sleeps as stock once the link lets go.
    if (CWL_KeepAwake() || CfDev_KeepAwake() || (esp_timer_get_time() < ConsoleAwakeUntil_us) ||
        (esp_timer_get_time() < operationUntil))
    {
        (void) xTimerReset(xTimer, 0);
        return;
    }
    PwrMgr_TriggerLightSleep();
}

void PwrMgr_SetLPM(const bool Active)
{
    if ((xSemaphore != NULL) && xSemaphoreTake(xSemaphore, portMAX_DELAY))
    {
        LPM_Status = Active ? kLowPowerMode_Active : kLowPowerMode_Inactive;
        xSemaphoreGive(xSemaphore);
    }
}

bool PwrMgr_IsLPMActive(void)
{
    LowPowerMode_t Status = kLowPowerMode_Inactive;

    if ((xSemaphore != NULL) && xSemaphoreTake(xSemaphore, portMAX_DELAY))
    {
        Status = LPM_Status;
        xSemaphoreGive(xSemaphore);
    }

    return Status;
}

// `pwr`: light-sleep counters; `pwr window <ms>` sets how long a console
// keystroke keeps the MCU awake (RAM only, default 10 s).
static int CmdPwr(int argc, char **argv)
{
    if ((argc >= 3) && (strcmp(argv[1], "window") == 0))
    {
        ConsoleAwake_ms = (uint32_t)atoi(argv[2]);
        ConsoleAwakeUntil_us = esp_timer_get_time() + (int64_t)ConsoleAwake_ms * 1000;
    }
    else if ((argc >= 3) && (strcmp(argv[1], "pokeseq") == 0))
    {
        // Bench test for waking from light sleep: poke each mask 150 ms
        // apart, then end the console window so the MCU may sleep at once
        // (e.g. `pwr pokeseq 10f 8`: reset the game with A held; its first
        // link event then arrives while the MCU sleeps).
        for (int i = 2; i < argc; i++)
        {
            Button_Poke((uint16_t)strtol(argv[i], NULL, 16));
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        ConsoleAwakeUntil_us = 0;
        return 0;
    }
    printf("{ \"sleeps\": %lu, \"slept_ms\": %lu, \"wake_fpga\": %lu, \"wake_console\": %lu, \"wake_timer\": %lu, \"wake_other\": %lu, "
           "\"uptime_ms\": %lu, \"console_window_ms\": %lu, \"lpm\": %d }\n",
           (unsigned long)SleepCount, (unsigned long)SleepMs, (unsigned long)WakeFpga, (unsigned long)WakeConsole,
           (unsigned long)WakeTimer, (unsigned long)WakeOther, (unsigned long)(esp_timer_get_time() / 1000), (unsigned long)ConsoleAwake_ms,
           PwrMgr_IsLPMActive());
    return 0;
}

void PwrMgr_RegisterCommands(void)
{
    const esp_console_cmd_t cmd = {
        .command = "pwr",
        .help = "Light-sleep counters; pwr window <ms>: console keeps the MCU awake this long",
        .func = &CmdPwr,
    };
    (void) esp_console_cmd_register(&cmd);
}
