#include "button.h"

#include "mutex.h"
#include "esp_log.h"
#include "osd.h"
#include "esp_console.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdint.h>
#include <stdbool.h>

enum {
    kMinHold_ticks = 10,

    kButtonBitsMask    = (1 << kNumButtons) - 1,

    kPokedInputsQLen = 10,
};

typedef struct ButtonCtx {
    ButtonState_t State;
} ButtonCtx_t;

__attribute__((unused)) static const char* TAG = "btn";

static ButtonCtx_t _Ctx[kNumButtons];
static const char* _ButtonNames[kNumButtons] = {
    [kButton_Start]  = "Button_Start",
    [kButton_Select] = "Button_Select",
    [kButton_B]      = "Button_B",
    [kButton_A]      = "Button_A",
    [kButton_Down]   = "Button_Down",
    [kButton_Right]  = "Button_Right",
    [kButton_Left]   = "Button_Left",
    [kButton_Up]     = "Button_Up",
    [kButton_MenuEn] = "Button_MenuEn",
    [kButton_MenuEnAlt] = "Button_MenuEnAlt",
};
static const char* _StateNames[kNumButtonStates] = {
    [kButtonState_None]     = "BState_None",
    [kButtonState_Pressed]  = "BState_Pressed",
};

static fnOnButtonPokeCb_t _PokeHandler;
static StaticQueue_t _PokedInputsQueue;
static QueueHandle_t _PokedInputsHandle;
static ButtonBits_t _PokedInputBuffer[kPokedInputsQLen];
static ButtonBits_t _PrevButtons = kButtonBits_None;

static ButtonBits_t _LastForwardedPoke;
static bool Button_PokeInputs(const ButtonBits_t Inputs);
static int poke_button_command(int argc, char **argv);

static uint32_t _PhysicalEvents;
void Button_NotePhysicalEvent(void)
{
    (void)__atomic_add_fetch(&_PhysicalEvents, 1, __ATOMIC_RELAXED);
}
uint32_t Button_PhysicalEventCount(void)
{
    return __atomic_load_n(&_PhysicalEvents, __ATOMIC_RELAXED);
}

void Button_Update(uint16_t NewButtons)
{
    NewButtons = NewButtons & kButtonBitsMask;

    if (Mutex_Take(kMutexKey_Buttons) == kMutexResult_Ok)
    {
        for (Button_t b = kButton_Start; b < kNumButtons; b++)
        {
            const uint16_t mask = (1 << b);

            // Determine the button state based on previous and current state
            const uint16_t PrevState = _PrevButtons & mask;
            const uint16_t CurrState = NewButtons & mask;

            if (!PrevState && CurrState)
            {
                // Button was not pressed, now pressed
                _Ctx[b].State = kButtonState_Pressed;
            }
        }

        _PrevButtons = NewButtons;

        (void) Mutex_Give(kMutexKey_Buttons);
    }
}

void Button_ResetAll(void)
{
    if (Mutex_Take(kMutexKey_Buttons) == kMutexResult_Ok)
    {
        _PrevButtons = 0;
        for (Button_t b = kButton_Start; b < kNumButtons; b++)
        {
            _Ctx[b].State = kButtonState_None;
        }
        (void) Mutex_Give(kMutexKey_Buttons);
    }
}

ButtonState_t Button_GetState(const Button_t b)
{
    // fr1: SELECT reaches the OSD (ModRetro 0.13.0 dropped it here, with no reason given; the menus that ask for it,
    // SYSTEM > NETWORK's consent and the GAMES library, never got it). START and the D-pad were never dropped.
    if ((unsigned)b >= kNumButtons)
    {
        return kButtonState_None;
    }

    ButtonState_t s = kButtonState_None;

    if (Mutex_Take(kMutexKey_Buttons) == kMutexResult_Ok)
    {
        s = _Ctx[b].State;

        _Ctx[b].State = kButtonState_None;
        (void) Mutex_Give(kMutexKey_Buttons);
    }

    return s;
}

const char* Button_GetNameStr(const Button_t b)
{
    if (((unsigned)b >= kNumButtons))
    {
        return "unknown-btn";
    }

    return _ButtonNames[b];
}

