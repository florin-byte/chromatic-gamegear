// Exercise the real button implementation with a bounded host queue.
#include "../button.c"
#include "../../mutex/mutex.h"
MutexResult_t Mutex_Take(MutexKey_t key) { (void)key; return kMutexResult_Ok; }
MutexResult_t Mutex_Give(MutexKey_t key) { (void)key; return kMutexResult_Ok; }
static int notified;
static void Notify(void) { notified++; }
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    uint16_t value;
    CHECK(Button_PhysicalEventCount() == 0);
    Button_NotePhysicalEvent();
    CHECK(Button_PhysicalEventCount() == 1);
    Button_RegisterOnButtonPokeCb(Notify);
    visible = false;
    CHECK(Button_Poke(kButtonPoke_Menu));
    visible = true;
    CHECK(Button_Poke(0));
    CHECK(Button_TakePokedInputs(&value) && value == kButtonPoke_Menu);
    CHECK(Button_TakePokedInputs(&value) && value == 0);
    CHECK(notified == 2);
    CHECK(!Button_TakePokedInputs(&value));
    // Ordinary visible inputs remain local.
    CHECK(Button_Poke(kButtonBits_A));
    CHECK(Button_GetState(kButton_A) == kButtonState_Pressed);
    CHECK(!Button_TakePokedInputs(&value));
    // An alternate menu bit retains its existing forwarding behavior.
    CHECK(Button_Poke(kButtonBits_MenuEn));
    CHECK(Button_Poke(0));
    CHECK(Button_TakePokedInputs(&value) && value == kButtonBits_MenuEn);
    CHECK(Button_TakePokedInputs(&value) && value == 0);
    // A failed release must retain the pending menu state for another attempt.
    CHECK(Button_Poke(kButtonPoke_Menu));
    for (int i = 1; i < kPokedInputsQLen; i++) CHECK(Button_Poke(kButtonPoke_Menu));
    CHECK(!Button_Poke(0));
    CHECK(Button_TakePokedInputs(&value));
    CHECK(Button_Poke(0));
    while (Button_TakePokedInputs(&value)) {}
    CHECK(value == 0);
    CHECK(Button_Poke(kButtonBits_B));
    CHECK(!Button_TakePokedInputs(&value));
    // Invisible ordinary inputs still reach the FPGA.
    visible = false;
    CHECK(Button_Poke(kButtonBits_A));
    CHECK(Button_TakePokedInputs(&value) && value == kButtonBits_A);
    CHECK(Button_PhysicalEventCount() == 1);
    printf("routing_test: %u checks passed\n", checks);
    return 0;
}
