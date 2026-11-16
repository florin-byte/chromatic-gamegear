; cf_writer.asm: the cart writer, a WRAM program (lane cartflash1). DRAFT.
;
; Loaded at $C000 by the netboot stub (cf_netboot.inc) from the ESP32 (CF_LOAD), never
; from the cartridge. It is a dumb executor: it sends CF_NEXT (op 0x21) with the result
; of the last command and gets the next command back; the ESP32 (cartflash/cf_esp.c)
; holds the plan, the chip table, the journal and every safety rule. The protocol is in
; cartflash/cf_proto.h. The writer keeps no state across a power cut: after one, the
; ESP32 resumes the job from its journal and the writer starts with HELLO again.
;
;
; v2 (after unit 1, 2026-10-10): the cart's flash ignores GB-timed write cycles, so every flash
; command / program / erase cycle goes through the FPGA cart bus engine (FF60-FF67) with
; static-pin timing; reads, CRC, blank checks, bank switching and cart RAM stay GB cycles.
; Bus rules this code keeps (CART_FLASH_DESIGN.md "Cart bus"):
; - instructions are fetched from WRAM only; the boot ROM stays mapped unless a READ asks
;   for the unmap flag, and nothing ever runs from the cartridge;
; - flash command cycles go to CPU 0x0000-0x1FFF (MBC5: RAM-enable register, so RAM stays
;   disabled through them); program data and sector addresses go through the 0x4000
;   window with the bank mapped first; the bank register is never written between the
;   cycles of a command or while the chip is busy (a stray write could suspend an erase);
; - busy = DQ6 toggling (two reads differ), DQ5 = chip-reported failure, plus a software
;   timeout; after any failure the chip gets the reset command F0;
; - interrupts stay off.

DEF NW_ID   EQU $FF58
DEF NW_CMD  EQU $FF5A
DEF NW_STAT EQU $FF5B
DEF NW_PTRL EQU $FF5C
DEF NW_PTRH EQU $FF5D
DEF NW_DATA EQU $FF5E

DEF BUF     EQU $D000
DEF BUF_MAX EQU 4064
DEF TabLo   EQU $CC00
DEF TabHi   EQU $CD00

DEF WRITER_VER EQU 5
; the cart bus engine (fpga cartflash1-engine, rtl/BSP/cart_engine.v); visible only while the
; ESP32 allows it (link cmd 8). Flash command / program / erase cycles go through it; reads, bank
; switching and cart RAM use ordinary GB bus cycles (those work on the ModRetro cart).
DEF ENG_CTRL  EQU $FF60
DEF ENG_AL    EQU $FF61
DEF ENG_AH    EQU $FF62
DEF ENG_RDATA EQU $FF63
DEF ENG_WDATA EQU $FF64
DEF ENG_PROG  EQU $FF65
DEF ENG_CMD3  EQU $FF66
DEF ENG_ERASE EQU $FF67
DEF ENG_ON      EQU 1
DEF ENG_AUTOINC EQU 2
DEF ENG_CLRERR  EQU 4
; results / commands (cf_proto.h)
DEF R_OK EQU 0
DEF R_HELLO EQU 1
DEF R_TIMEOUT EQU 2
DEF R_DQ5 EQU 3
DEF R_MISMATCH EQU 4
DEF R_BADCMD EQU 5
DEF R_RESET EQU 6
DEF R_UNSTABLE EQU 7
DEF C_WAIT EQU 0
DEF C_PROGRAM EQU 3
DEF C_RAMWRITE EQU 6
DEF N_CMDS EQU 11

