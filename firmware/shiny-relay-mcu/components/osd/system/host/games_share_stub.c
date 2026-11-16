// games_shots (m110 merge): SHARE as a build with no radio (share.c's own stubs), so the GAMES page links on the host.
// m110s: games_shots sets GamesShare_Start to what the next SHARE answers (Share_Available then says yes)
#include "share.h"
int GamesShare_Start = -1;                 // -1: no SHARE on this build; else kShare_*
int GamesShare_Calls;
bool Share_Available(void) { return GamesShare_Start >= 0; }
int Share_SendStart(ShareSrc_t Src, uint32_t Uid, const char *Path, const char *Title, fnOSDButtonCb_t Back) { (void)Src; (void)Uid; (void)Path; (void)Title; (void)Back; GamesShare_Calls++; return GamesShare_Start; }
int Share_RecvStart(fnOSDButtonCb_t Back) { (void)Back; return kShare_Busy; }
bool Share_Rows(ShareRows_t *Out) { (void)Out; return false; }
void Share_HostLeft(void) {}
