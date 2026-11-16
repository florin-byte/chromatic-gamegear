; cf_netboot.inc: network boot stub (lane cartflash1). DRAFT, TEST boot ROMs only.
;
; Called by the boot ROM right after `ld sp, $fffe` (make_sm83.sh: stock CGB boot ROM +
; this stub, assembled with -D NWTEST; never flashed, SRAM load only). In the production
; design the same stub is the ALTERNATE boot image the ESP32 selects for a cart job
; (CART_FLASH_DESIGN.md, "FPGA change"). The stub itself never touches the cartridge.
;
; It unlocks the network window, waits up to ~60 s for the ESP32's window service (after
; an FPGA SRAM load the ESP32 reboots and rejoins Wi-Fi: the first hardware run, 18.21 +
; 0.17.2, gave up after the old 4 s wait), and fetches a WRAM program for $C000, retrying
; ~1 s apart for ~60 s while the answer is OFFLINE / TIMEOUT / a window error:
;   1. CF_LOAD (op 0x20; answered by an ESP32 built with NW_CARTFLASH while a cart job
;      or `cart probe` is armed: the writer or the probe. DENIED = such an ESP32 with
;      nothing armed: boot at once. UNSUPPORTED = stock firmware: try 2.);
;   2. else IDENTIFY "SHGCARTPROBE0001" + GET cp/probe.bin (the read-only cart probe,
;      stock ESP32 0.17.0: served only by the owner's local receiver; the Shiny Gen
;      gateway answers 404 and the stub stops at once on NOT_FOUND / DENIED).
; The program must start with "SGWX"; the stub calls $C004. A program that returns gets
; a normal boot (window locked); the writer never returns. Nothing fetched: lock, return,
; boot the cart as usual.

DEF NB_ID   EQU $FF58
DEF NB_CMD  EQU $FF5A
DEF NB_STAT EQU $FF5B
DEF NB_PTRL EQU $FF5C
DEF NB_PTRH EQU $FF5D
DEF NB_DATA EQU $FF5E
DEF NB_ORG  EQU $C000
DEF NB_MAX  EQU $0E00
DEF vNbOff  EQU $CFF0   ; u16 bytes loaded
DEF vNbTot  EQU $CFF2   ; u16 image size
DEF vNbTry  EQU $CFF4   ; tries left

NetBoot:
    ld a, 60                        ; tries left (~1 s apart): the ESP32 may still be booting
    ld [vNbTry], a                  ; and joining Wi-Fi after an FPGA SRAM load (it resets too)
NbAgain:
    ld hl, NbKey                    ; (re)unlock every try: harmless when already unlocked
    ld c, LOW(NB_ID)
    ld b, 4
    call NbOut
    ldh a, [NB_ID]
    cp $4E
    ret nz                          ; no window (stock FPGA / real CGB)
    ld d, 70                        ; SVC within ~60 s
.ws ld bc, 0
.wsi
    ldh a, [NB_STAT]
    and $08
    jr nz, .svc
    dec bc
    ld a, b
    or c
    jr nz, .wsi
    dec d
    jr nz, .ws
    jp NbOut0                       ; no service at all (NETWORK off): normal boot
.svc
    xor a
    ld [vNbOff], a
    ld [vNbOff + 1], a
; ---- 1. CF_LOAD [off u16], until the image is in (armed job or `cart probe`)
.load
    ld hl, NbLoad
    ld b, 4
    call NbStart
    ld a, [vNbOff]
    ldh [c], a
    ld a, [vNbOff + 1]
    ldh [c], a
    call NbSend
    jr z, .loaded
    cp 3                            ; DENIED: a cart-flash ESP32 with nothing armed: boot now
    jp z, NbOut0
    jr .probe                       ; UNSUPPORTED (stock firmware) / error: the GET path
.loaded
    call NbBody
    jr z, .probe
    ld a, [vNbOff + 1]              ; more to come?
    ld b, a
    ld a, [vNbTot + 1]
    cp b
    jr nz, .load
    ld a, [vNbOff]
    ld b, a
    ld a, [vNbTot]
    cp b
    jr nz, .load
    jr NbRun
; ---- 2. IDENTIFY + GET cp/probe.bin (one answer, <= 4086 bytes)
.probe
    xor a
    ld [vNbOff], a
    ld [vNbOff + 1], a
    ld hl, NbIdent
    ld b, NbIdentEnd - NbIdent
    call NbStart
    call NbSend
    jr nz, NbRetry
    ld hl, NbGet
    ld b, NbGetEnd - NbGet
    call NbStart
    call NbSend
    jr nz, NbRetry
    call NbBody
    jr z, NbOut0