; variables
DEF vSeq    EQU $CE00
DEF vRes    EQU $CE01
DEF vRlen   EQU $CE02   ; u16
DEF vRptr   EQU $CE04   ; u16
DEF vCmd    EQU $CE06
DEF vArgs   EQU $CE08   ; 16: mbc, bank u16, addr u16, len u16, flags, cmdset, op, tmo u16, cfi_mode, cfi_addr u16, 0
DEF aMbc    EQU vArgs + 0
DEF aBank   EQU vArgs + 1
DEF aAddr   EQU vArgs + 3
DEF aLen    EQU vArgs + 5
DEF aFlags  EQU vArgs + 7
DEF aCmdset EQU vArgs + 8
DEF aOp     EQU vArgs + 9
DEF aTmo    EQU vArgs + 10
DEF aCfiMode EQU vArgs + 12
DEF aCfiAddr EQU vArgs + 13
DEF vSmall  EQU $CE18   ; 8: small results
DEF vCAA    EQU $CE20   ; command bytes as written (D0/D1 swapped if the board needs it)
DEF vC55    EQU $CE21
DEF vCA0    EQU $CE22
DEF vC80    EQU $CE23
DEF vCF0    EQU $CE24
DEF vC90    EQU $CE25
DEF vC98    EQU $CE26
DEF vCOP    EQU $CE27
DEF vMm     EQU $CE28   ; u16 mismatches
DEF vTry    EQU $CE2A
DEF vHello  EQU $CE30   ; 82: ver, cgb, header 0x100-0x14F

SECTION "Writer", ROM0[$0]
LOAD "WriterWram", WRAM0[$C000]
    db "SGWX"
Entry:
    di
    ld sp, $D000
    call DoubleSpeed                ; v5 (wifispeed1): the CPU's loops (window copy, program, compare, CRC) at 2x
    call CrcInit
    call CmdHello
.loop
    call Exchange
    xor a
    ld [vRes], a
    ld [vRlen], a
    ld [vRlen + 1], a
    ld a, [vCmd]
    cp N_CMDS
    jr c, :+
    ld a, R_BADCMD
    ld [vRes], a
    jr .loop
:   add a
    ld hl, Commands
    add l
    ld l, a
    adc h
    sub l
    ld h, a
    ld a, [hli]
    ld h, [hl]
    ld l, a
    call JpHl
    jr .loop

JpHl:
    jp hl

Commands:
    dw CmdWait, CmdRead, CmdErase, CmdProgram, CmdCrc, CmdId, CmdRamWrite, CmdDone, CmdBlank, CmdHello, CmdGbTest

; ---------------------------------------------------------------- the exchange
; send CF_NEXT [seq, res, rlen, result], get [seq, cmd, args16, data]. Retries forever.
Exchange:
.retry
    xor a
    ldh [NW_PTRL], a
    ldh [NW_PTRH], a
    ld c, LOW(NW_DATA)
    inc a
    ldh [c], a                      ; ver 1
    ld a, $21
    ldh [c], a                      ; CF_NEXT
    xor a
    ldh [c], a
    ldh [c], a
    ld hl, vSeq
    ld a, [hli]
    ldh [c], a                      ; seq
    ld a, [hli]
    ldh [c], a                      ; res
    ld a, [hli]
    ld e, a
    ldh [c], a                      ; rlen
    ld a, [hli]
    ld d, a
    ldh [c], a
    ld a, [hli]
    ld h, [hl]
    ld l, a                         ; rptr
    ld a, d
    or e
    jr z, .sent
.r  ld a, [hli]
    ldh [c], a
    dec de
    ld a, d
    or e
    jr nz, .r
.sent
    call Transact
    jr nz, .again
    ld a, 10
    ldh [NW_PTRL], a
    ldh a, [c]
    ld b, a
    ldh a, [c]
    ld [vCmd], a
    ld a, b
    ld [vSeq], a
    ld hl, vArgs
    ld b, 16
:   ldh a, [c]
    ld [hli], a
    dec b
    jr nz, :-
    ld a, [vCmd]
    cp C_PROGRAM
    jr z, .data
    cp C_RAMWRITE
    ret nz
.data
    ld a, [aLen]
    ld e, a
    ld a, [aLen + 1]
    ld d, a
    cp HIGH(BUF_MAX) + 1
    jr c, :+
    ld a, $FF                       ; too long: refuse the command
    ld [vCmd], a
    ret
:   ld hl, BUF
    ld a, d
    or e
    ret z
:   ldh a, [c]
    ld [hli], a
    dec de
    ld a, d
    or e
    jr nz, :-
    ret
.again
    ld de, 250
    call DelayMs
    jp .retry

; SEND, wait for RX_READY (~12 s). z: status OK, c = LOW(NW_DATA)
Transact:
    xor a
    ldh [NW_CMD], a
    inc a
    ldh [NW_CMD], a
    ld d, 12
