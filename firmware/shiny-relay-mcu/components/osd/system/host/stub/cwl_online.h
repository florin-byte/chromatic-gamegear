/* host stub of the few cwl_online.h calls SYSTEM > CUSTOM makes (custom_shots.c scripts them) */
#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef enum { kCwlPair_Idle, kCwlPair_Starting, kCwlPair_Waiting, kCwlPair_Done, kCwlPair_Failed } CwlPairState_t;
bool CwlOnline_Paired(void);
const char *CwlOnline_DeviceId(void);
void CwlOnline_PairStart(void);
void CwlOnline_PairCancel(void);
CwlPairState_t CwlOnline_PairState(void);
const char *CwlOnline_PairError(void);
void CwlOnline_PairCode(char *code, size_t code_len, char *url, size_t url_len);
