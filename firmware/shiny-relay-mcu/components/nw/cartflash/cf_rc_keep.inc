/* cf_rc_keep.inc (lane m110x): a keep over the USB cable (lib imp, its begin / part / end) while a game plays from memory
 * with no cartridge (a Wi-Fi Just play it played from the staging area, a kept game, a card game). The game stops first, its
 * save kept, as the Wi-Fi keep's receive mode stops it (CfRc_Yield), then the keep goes on. Before this the console answered
 * -98 (busy) for as long as the game played, and the page could only say "busy" (m110g1 finding 2, unit 1).
 *
 * Built into cf_rc_dev.c and into the Mac test host/cf_keep_test.c (the same code). The includer gives:
 *   static int  kp_playing(void);        a game plays (or loads) from memory
 *   static int  kp_loading(void);        it is still loading (the task stops only a game that plays)
 *   static int  kp_stop_pending(void);   a stop was asked and the RAM cart task has not taken it yet
 *   static void kp_stop(void);           ask the RAM cart task to stop it (its save is copied first)
 *   static int  kp_lib_busy(void);       a library op (the open a `lib st` asked for) still runs
 *   static void kp_delay_ms(int ms);
 *   static void kp_say(const char *m);   the console's line */

enum { KP_STOP_MS = 30000, KP_LIB_MS = 30000, KP_STEP_MS = 50 };

/* 0: nothing plays now (it stopped, or nothing played); -1: it did not stop in time. *stopped: a game was stopped */
static int kp_stop_memory_game(int *stopped)
{
    *stopped = 0;
    if (!kp_playing() && !kp_stop_pending()) return 0;
    kp_say("lib: the game playing stops first (its save is kept)");
    if (kp_playing() && !kp_stop_pending()) { kp_stop(); *stopped = 1; }
    int again = kp_loading();                        /* (a game still loading lets a stop go by: asked again once it plays) */
    for (int t = 0; t < KP_STOP_MS / KP_STEP_MS && (kp_playing() || kp_stop_pending()); t++) {
        kp_delay_ms(KP_STEP_MS);
        if (kp_loading()) again = 1;
        else if (again && kp_playing() && !kp_stop_pending()) { kp_stop(); *stopped = 1; again = 0; }
    }
    return kp_playing() || kp_stop_pending() ? -1 : 0;
}

/* before a keep: nothing playing from memory, no library op running. 0 = go on, -98 = busy (the console's word) */
static int kp_keep_ready(void)
{
    int stopped;
    if (kp_stop_memory_game(&stopped)) return -98;
    for (int t = 0; t < KP_LIB_MS / KP_STEP_MS && kp_lib_busy(); t++) kp_delay_ms(KP_STEP_MS);
    return kp_lib_busy() ? -98 : 0;
}