.o  ld bc, 0
.i  ldh a, [NW_STAT]
    bit 1, a
    jr nz, .ready
    bit 2, a
    jr nz, .err
    dec bc
    ld a, b
    or c
    jr nz, .i
    dec d
    jr nz, .o
.err
    ld a, 3
    ldh [NW_CMD], a                 ; ABORT
    xor a
    ldh [NW_CMD], a
    or 1
    ret
.ready
    xor a
    ldh [NW_PTRL], a
    ldh [NW_PTRH], a
    ld c, LOW(NW_DATA)
    ldh a, [c]
    ldh a, [c]
    or a
    ret

; ---------------------------------------------------------------- commands
CmdHello:
    ld a, WRITER_VER
    ld [vHello], a
    ldh a, [ENG_CTRL]               ; flags bit 1: a cart bus engine answers (STAT = $A_)
    and $F0
    cp $A0
    ld a, 1
    jr nz, :+
    ld a, 3
:   ld [vHello + 1], a
    ld hl, $0100                    ; header (cart, also while the boot ROM is mapped)
    ld de, vHello + 2
    ld b, 80
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    xor a
    ld [vSeq], a
    ld a, R_HELLO
    ld [vRes], a
    ld a, 82
    ld [vRlen], a
    xor a
    ld [vRlen + 1], a
    ld hl, vHello
    jp SetRptr

CmdWait:
    ld a, [aTmo]
    ld e, a
    ld a, [aTmo + 1]
    ld d, a
    jp DelayMs

CmdDone:
    xor a
    ldh [NW_ID], a                  ; lock the window
    ld [$1000], a                   ; RAM off (1000: 0000 is under the mapped boot ROM)
    ld a, [aMbc]
    cp 5
    jr nz, .h
    ld a, 1
    ld [$2000], a
    xor a
    ld [$3000], a
.h  halt                            ; the ESP32 power-cycles the cart (GB reset)
    jr .h

; READ: data (each byte read twice and compared, up to 4 tries) -> result
CmdRead:
    ld a, [aFlags]
    bit 3, a                        ; F_GBT: the GB-timed route (RAM enable at 1000, never 0000)
    jr nz, :+
    and 5                           ; F_RAM (or the old F_ENG): cart RAM, through the engine
    jp nz, CmdReadEng
:
    call Setup
    ld a, 4
    ld [vTry], a
.again
    call ArgsHlBc
    ld de, BUF
:   ld a, [hli]
    ld [de], a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, :-
    call ArgsHlBc
    ld de, BUF
:   ld a, [de]
    cp [hl]
    jr nz, .diff
    inc hl
    inc de
    dec bc
    ld a, b
    or c
    jr nz, :-
    jr .ok
.diff
    ld a, [vTry]
    dec a
    ld [vTry], a
    jr nz, .again
    ld a, R_UNSTABLE
    ld [vRes], a
.ok
    call RamOffIf
    ld a, [aLen]
    ld [vRlen], a
    ld a, [aLen + 1]
    ld [vRlen + 1], a
    ld hl, BUF
    jp SetRptr

; CRC16-CCITT (init FFFF) of len bytes -> result u16
CmdCrc:
    call Setup
    call ArgsHlBc
    ld de, $FFFF
.l  ld a, [hli]
    xor d
    push hl
    ld l, a
    ld h, HIGH(TabHi)
    ld a, [hl]
    xor e
    ld d, a
    dec h
    ld e, [hl]
    pop hl
    dec bc
    ld a, b
    or c
    jr nz, .l
    ld a, e
    ld [vSmall], a
    ld a, d
    ld [vSmall + 1], a
    call RamOffIf
    ld a, 2
    ld [vRlen], a
    ld hl, vSmall
    jp SetRptr

; BLANK: every byte must read FF -> OK, else MISMATCH [off, FF, got, count]
; First a fast pass: AND of all bytes, 8 per turn (4.5 M-cycles a byte against 15: a 16 KiB
; bank in ~70 ms instead of ~235 ms; a unit spent 22 s "tidying" behind a 256 KiB game). Only
; when that is not FF does the slow pass run, which finds the first wrong byte and counts them.
CmdBlank:
    call Setup
    xor a
    ld [vMm], a
    ld [vMm + 1], a
    call ArgsHlBc
    ld a, c
    and 7
    jr nz, .slow                    ; a length that is not a multiple of 8: the slow pass
