// poke_test.c: pins the MENU poke mask (0.17.6). Not part of the IDF build.
//   make -C components/button/host test
//
// On real units (2026-10-11) the room menu's MENU tap used mask 0x200 and the
// OSD never opened; 0x100 opens it. The FPGA side, from system_monitor.sv and
// top.v (stock 18.14, 18.21, 18.22 alike), is modelled below.
#include "../button_poke.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _Checks, _Fails;
#define CHECK(c) do { _Checks++; if (!(c)) { _Fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// The FPGA: MCU_buttons <= rx_data[8:0]; MENU = MCU_buttons[8]; the OSD
// toggles when MENU is released after a press. Only the physical game buttons
// cancel that (poked ones are not wired to the OSD logic). The Game Boy resets
// on MENU + A + B + SELECT + START while the OSD is closed.
typedef struct { unsigned Reg; int OsdOpen; int MenuDown; int Resets; } Fpga_t;
static void Poke(Fpga_t *f, unsigned Mask)
{
    const unsigned was = f->Reg;
    f->Reg = Mask & 0x1FF;
    const int menu = (f->Reg >> 8) & 1, menu_was = (was >> 8) & 1;
    if (menu && !menu_was) f->MenuDown = 1;
    if (!menu && menu_was && f->MenuDown) { f->OsdOpen = !f->OsdOpen; f->MenuDown = 0; }
    if (!f->OsdOpen && (f->Reg & 0x10F) == 0x10F && (was & 0x10F) != 0x10F) f->Resets++;
}

static char *Slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    s[n] = '\0';
    fclose(f);
    return s;
}

int main(void)
{
    // The masks.
    CHECK(kButtonPoke_Menu == 0x100);
    CHECK(kButtonPoke_Menu == (1 << 8));
    CHECK(kButtonPoke_FpgaMask == 0x1FF);
    CHECK((kButtonPoke_Menu & kButtonPoke_FpgaMask) == kButtonPoke_Menu);
    CHECK(kButtonPoke_ResetCombo == (0x100 | 0x08 | 0x04 | 0x02 | 0x01));

    // A tap with the mask opens the OSD, and a second one closes it.
    Fpga_t f = {0};
    Poke(&f, kButtonPoke_Menu); Poke(&f, 0);
    CHECK(f.OsdOpen == 1 && f.Resets == 0);
    Poke(&f, kButtonPoke_Menu); Poke(&f, 0);
    CHECK(f.OsdOpen == 0);
    // The mask used until 0.17.5 does nothing at all.
    memset(&f, 0, sizeof(f));
    Poke(&f, 0x200); Poke(&f, 0);
    CHECK(f.OsdOpen == 0 && f.Reg == 0);
    // The reset combo resets the game and leaves the OSD open behind it; with
    // the OSD already open it resets nothing and closes the OSD.
    memset(&f, 0, sizeof(f));
    Poke(&f, kButtonPoke_ResetCombo); Poke(&f, 0);
    CHECK(f.Resets == 1 && f.OsdOpen == 1);
    Poke(&f, kButtonPoke_ResetCombo); Poke(&f, 0);
    CHECK(f.Resets == 1 && f.OsdOpen == 0);
    // cwl cartcycle's sequence (close first if open, combo, close if it opened).
    for (int open = 0; open < 2; open++) {
        memset(&f, 0, sizeof(f));
        f.OsdOpen = open;
        if (f.OsdOpen) { Poke(&f, kButtonPoke_Menu); Poke(&f, 0); }
        Poke(&f, kButtonPoke_ResetCombo); Poke(&f, 0);
        if (f.OsdOpen) { Poke(&f, kButtonPoke_Menu); Poke(&f, 0); }
        CHECK(f.Resets == 1 && f.OsdOpen == 0);
    }

    // Every place in the firmware that synthesizes MENU uses the named masks.
    char *osd = Slurp("../../osd/osd_default.c");
    char *cwl = Slurp("../../cwl/cwl_esp.c");
    CHECK(osd != NULL && cwl != NULL);
    if (osd && cwl) {
        CHECK(strstr(osd, "Button_Poke(kButtonPoke_Menu)") != NULL);
        CHECK(strstr(osd, "Button_Poke(kButtonBits_MenuEn)") == NULL);
        CHECK(strstr(cwl, "Button_Poke(kButtonPoke_ResetCombo)") != NULL);
        CHECK(strstr(cwl, "Button_Poke(kButtonPoke_Menu)") != NULL);
        CHECK(strstr(cwl, "Button_Poke(0x") == NULL && strstr(osd, "Button_Poke(0x") == NULL);
    }
    free(osd); free(cwl);

    printf("%d checks, %d failed\n", _Checks, _Fails);
    return _Fails ? 1 : 0;
}
