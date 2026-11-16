#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bit definitions follow the scheme sent to us by the FPGA
typedef enum Button {
    kButton_Start,
    kButton_Select,
    kButton_B,
    kButton_A,
    kButton_Up,
    kButton_Right,
    kButton_Left,
    kButton_Down,
    kButton_MenuEnAlt,
    kButton_MenuEn,
    kNumButtons,
    kButton_None,
} Button_t;

typedef enum ButtonState {
    kButtonState_None,
    kButtonState_Pressed,
    kNumButtonStates,
} ButtonState_t;

typedef enum ButtonBits {
    kButtonBits_None   = 0,
    kButtonBits_B      = (1 << kButton_B),
    kButtonBits_A      = (1 << kButton_A),
    kButtonBits_Down   = (1 << kButton_Down),
    kButtonBits_Right  = (1 << kButton_Right),
    kButtonBits_Left   = (1 << kButton_Left),
    kButtonBits_Up     = (1 << kButton_Up),
    kButtonBits_MenuEn = (1 << kButton_MenuEn),
    kButtonBits_MenuEnAlt = (1 << kButton_MenuEnAlt),
} ButtonBits_t;

#include "button_poke.h"
// The poke masks are the FPGA's 9 bits; the enum above is the status word's 10.
_Static_assert((int)kButtonPoke_Menu == (int)kButtonBits_MenuEnAlt, "the MENU poke is bit 8");
_Static_assert(((int)kButtonBits_MenuEn & (int)kButtonPoke_FpgaMask) == 0, "bit 9 is status only");

typedef void (*fnOnButtonPokeCb_t)(void);

// Count only button packets received from the FPGA, never local/synthetic pokes.
void Button_NotePhysicalEvent(void);
uint32_t Button_PhysicalEventCount(void);
void Button_Update(const uint16_t NewButtons);
ButtonState_t Button_GetState(const Button_t b);
const char* Button_GetNameStr(const Button_t b);
const char* Button_GetStateStr(const ButtonState_t s);
void Button_ResetAll(void);
void Button_RegisterOnButtonPokeCb(fnOnButtonPokeCb_t Handler);
uint16_t Button_GetPokedInputs(void);
// Fake button input (the `poke` console command).
// Returns false if the FPGA queue did not accept the input.
bool Button_Poke(uint16_t Inputs);
// Next queued console poke, false when none is left.
bool Button_TakePokedInputs(uint16_t *pInputs);
void Button_RegisterCommands(void);