REPT 3
    srl b
    rr c
ENDR
    ld a, b
    or c
    jr z, .slow
    ld a, c
    or a
    jr z, :+
    inc b
:   ld a, $FF
.f
REPT 8
    and [hl]
    inc hl
ENDR
    dec c
    jr nz, .f
    dec b
    jr nz, .f
    inc a
    jp z, MmFinish                  ; every byte FF (vMm is 0: OK)
.slow
    call ArgsHlBc
.l  ld a, [hl]
    inc a
    jr z, .n
    ld a, $FF
    ld [vSmall + 6], a
    ld a, [hl]
    call Mismatch
.n  inc hl
    dec bc
    ld a, b
    or c
    jr nz, .l
    jp MmFinish

; ERASE: unlock, 80, unlock, op at the sector address; wait (tmo x 16 ms)
; ERASE through the cart bus engine (FF67): unlock 80 unlock op@sector, DQ6 polling and the
; erase timeout in hardware. The mapper's bank is set by an ordinary GB write first.
CmdErase:
    call Setup
    ld a, [aFlags]
    bit 3, a
    jp nz, GbtErase
    ld a, ENG_ON | ENG_CLRERR
    ldh [ENG_CTRL], a
    ld hl, aAddr
    ld a, [hli]
    ld h, [hl]
    ld l, a
    call EngAddrHL
    ld a, [aOp]
    ldh [ENG_ERASE], a
    call EngWait
    ld b, a
    xor a
    ldh [ENG_CTRL], a               ; session off: the bus is the GB's again
    bit 2, b
    ret z
    ld a, R_TIMEOUT
    ld [vRes], a
    ret

; PROGRAM through the engine (FF65 per byte: unlock A0 byte, DQ6 poll, verify, ADDR++; FF bytes
; are skipped by the engine), then the usual compare of everything with ordinary GB reads
CmdProgram:
    call Setup
    ld a, [aFlags]
    bit 3, a
    jp nz, GbtProgram
    ld a, ENG_ON | ENG_CLRERR
    ldh [ENG_CTRL], a
    call ArgsHlBc
    call EngAddrHL
    ld de, BUF
.p  call EngWait
    ld a, [de]
    inc de
    ldh [ENG_PROG], a
    dec bc
    ld a, b
    or c
    jr nz, .p
    call EngWait
    ld b, a
    xor a
    ldh [ENG_CTRL], a
    bit 2, b
    jr z, :+
    ld a, R_TIMEOUT
    ld [vRes], a
    ret
:   call ArgsHlBc
    ld de, BUF
    jp CompareBuf

; ID: autoselect (16 bytes at flash 0), CFI (128 bytes at flash 0), reset, header check.
; Result [16 id, 128 cfi, reset_ok], 145 bytes.
; ID through the engine: CMD3 90, 16 bytes at cart 0x0000 (the engine reads the pins: no boot
; ROM shadow), CMD3 F0; CFI: cfi_mode 2 = CMD3 98 (SST), 1 = 98 written at cfi_addr; 128 bytes;
; F0. Then the header must read back with ordinary GB reads. Result [16 id, 128 cfi, reset_ok].
CmdId:
    ld a, [aFlags]
    bit 3, a
    jp nz, GbtId
    ld a, ENG_ON | ENG_AUTOINC | ENG_CLRERR
    ldh [ENG_CTRL], a
    ld a, $90
    ldh [ENG_CMD3], a
    call EngWait
    ld hl, $0000
    call EngAddrHL
    call EngWait
    ld de, BUF
    ld b, 16
    call EngRead
    ld a, $F0
    ldh [ENG_CMD3], a
    call EngWait
    ld a, [aCfiMode]
    or a
    jr z, .nocfi
    cp 2
    jr z, .sst
    ld hl, aCfiAddr                 ; 1: 98 at cfi_addr
    ld a, [hli]
    ld h, [hl]
    ld l, a
    call EngAddrHL
    call EngWait
    ld a, $98
    ldh [ENG_WDATA], a
    jr .cfi
