/* shl_dev.h: a Chromatic's network side on a Mac (lane chromshlink1).
 *
 * The REAL ESP32 code (components/nw: nw_esp.c, nw_link.c, shl_client.c)
 * behind the packet-level FPGA model (tools/nw/nw_fpga_model.c) and a
 * 115200-baud UART model, with a real UDP socket to a Shiny Relay. What runs
 * here is byte-for-byte what the ESP32 runs; only time, the socket and the
 * ticket source are the host's.
 *
 * Emulated time drives the window (shl_dev_step_ms, every 1 ms of GB time);
 * wall time drives the network (shl_dev_net, as often as possible).
 */
#ifndef SHL_DEV_H
#define SHL_DEV_H
#include <stdint.h>
#include "nw_fpga_model.h"
#include "nw_esp.h"
#include "nw_link.h"
#include "shl_client.h"

extern nw_fpga_t   DF;
extern nw_esp_t    DE;
extern NwLink_t    DL;
extern ShlClient_t DC;
extern int         dev_asked;          /* the game asked for the room menu (count) */
extern int         dev_need_ticket;    /* the client is waiting for a ticket */
extern char        dev_ticket_cmd[512];/* run to get a ticket (stdout), when set */

/* relay "host:port"; client id; returns 0 on success */
int  shl_dev_init(const char *relay, uint64_t client_id, int window_present);
void shl_dev_step_ms(void);            /* one ms of GB time: UART + FPGA + ESP32 ticks */
void shl_dev_net(void);                /* the socket and the client's timers (wall time) */
uint32_t shl_dev_wall_ms(void);
/* The OSD room menu's choices: "host", "mine", "join CODE", "room ID", "leave" */
int  shl_dev_room(const char *what);
void shl_dev_ticket(const char *ticket);
/* status as one JSON line */
int  shl_dev_status(char *out, int cap);
#endif
