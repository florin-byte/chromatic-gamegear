// cwl_priv.h: between cwl_esp.c (the link session, owner of the Wi-Fi driver)
// and cwl_net.c (network choice). Not for other components.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pause (true) or resume (false) the link session, so cwl_net can use the
// radio alone (scan, join, phone setup). true returns once the session has
// stopped and Wi-Fi is off.
void CWL_NetHold(bool On);
bool CWL_NetHeld(void);
// esp_netif + esp_wifi_init (once), as the link session does it.
esp_err_t CWL_WifiDriverInit(void);
// The network the link joins (NVS "cwl" ssid/pass). The password only ever
// travels between these two files.
void CWL_GetCurrentCredentials(char *Ssid, size_t SsidLen, char *Pass, size_t PassLen);
void CWL_SetCurrentNetwork(const char *Ssid, const char *Pass);
// The channel the current network was just joined on (0: unknown, full scan).
void CWL_SetChannelHint(uint8_t Channel);
// The link task's own retry: after a CwlNet auto-join switch.
void CWL_RejoinNow(void);
// lane share1: the channel a share receiver listens on (the last AP's, else the ESP-NOW home channel)
uint8_t CWL_ShareChannel(void);