.sst
    ld a, $98
    ldh [ENG_CMD3], a
.cfi
    call EngWait
    ld hl, $0000
    call EngAddrHL
    call EngWait
    ld de, BUF + 16
    ld b, 128
    call EngRead
    ld a, $F0
    ldh [ENG_CMD3], a
    call EngWait
    jr .chk
.nocfi
    ld hl, BUF + 16
    ld b, 128
    ld a, $FF
:   ld [hli], a
    dec b
    jr nz, :-
.chk
    xor a
    ldh [ENG_CTRL], a
    call CheckHdr
    ld a, 1
    jr z, :+
    ld a, R_RESET
    ld [vRes], a
    xor a
:   ld [BUF + 144], a
    ld a, 145
    ld [vRlen], a
    ld hl, BUF
    jp SetRptr

; ---------------------------------------------------------------- GB-timed route (v4, flag F_GBT)
; Plain CPU cycles, for a bitstream without the engine. Nothing is aimed under the mapped boot
; ROM (gb.v drops CPU writes to 0000-00FF / 0200-08FF while it is mapped): flash commands go to
; 1AAA / 1555 (the chip decodes A11-A0: AAA / 555; the MBC5 also sees a RAM enable / disable
; there, harmless), the data and the polls to 4000-7FFF (Setup put the bank there), RAM
; enable to 1000. Proven on unit 1 (2026-10-11 gbtest): RAM reads and the flash ID. Program
; and erase: untested on hardware (cartflash3 report 0m).
GbtUnlock:
    ld a, $AA
    ld [$1AAA], a
    ld a, $55
    ld [$1555], a
    ret
; DQ6 toggle poll at hl until two reads agree; c:b = the outer:inner budget. nz = timed out
GbtPoll:
    push de
.l  ld a, [hl]
    ld e, a
    ld a, [hl]
    xor e
    and $40
    jr z, .done
    dec b
    jr nz, .l
    dec c
    jr nz, .l
    or 1
.done
    pop de
    ret

GbtErase:
    call GbtUnlock
    ld a, $80
    ld [$1AAA], a
    call GbtUnlock
    ld hl, aAddr
    ld a, [hli]
    ld h, [hl]
    ld l, a
    ld a, [aOp]
    cp $10
    jr nz, :+
    ld hl, $1AAA                    ; chip erase: at AAA
:   ld [hl], a
    ld hl, aAddr
    ld a, [hli]
    ld h, [hl]
    ld l, a
    ld bc, 0                        ; 65536 polls x ~25 us: ~1.6 s, then again
    call GbtPoll
    jr z, .ok
    ld bc, 0
    call GbtPoll
    jr z, .ok
    ld bc, 0
    call GbtPoll
    jr z, .ok
    ld a, R_TIMEOUT
    ld [vRes], a
.ok
    ld a, $F0
    ld [$1000], a                   ; read mode (also RAM off)
    ret

GbtProgram:
    call ArgsHlBc
    ld de, BUF
.p  ld a, [de]
    cp $FF
    jr z, .n                        ; FF: erased already
    push bc
    call GbtUnlock
    ld a, $A0
    ld [$1AAA], a
    ld a, [de]
    ld [hl], a
    ld bc, $0410                    ; ~1000 polls: tBP max 10 us, room for slower parts
    call GbtPoll
    pop bc
    jr z, .n
    ld a, R_TIMEOUT
    ld [vRes], a
    ld a, $F0
    ld [$1000], a
    ret
.n  inc hl
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .p
    call ArgsHlBc
    ld de, BUF
    jp CompareBuf

; ID: 16 autoselect bytes of ROM bank 0 at 4000, no CFI, then the header must read back.
GbtId:
    xor a
    ld [$2000], a
    ld [$3000], a
    call GbtUnlock
    ld a, $90
    ld [$1AAA], a
    ld hl, $4000
    ld de, BUF
    ld b, 16
    call Copy8
    ld a, $F0
    ld [$1000], a
    ld hl, BUF + 16
    ld b, 128
    ld a, $FF
:   ld [hli], a
    dec b
    jr nz, :-
    ld a, 1
    ld [$2000], a
    jp CmdId.chk