const char* Button_GetStateStr(const ButtonState_t s)
{
    if ((unsigned)s >= kNumButtonStates)
    {
        return "unknown-btn-state";
    }

    return _StateNames[s];
}

static bool Button_PokeInputs(const ButtonBits_t Inputs)
{
    if (_PokedInputsHandle == NULL)
    {
        _PokedInputsHandle = xQueueCreateStatic(kPokedInputsQLen, sizeof(_PokedInputBuffer[0]), (uint8_t*)_PokedInputBuffer, &_PokedInputsQueue);
        configASSERT(_PokedInputsHandle);
    }

    const ButtonBits_t menu = kButtonBits_MenuEnAlt | kButtonBits_MenuEn;
    const bool release = (_LastForwardedPoke & menu & ~Inputs) != 0;
    const bool visible = OSD_IsVisible();
    const bool forward = (Inputs & menu) != 0 || release || !visible;
    bool queued = false;
    if (forward)
    {
        queued = xQueueSend(_PokedInputsHandle, &Inputs, pdMS_TO_TICKS(1)) == pdPASS;
        if (queued)
        {
            // Keep the press until its release is accepted, even if the OSD opens.
            _LastForwardedPoke = Inputs;
            if (_PokeHandler != NULL) _PokeHandler();
        }
    }
    else
    {
        Button_Update(Inputs);
    }
    if ((Inputs & menu) != 0 || release)
        printf("[%u ms] poke: mask 0x%03x -> %s (visible %d, queued %d)\n",
               (unsigned)esp_log_timestamp(), (unsigned)Inputs,
               forward ? "fpga" : "local", visible, queued);
    return !forward || queued;
}

bool Button_Poke(uint16_t Inputs)
{
    return Button_PokeInputs((ButtonBits_t)Inputs);
}

bool Button_TakePokedInputs(uint16_t *pInputs)
{
    ButtonBits_t Input = 0;
    if ((_PokedInputsHandle == NULL) || (xQueueReceive(_PokedInputsHandle, &Input, 0) != pdTRUE))
    {
        return false;
    }
    *pInputs = (uint16_t) Input;
    return true;
}

uint16_t Button_GetPokedInputs(void)
{
    ButtonBits_t Input = 0;
    if (_PokedInputsHandle != NULL)
    {
        xQueueReceive(_PokedInputsHandle, &Input, 0);
    }
    return (uint16_t) Input;
}

void Button_RegisterOnButtonPokeCb(fnOnButtonPokeCb_t Handler)
{
    if (Handler == NULL)
    {
        ESP_LOGE(TAG, "Poke callback handler was null");
        return;
    }

    _PokeHandler = Handler;
}

static int poke_button_command(int argc, char **argv)
{
    // poke <mask> [tap_ms]: with tap_ms, press for that long then release
    // (one clean tap; menus auto-repeat a press held ~150 ms).
    if (argc != 2 && argc != 3)
    {
        ESP_LOGE(TAG, "Poke expects a single argument");
        return -1;
    }

    char *arg0 = argv[1];
    if (strncmp(arg0, "0x", 2) == 0 ||
        strncmp(arg0, "0X", 2) == 0)
    {
        ESP_LOGE(TAG, "Argument must be hexadecimal without prefix.");
        return ESP_ERR_INVALID_ARG;
    }
    int32_t hex = strtol(arg0, NULL, 16);
    if (hex >= (1 << kNumButtons))
    {
        ESP_LOGE(TAG, "Illegal button bitmap.");
        return ESP_ERR_INVALID_ARG;
    }
    if (!Button_PokeInputs((ButtonBits_t)hex)) return ESP_FAIL;
    if (argc == 3)
    {
        vTaskDelay(pdMS_TO_TICKS(atoi(argv[2])));
        if (!Button_PokeInputs(0)) return ESP_FAIL;
    }

    return ESP_OK;
}

void Button_RegisterCommands(void)
{
    esp_console_cmd_t command = {
        .command = "poke",
        .help = "Fakes a button input by the Chromatic",
        .func = &poke_button_command,
        .argtable = NULL,
    };

    esp_err_t err;
    if ((err = esp_console_cmd_register(&command)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Registering '%s' command failed: %s", command.command, esp_err_to_name(err));
    }
}
