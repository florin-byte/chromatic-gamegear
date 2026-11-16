#pragma once
// What a button poke may carry (Button_Poke -> FPGA UART address 9). No
// dependencies: the Mac test components/button/host/poke_test.c includes it.
//
// The FPGA keeps 9 bits of a poke (system_monitor.sv: MCU_buttons <= rx_data[8:0];
// the same in stock 18.14 and in 18.21):
//   bits 0-7  START, SELECT, B, A, UP, RIGHT, LEFT, DOWN
//   bit 8     the MENU button (top.v ORs it into the debounced MENU line; the
//             OSD opens or closes when it is released)
// Bit 9 exists only in the other direction, in the FPGA's button status word,
// where it means "the OSD is closed" (kButton_MenuEn; bit 8 there is "MENU is
// pressed", kButton_MenuEnAlt). A poke with bit 9 does nothing: until 0.17.5
// the room menu's tap used it and the menu never opened on a real unit.
enum {
    kButtonPoke_Menu       = 0x100,
    // MENU + A + B + SELECT + START: the Game Boy reset. Poked game buttons do
    // not reach the FPGA's OSD logic, so releasing this also toggles the OSD.
    kButtonPoke_ResetCombo = 0x10F,
    kButtonPoke_FpgaMask   = 0x1FF,
};