; RAMWRITE, GB-timed: enable at 1000, write, compare, disable
GbtRamWrite:
    call Setup
    call ArgsHlBc
    ld de, BUF
:   ld a, [de]
    ld [hli], a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, :-
    call ArgsHlBc
    ld de, BUF
    call CompareBuf
    jp RamOffIf

; ---------------------------------------------------------------- cart RAM (v3: engine cycles only)
; Why: this program runs while the boot ROM is still mapped, and gb.v keeps CPU writes to
; 0000-00FF and 0200-08FF off the cart bus then, so the GB-timed RAM enable 0A@0000 never
; reached the cart (unit 1, 2026-10-11: RAM reads were bus residue, RAM writes were lost).
; The engine drives the cart pins itself (mapper registers at their real addresses, /CS for
; A000-BFFF), whatever the boot ROM state.
EngRamOn:                           ; session on, RAM enabled, RAM bank = aBank
    ld a, ENG_ON | ENG_AUTOINC | ENG_CLRERR
    ldh [ENG_CTRL], a
    call EngWait
    ld hl, $0000
    ld a, $0A
    call EngPoke
    ld hl, $4000
    ld a, [aBank]
    jp EngPoke
EngRamOff:                          ; RAM disabled, session off (the core owns the bus again)
    ld hl, $0000
    xor a
    call EngPoke
    xor a
    ldh [ENG_CTRL], a
    ret
EngArgAddr:                         ; ADDR = aAddr, prefetch done; bc = aLen
    ld a, [aAddr]
    ld l, a
    ld a, [aAddr + 1]
    ld h, a
    call EngAddrHL
    call EngWait
    ld a, [aLen]
    ld c, a
    ld a, [aLen + 1]
    ld b, a
    ret
; RDATA stream against BUF, bc bytes: z = all equal
EngCmpBuf:
    ld hl, BUF
.l  ldh a, [ENG_RDATA]
    cp [hl]
    ret nz
    inc hl
    dec bc
    ld a, b
    or c
    jr nz, .l
    ret

; READ of cart RAM (flags bit 0): len (<= BUF_MAX) bytes read through RDATA, then read again
; and compared, up to 4 tries (else UNSTABLE). Writes only the mapper registers.
CmdReadEng:
    call EngRamOn
    ld a, 4
    ld [vTry], a
.again
    call EngArgAddr
    ld de, BUF
.r  ldh a, [ENG_RDATA]
    ld [de], a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .r
    call EngArgAddr
    call EngCmpBuf
    jr z, .ok
    ld a, [vTry]
    dec a
    ld [vTry], a
    jr nz, .again
    ld a, R_UNSTABLE
    ld [vRes], a
.ok
    call EngRamOff
    ld a, [aLen]
    ld [vRlen], a
    ld a, [aLen + 1]
    ld [vRlen + 1], a
    ld hl, BUF
    jp SetRptr

; RAMWRITE: BUF[len] to cart RAM with WDATA (ADDR steps in A000-BFFF), then read back and
; compare -> OK or MISMATCH. RAM disabled after.
CmdRamWrite:
    ld a, [aFlags]
    bit 3, a
    jp nz, GbtRamWrite
    call EngRamOn
    call EngArgAddr
    ld de, BUF
.w  ld a, [de]
    ldh [ENG_WDATA], a
    call EngWait
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .w
    call EngArgAddr
    call EngCmpBuf
    jr z, .ok
    ld a, R_MISMATCH
    ld [vRes], a
.ok
    jp EngRamOff