NbRun:
    ld hl, NB_ORG                   ; "SGWX"
    ld a, [hli]
    cp "S"
    jr nz, NbOut0
    ld a, [hli]
    cp "G"
    jr nz, NbOut0
    ld a, [hli]
    cp "W"
    jr nz, NbOut0
    ld a, [hli]
    cp "X"
    jr nz, NbOut0
    call NB_ORG + 4
; a = status: DENIED (3) / NOT_FOUND (4) = the gateway is not the owner's receiver: stop now.
; Anything else (OFFLINE, TIMEOUT, NOT_IDENTIFIED, RATE_LIMITED, window error $FF): retry.
NbRetry:
    cp 3
    jr z, NbOut0
    cp 4
    jr z, NbOut0
    ld a, [vNbTry]
    dec a
    ld [vNbTry], a
    jr z, NbOut0
    ld a, 3
    ldh [NB_CMD], a                 ; ABORT anything in flight
    ld d, 2                         ; ~0.9 s
.d  ld bc, 0
.di dec bc
    ld a, b
    or c
    jr nz, .di
    dec d
    jr nz, .d
    jp NbAgain
NbOut0:
    ld a, 3
    ldh [NB_CMD], a                 ; ABORT (if anything is in flight)
    xor a
    ldh [NB_CMD], a                 ; CLR_ERR
    ldh [NB_ID], a                  ; lock
    ret

; the answer's body -> $C000 + off; off += body_len; vNbTot = total_len. z = empty / too big
NbBody:
    ld a, 4
    ldh [NB_PTRL], a
    ldh a, [c]
    ld e, a                         ; body_len
    ldh a, [c]
    ld d, a
    ldh a, [c]
    ld [vNbTot], a                  ; total_len (low 16 bits)
    ldh a, [c]
    ld [vNbTot + 1], a
    ldh a, [c]
    ld b, a
    ldh a, [c]
    or b
    jr nz, .bad                     ; >= 64 KiB
    ld a, [vNbTot + 1]
    cp HIGH(NB_MAX) + 1
    jr nc, .bad
    ld a, d
    or e
    ret z
    ld a, [vNbOff]
    ld l, a
    ld a, [vNbOff + 1]
    add HIGH(NB_ORG)
    ld h, a
.copy
    ldh a, [c]
    ld [hli], a
    ld a, h
    cp HIGH(NB_ORG + NB_MAX)
    jr nc, .bad
    dec de
    ld a, d
    or e
    jr nz, .copy
    ld a, l
    ld [vNbOff], a
    ld a, h
    sub HIGH(NB_ORG)
    ld [vNbOff + 1], a
    or 1                            ; nz
    ret
.bad
    xor a                           ; z
    ret

; SEND, wait for the answer (~12 s). z = status OK (c = LOW(NB_DATA), PTR = 2)
NbSend:
    xor a
    ldh [NB_CMD], a                 ; CLR_ERR
    inc a
    ldh [NB_CMD], a                 ; SEND
    ld d, 12
.w  ld bc, 0
.wi ldh a, [NB_STAT]
    bit 1, a
    jr nz, .ready
    bit 2, a
    jr nz, .err
    dec bc
    ld a, b
    or c
    jr nz, .wi
    dec d
    jr nz, .w
.err
    ld a, $FF                       ; window error / no answer
    or a
    ret
.ready
    xor a
    ldh [NB_PTRL], a
    ldh [NB_PTRH], a
    ld c, LOW(NB_DATA)
    ldh a, [c]                      ; ver
    ldh a, [c]                      ; status
    or a
    ret

NbStart:
    xor a
    ldh [NB_PTRL], a
    ldh [NB_PTRH], a
    ld c, LOW(NB_DATA)
NbOut:
    ld a, [hli]
    ldh [c], a
    dec b
    jr nz, NbOut
    ret

NbKey:
    db "SGNW"
NbLoad:
    db 1, $20, 0, 0
NbIdent:
    db 1, 2, 0, 0, "SHGCARTPROBE0001", 1, 0, "NETBOOT", 0
NbIdentEnd:
NbGet:
    db 1, 3, 0, 0, "cp/probe.bin", 0
NbGetEnd:
