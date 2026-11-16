#pragma once

#include "fpga_common.h"

typedef enum {
    kRxCmd_VoltageAA       = 0x0,
    kRxCmd_VoltageLiPo     = 0x1,
    kRxCmd_Buttons         = 0x2,
    kRxCmd_AudioBrightness = 0x3,
    kRxCmd_SysCtl          = 0x4,
    kRxCmd_PMIC            = 0x5,
    kRxCmd_FWVer           = 0x6,
    kRxCmd_Reserved        = 0x7,
    kRxCmd_StatusExtended  = 0x8,
    kRxCmd_BGPalette       = 0x9,
    kRxCmd_LinkEvent       = 0xA,   // CWL1: [event, byte], see cwl.h
    kRxCmd_NetWindow       = 0xB,   // network window (FPGA 18.21+): up to 255 bytes, see nw_dev.h
    kRxCmd_CartXs          = 0xC,   // cartspeed1: express cart stream status (FPGA 18.26+): 32 bytes, see cf_proto.h

    kNumRxCmds,
} RxIDs_t;

typedef enum {
    kFPGA_RxConsts_BufferSize = 1024,    // [bytes]
} FPGA_RxConsts_t;

void FPGA_RxTask(void *arg);
void FPGA_Rx_Resume(void);
void FPGA_Rx_Pause(void);
void FPGA_Rx_UseBrightnessReadback(void);
void FPGA_Tx_PokeButtons(void);