; GBTEST (read-only experiment, 2026-10-11): can GB-timed cycles work after all, if no write is
; aimed at the addresses the mapped boot ROM blocks? Result, 196 bytes:
;   [0..63]    RAM bank 0 A000.. through the engine (the reference)
;   [64..127]  the same with GB-timed reads, RAM left enabled by the engine
;   [128..191] the same after a GB-timed disable + enable at 1000 (00@1000, 0A@1000, 00@4000)
;   [192,193]  flash ID with GB-timed cycles: ROM bank 0 at 4000, AA@1AAA 55@1555 90@1AAA,
;              read 4000 / 4001 (SST decodes A11-A0 only)
;   [194,195]  4000 / 4001 after the exit (F0@1000, then the engine's F0 as well)
; Never writes cart RAM; the flash only sees ID entry / exit.
CmdGbTest:
    xor a
    ld [aBank], a
    call EngRamOn
    ld hl, $A000
    call EngAddrHL
    call EngWait
    ld de, BUF
    ld b, 64
    call EngRead
    xor a
    ldh [ENG_CTRL], a               ; session off: the core owns the cart bus again
    ld hl, $A000
    ld de, BUF + 64
    ld b, 64
    call Copy8
    xor a
    ld [$1000], a                   ; GB-timed: RAM off
    ld a, $0A
    ld [$1000], a                   ; RAM on (1000 is the same MBC5 register as 0000)
    xor a
    ld [$4000], a
    ld hl, $A000
    ld de, BUF + 128
    ld b, 64
    call Copy8
    xor a
    ld [$1000], a
    ld [$2000], a                   ; ROM bank 0 at 4000-7FFF (MBC5 allows it)
    ld [$3000], a
    ld a, $AA
    ld [$1AAA], a
    ld a, $55
    ld [$1555], a
    ld a, $90
    ld [$1AAA], a
    ld a, [$4000]
    ld [BUF + 192], a
    ld a, [$4001]
    ld [BUF + 193], a
    ld a, $F0
    ld [$1000], a                   ; exit (also RAM off)
    ld a, ENG_ON | ENG_AUTOINC | ENG_CLRERR
    ldh [ENG_CTRL], a
    call EngWait
    ld a, $F0
    ldh [ENG_CMD3], a               ; and the engine's exit, whatever the GB-timed one did
    call EngWait
    xor a
    ldh [ENG_CTRL], a
    ld a, [$4000]
    ld [BUF + 194], a
    ld a, [$4001]
    ld [BUF + 195], a
    ld a, 1
    ld [$2000], a
    ld a, 196
    ld [vRlen], a
    xor a
    ld [vRlen + 1], a
    ld hl, BUF
    jp SetRptr

; one engine write cycle: a at hl
EngPoke:
    push af
    call EngAddrHL
    call EngWait
    pop af
    ldh [ENG_WDATA], a
    jp EngWait

; b bytes from the engine's RDATA (autoinc) to [de]; reads are >= 2 M-cycles apart
EngRead:
    ldh a, [ENG_RDATA]
    ld [de], a
    inc de
    dec b
    jr nz, EngRead
    ret

EngAddrHL:
    ld a, l
    ldh [ENG_AL], a
    ld a, h
    ldh [ENG_AH], a
    ret

; wait until the engine is idle; a = STAT (bit 2 timeout, bit 3 verify). The engine's own
; timeouts (program ~100 us, erase ~100 ms) always end busy.
EngWait:
    ldh a, [ENG_CTRL]
    cp $FF                          ; no engine (or not allowed): returns as a timeout
    ret z
    bit 1, a
    jr nz, EngWait
    ret

CheckHdr:
    ld hl, $0100
    ld de, vHello + 2
    ld b, 80
:   ld a, [de]
    cp [hl]
    ret nz
    inc hl
    inc de
    dec b
    jr nz, :-
    ret

Copy8:
    ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, Copy8
    ret

; ---------------------------------------------------------------- helpers
; map for the command: flags bit 1 = unmap the boot ROM first; bit 0 = cart RAM (enable,
; RAM bank = bank); else ROM bank -> 0x4000
Setup:
    ld a, [aFlags]
    bit 1, a
    jr z, :+
    ld a, $11
    ldh [$FF50], a
    ld a, [aFlags]
:   bit 0, a
    jr z, .rom
    ld a, [aMbc]
    cp 1
    jr nz, :+
    ld a, 1
    ld [$6000], a
:   ld a, $0A
    ld [$1000], a                   ; 1000 = the same MBC register; 0000 is under the mapped boot ROM
    ld a, [aBank]
    ld [$4000], a
    ret
.rom
    ld a, [aBank]
    ld e, a
    ld a, [aBank + 1]
    ld d, a
    ld a, [aMbc]
    cp 5
    jr z, .m5
    cp 1
    jr z, .m1
    cp 3
    ret nz
    ld a, e
    ld [$2000], a
    ret
.m5 ld a, e
    ld [$2000], a
    ld a, d
    and 1
    ld [$3000], a
    ret
.m1 ld a, e
    and $1F
    ld [$2000], a
    ld a, e
    swap a
    rrca
    and 3
    ld [$4000], a
    ret

RamOffIf:
    ld a, [aFlags]
    bit 0, a
    ret z
    xor a
    ld [$1000], a                   ; 1000 = the same MBC register; 0000 is under the mapped boot ROM
    ld a, [aMbc]
    cp 1
    ret nz
    xor a
    ld [$6000], a
    ret

ArgsHlBc:
    ld hl, aLen
    ld a, [hli]
    ld c, a
    ld b, [hl]
    ld hl, aAddr
    ld a, [hli]
    ld h, [hl]
    ld l, a
    ret

SetRptr:
    ld a, l
    ld [vRptr], a
    ld a, h
    ld [vRptr + 1], a
    ret

; compare cart [hl] with [de], bc bytes -> OK or MISMATCH [off u16, want, got, count u16]
CompareBuf:
    xor a
    ld [vMm], a
    ld [vMm + 1], a
.l  ld a, [de]
    cp [hl]
    jr z, .n
    ld [vSmall + 6], a
    ld a, [hl]
    call Mismatch
.n  inc hl
    inc de
    dec bc
    ld a, b
    or c
    jr nz, .l
; fall through
MmFinish:
    ld a, [vMm]
    ld b, a
    ld a, [vMm + 1]
    or b
    ret z
    ld a, [vMm]
    ld [vSmall + 4], a
    ld a, [vMm + 1]
    ld [vSmall + 5], a
    ld a, R_MISMATCH
    ld [vRes], a
    ld a, 6
    ld [vRlen], a
    ld hl, vSmall
    jp SetRptr

; a = the byte read at hl; vSmall+6 = the wanted byte. Records the first, counts all.
Mismatch:
    push bc
    ld b, a
    ld a, [vMm]
    ld c, a
    ld a, [vMm + 1]
    or c
    jr nz, .cnt
    ld a, b
    ld [vSmall + 3], a
    ld a, [vSmall + 6]
    ld [vSmall + 2], a
    ld a, [aAddr]
    ld c, a
    ld a, l
    sub c
    ld [vSmall], a
    ld a, [aAddr + 1]
    ld c, a
    ld a, h
    sbc c
    ld [vSmall + 1], a
.cnt
    push hl
    ld hl, vMm
    inc [hl]
    jr nz, :+
    inc hl
    inc [hl]
:   pop hl
    pop bc
    ret

; de = milliseconds (normal speed, ~1050 M-cycles per ms)
; CGB double speed (KEY1 + STOP; interrupts off, P1 deselected). Cart reads at double speed are what
; CGB games do; flash command / program / erase cycles are the FPGA engine's own static-pin timing.
; The power cycle at the job's end puts the console back to normal speed.
DoubleSpeed:
    ldh a, [$FF4D]
    bit 7, a
    ret nz                          ; already
    xor a
    ldh [$FFFF], a                  ; IE: nothing may wake the STOP early
    ldh [$FF0F], a
    ld a, $30
    ldh [$FF00], a
    ld a, 1
    ldh [$FF4D], a
    stop
    nop
    ret

DelayMs:                            ; de milliseconds at DOUBLE speed (300 x ~13 cycles of 2.1 MHz M-cycles)
    ld a, d
    or e
    ret z
.ms ld bc, 300
.i  dec bc
    ld a, b
    or c
    jr nz, .i
    dec de
    ld a, d
    or e
    jr nz, .ms
    ret

; CRC16-CCITT tables (poly 1021) at TabLo / TabHi
CrcInit:
    ld c, 0
.t  ld d, c
    ld e, 0
    ld b, 8
.b  sla e
    rl d
    jr nc, :+
    ld a, d
    xor $10
    ld d, a
    ld a, e
    xor $21
    ld e, a
:   dec b
    jr nz, .b
    ld h, HIGH(TabLo)
    ld l, c
    ld [hl], e
    inc h
    ld [hl], d
    inc c
    jr nz, .t
    ret
WriterEnd:
ENDL

ASSERT WriterEnd <= TabLo, "writer code overlaps its CRC tables"
