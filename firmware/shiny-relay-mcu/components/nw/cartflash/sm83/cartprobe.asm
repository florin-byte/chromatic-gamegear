; cartprobe.asm: READ-ONLY cartridge probe + backup dump (lane cartflash1).
; A WRAM program ($C000), loaded by the netboot stub of the TEST-ONLY boot ROM
; (cf_netboot.inc; make_sm83.sh; never flashed: SRAM load only), which fetches it
; with GET cp/probe.bin from the local receiver. Works with the stock ESP32 0.17.0
; network window service: only the v1 ops IDENTIFY / GET / POST are used, so the
; ESP32 forwards everything to the gateway URL set with `nw url`, which for this
; test must be the owner's Mac running cartflash/host/cart_probe_rx.py.
;
; What it does, from WRAM (the boot ROM only carries it):
;   unlock 'SGNW', wait up to ~60 s for SVC, IDENTIFY "SHGCARTPROBE0001" (retried ~1 s
;   apart for ~60 s while the ESP32 is offline / timing out),
;   GET cp/hello: the body must be "CARTPROBE-LOCAL1" (only the local receiver
;     says that; the Shiny Gen gateway answers 404) or NOTHING is read,
;   read the header 0x100-0x14F, POST cp/h -> the receiver answers what to dump
;     (go, mapper, ROM banks, RAM banks, options),
;   dump every ROM bank in 1 KiB chunks (each chunk read twice and compared,
;     up to 4 tries), POST cp/r, a 16-bit sum per chunk,
;   optionally fingerprint banks past the declared size (cp/x),
;   dump cart RAM (RAM enable 0x0A at 0x0000, reads only, disabled after),
;   [CP_FLASHID build only, owner approval: autoselect + CFI, cp/i],
;   POST cp/e: counts, the cart's global checksum as computed here, the header
;     checksum as computed here,
;   lock the window, map ROM bank 1 again and return: the boot ROM boots the
;   cart as usual.
;
; Cart bus writes in the read-only build: MBC register writes only (ROM bank
; number to 0x2000/0x3000 (MBC1: 0x4000 upper bits), RAM enable 0x0A / 0x00 to
; 0x0000, RAM bank to 0x4000), the same writes every game on the cart makes.
; No flash command sequence is ever written (no 0x555/0x2AA/0xAAA writes).
;
; Bank 0 under the boot ROM: while the boot ROM is mapped, CPU 0x0000-0x00FF
; and 0x0200-0x08FF read the boot ROM. MBC5 maps bank 0 at 0x4000, so the
; probe reads all of bank 0 there. Other mappers: bank 0 is read at 0x0000
; except chunks 0-2 (0x0000-0x0BFF), which need the boot ROM unmapped (FF50);
; that happens only with CP_OPT_UNMAP, at the very end, and the console then
; halts (power it off) instead of booting the cart.

DEF NW_ID   EQU $FF58
DEF NW_CMD  EQU $FF5A
DEF NW_STAT EQU $FF5B
DEF NW_PTRL EQU $FF5C
DEF NW_PTRH EQU $FF5D
DEF NW_DATA EQU $FF5E

DEF CP_BUF  EQU $D000
DEF CP_KIND_ROM_C  EQU "R"
DEF CP_KIND_RAM_C  EQU "S"
DEF CP_KIND_HOLE_C EQU "Z"
DEF CP_VERSION_C   EQU 1
DEF CpIdBase EQU $4000
DEF CP_CHUNK EQU 1024

; variables (cleared at start). CF00..: the cp/h answer, in its order
DEF vGo       EQU $CF00
DEF vMbc      EQU $CF01
DEF vRomBanks EQU $CF02   ; u16
DEF vRamBanks EQU $CF04
DEF vRamKb    EQU $CF05
DEF vOpts     EQU $CF06
DEF vExtBanks EQU $CF07   ; u16
; CF10..: the cp/r content header, in its order
DEF vKind     EQU $CF10
DEF vBank     EQU $CF11   ; u16
DEF vOff      EQU $CF13   ; u16
DEF vLen      EQU $CF15   ; u16
DEF vSum      EQU $CF17   ; u16
DEF vRr       EQU $CF19
; CF20..: the cp/e content, in its order
DEF vResult   EQU $CF20
DEF vChunks   EQU $CF21   ; u16
DEF vRereads  EQU $CF23   ; u16
DEF vBad      EQU $CF25   ; u16
DEF vRetries  EQU $CF27   ; u16
DEF vGsum     EQU $CF29   ; u16
DEF vHsum     EQU $CF2B
DEF vBank0    EQU $CF2C   ; 0 bank 0 fully read (MBC5), 1 chunks 0-2 pending, 2 missing, 3 read after unmap
; work
DEF vSrc      EQU $CF30   ; u16
DEF vTry      EQU $CF32
DEF vRrLeft   EQU $CF33
DEF vExtCnt   EQU $CF36
DEF vExtFirst EQU $CF37   ; u16
DEF vVariant  EQU $CF39
DEF vResetOk  EQU $CF3A
DEF vHdr      EQU $CE00   ; 80 bytes: the header 0x100-0x14F
; pipelining: chunk n+1 is read while chunk n's request is on the UART
DEF vCur      EQU $CF3B   ; buffer the next chunk goes into (0: $D000, 1: $D400)
DEF vInfl     EQU $CF3C   ; 1: a chunk request is in flight
DEF vInfBuf   EQU $CF3D   ; ... from this buffer
DEF vBuildBuf EQU $CF3E   ; CpBuildChunk builds from this buffer
DEF vHdr0     EQU $CF40   ; the chunk header (10 bytes, as vKind..vRr) of buffer 0
DEF vHdr1     EQU $CF50   ; ... of buffer 1

MACRO INC16
    ld hl, \1
    inc [hl]
    jr nz, :+
    inc hl
    inc [hl]
:
ENDM

SECTION "CartProbe", ROM0[$0]
LOAD "CartProbeWram", WRAM0[$C000]
    db "SGWX"                       ; netboot magic; the stub calls $C004
CpMain:
    ld hl, CpKey
    ld c, LOW(NW_ID)
    ld b, 4
    call CpOutC
    ldh a, [NW_ID]
    cp $4E
    ret nz                          ; no network window: boot as stock
    ld hl, $CF00
    ld b, $40
    xor a
:   ld [hli], a
    dec b
    jr nz, :-
    ld e, $08                       ; SVC (not ONLINE: over USB the ESP32 needs no Wi-Fi)
    call CpWaitStat
    jp nz, CpLockOut
    ld a, 60                        ; IDENTIFY + GET cp/hello, ~1 s apart while the answer is
    ld [vTry], a                    ; OFFLINE / TIMEOUT / a window error (Wi-Fi still joining)
.hello
    ld hl, CpIdent
    ld b, CpIdentEnd - CpIdent
    call CpStart
    call CpTransact
    jr nz, .hretry
    ld hl, CpHelloReq               ; GET cp/hello
    ld b, CpHelloReqEnd - CpHelloReq
    call CpStart
    call CpTransact
    jr z, .hok
.hretry
    cp 3                            ; DENIED / NOT_FOUND: not the owner's receiver, stop
    jp z, CpLockOut
    cp 4
    jp z, CpLockOut
    call CpDelay
    call CpDelay
    call CpDelay
    call CpDelay
    ld a, [vTry]
    dec a
    ld [vTry], a
    jp z, CpLockOut
    jr .hello
.hok
    ld a, 10                        ; body at RX 10
    ldh [NW_PTRL], a
    xor a
    ldh [NW_PTRH], a
    ld hl, CpMagic
    ld b, 16
:   ldh a, [c]
    cp [hl]
    jp nz, CpLockOut                ; not the local receiver: read nothing
    inc hl
    dec b
    jr nz, :-
    ; the header (0x100-0x1FF is the cart even while the boot ROM is mapped)
    ld hl, $0100
    ld de, vHdr
    ld b, 80
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    ld hl, CpBuildHdr
    call CpCall
    jp nz, CpLockOut
    ld a, 10
    ldh [NW_PTRL], a
    xor a
    ldh [NW_PTRH], a
    ld hl, vGo
    ld b, 9
:   ldh a, [c]
    ld [hli], a
    dec b
    jr nz, :-
    ld a, [vGo]
    cp 1
    jp nz, CpLockOut
    ; header checksum, as the boot ROM computes it
    ld hl, vHdr + $34
    ld b, $4D - $34
    xor a
:   sub [hl]
    dec a
    inc hl
    dec b
    jr nz, :-
    ld [vHsum], a

; ---- ROM banks 0 .. rom_banks-1
    ld a, CP_KIND_ROM_C
    ld [vKind], a
    xor a
    ld [vBank], a
    ld [vBank + 1], a
.bank
    ld a, [vBank]
    ld e, a
    ld a, [vBank + 1]
    ld d, a
    ld a, [vRomBanks]
    cp e
    jr nz, .bank_go
    ld a, [vRomBanks + 1]
    cp d
    jp z, .rom_done
.bank_go
    ld hl, $4000                    ; base
    ld bc, 0                        ; first offset
    ld a, d
    or e
    jr nz, .mapped                  ; bank >= 1: through 0x4000
    ld a, [vMbc]
    cp 5
    jr z, .mapped                   ; MBC5: bank 0 at 0x4000 too
    ld hl, $0000                    ; bank 0 at 0x0000: chunks 0-2 lie under the boot ROM
    ld bc, 3 * CP_CHUNK
    ld a, 1
    ld [vBank0], a
    jr .chunks
.mapped
    push hl
    push bc
    call CpMap
    pop bc
    pop hl
.chunks
    ; off = bc, src = base + off
.chunk
    ld a, b
    cp $40
    jr nc, .next_bank
    ld a, c
    ld [vOff], a
    ld a, b
    ld [vOff + 1], a
    push hl
    add hl, bc
    ld a, l
    ld [vSrc], a
    ld a, h
    ld [vSrc + 1], a
    push bc
    call CpReadPost
    pop bc
    pop hl
    jp nz, CpFail
    ld a, b
    add HIGH(CP_CHUNK)
    ld b, a
    jr .chunk
.next_bank
    INC16 vBank
    jp .bank
.rom_done
    call CpFlush                    ; the last chunk answered before anything synchronous
    jp nz, CpFail

; ---- fingerprints past the declared size (CP_OPT_EXTENDED)
    ld a, [vOpts]
    and 1
    jp z, .ext_done
    ld a, [vMbc]
    or a
    jp z, .ext_done
.ext_group
    ld a, [vBank]                   ; vBank = rom_banks here, then steps on
    ld [vExtFirst], a
    ld a, [vBank + 1]
    ld [vExtFirst + 1], a
    xor a
    ld [vExtCnt], a
    ld hl, CP_BUF
.ext_bank
    ld a, [vBank]
    ld e, a
    ld a, [vBank + 1]
    ld d, a
    ld a, [vExtBanks]
    cp e
    jr nz, .ext_go
    ld a, [vExtBanks + 1]
    cp d
    jr z, .ext_flush
.ext_go
    push hl
    call CpMap
    ld hl, $4000                    ; sum the bank
    ld de, 0
    ld bc, $4000
:   ld a, [hli]
    add e
    ld e, a
    jr nc, @ + 3
    inc d
    dec bc
    ld a, b
    or c
    jr nz, :-
    pop hl
    ld a, e
    ld [hli], a
    ld a, d
    ld [hli], a
    ld de, $4000                    ; and its first 16 bytes
    ld b, 16
:   ld a, [de]
    ld [hli], a
    inc de
    dec b
    jr nz, :-
    push hl
    INC16 vBank
    pop hl
    ld a, [vExtCnt]
    inc a
    ld [vExtCnt], a
    cp 32
    jr nz, .ext_bank
.ext_flush
    ld a, [vExtCnt]
    or a
    jr z, .ext_done
    ld hl, CpBuildExt
    call CpCall
    jp nz, CpFail
    ld a, [vExtCnt]
    cp 32
    jr z, .ext_group
.ext_done

; ---- cart RAM (reads only)
    ld a, [vRamBanks]
    or a
    jp z, .ram_done
    ld a, CP_KIND_RAM_C
    ld [vKind], a
    xor a
    ld [vBank], a
    ld [vBank + 1], a
    call CpRamDiag                  ; time vs address: what makes a save RAM read float
    jp nz, CpFail
.ram_bank
    ld a, [vBank]
    ld [$4000], a                   ; MBC1/3/5: RAM bank
    ld bc, 0
.ram_chunk
    ld a, [vRamKb]
    add a
    add a                           ; KiB -> high byte of the bank size
    cp b
    jr z, .ram_next
    ld a, c
    ld [vOff], a
    ld a, b
    ld [vOff + 1], a
    ld hl, $A000
    add hl, bc
    ld a, l
    ld [vSrc], a
    ld a, h
    ld [vSrc + 1], a
    push bc
    call CpReadPost
    pop bc
    jr nz, .ram_fail
    ld a, b
    add HIGH(CP_CHUNK)
    ld b, a
    jr .ram_chunk
.ram_next
    ld a, [vBank]
    inc a
    ld [vBank], a
    ld hl, vRamBanks
    cp [hl]
    jr nz, .ram_bank
    call CpRamOff
    jr .ram_done
.ram_fail
    call CpRamOff
    jp CpFail
.ram_done
    call CpFlush                    ; the last chunk answered before anything synchronous
    jp nz, CpFail

IF DEF(CP_FLASHID)
    ld a, [vOpts]
    and 2
    call nz, CpFlashId
    jp nz, CpFail
ENDC

; ---- bank 0 under the boot ROM (non-MBC5, CP_OPT_UNMAP): no normal boot after this
    ld a, [vBank0]
    cp 1
    jr nz, .end
    ld a, [vOpts]
    and 4
    jr nz, .unmap
    ld a, 2                         ; left out: the receiver marks the backup incomplete
    ld [vBank0], a
    jr .end
.unmap
    ld a, $11
    ldh [$FF50], a                   ; boot ROM off (this code runs from WRAM)
    ld a, CP_KIND_HOLE_C
    ld [vKind], a
    xor a
    ld [vBank], a
    ld [vBank + 1], a
    ld bc, 0
.hole
    ld a, c
    ld [vOff], a
    ld [vSrc], a
    ld a, b
    ld [vOff + 1], a
    ld [vSrc + 1], a
    push bc
    call CpReadPost
    pop bc
    jp nz, CpFail
    ld a, b
    add HIGH(CP_CHUNK)
    ld b, a
    cp HIGH(3 * CP_CHUNK)
    jr nz, .hole
    ld a, 3
    ld [vBank0], a
.end
    call CpFlush                    ; (the bank-0 holes after unmap are pipelined too)
    jp nz, CpFail
    ; the global checksum leaves out its own two bytes (bank 0 complete only)
    ld a, [vBank0]
    cp 2
    jr z, .report
    ld hl, vGsum
    ld a, [vHdr + $4E]
    ld b, a
    ld a, [hl]
    sub b
    ld [hli], a
    jr nc, :+
    dec [hl]
:   dec hl
    ld a, [vHdr + $4F]
    ld b, a
    ld a, [hl]
    sub b
    ld [hli], a
    jr nc, .report
    dec [hl]
.report
    ld hl, CpBuildEnd
    call CpCall
    ld a, [vBank0]
    cp 3
    jr nz, CpLockOut
    xor a
    ldh [NW_ID], a
:   halt                            ; boot ROM gone: power the console off
    jr :-

CpFail:
    call CpFlush                    ; (result ignored: we are failing anyway)
    ld a, 1
    ld [vResult], a
    ld hl, CpBuildEnd
    call CpCall
    ; fall through
CpLockOut:
    xor a
    ldh [NW_ID], a                  ; lock the window
    ld a, [vMbc]
    or a
    ret z
    ld a, 1                         ; ROM bank 1 again, as at power-on
    ld [$2000], a
    xor a
    ld [$3000], a
    ld [$4000], a
    ld a, [vMbc]
    cp 1
    ret nz
    xor a
    ld [$6000], a
    ret

; EVERY CART WRITE of this program is aimed at 1000-1FFF or above (2026-10-11): it runs with
; the boot ROM mapped, and gb.v keeps CPU writes to 0000-00FF and 0200-08FF off the cart bus
; then (ext_bus_a15 = a15 | sel_boot_rom). 0A@0000, 55@0555, 55@02AA and F0@0000 never reached
; the cart in the first hardware runs. 1000 is the same MBC RAM-enable register, and the
; flash decodes A11-A0 for commands, so 1AAA / 1555 / 12AA are the same unlock addresses.
; vKind = save RAM: (MBC1 RAM mode,) RAM enable 0x0A, RAM bank = vBank. Reads only follow.
CpRamSelIf:
    ld a, [vKind]
    cp CP_KIND_RAM_C
    ret nz
CpRamSel:
    ld a, [vMbc]
    cp 1
    jr nz, :+
    ld a, 1
    ld [$6000], a
:   ld a, $0A
    ld [$1000], a
    ld a, [vBank]
    ld [$4000], a
    ret

; POST cp/d: 6 x 16 bytes of RAM bank 0 (the 2026-10-10 run read the first KiB of each bank
; stable and the rest floating): [A400 right after enable+bank, A400 again, A000 after
; ~0.5 s with no cart write, A400 after another ~0.5 s, A000 right after enable+bank
; again, A400 right after that]
CpRamDiag:
    call CpRamSel
    ld de, CP_BUF
    ld hl, $A400
    call .rd
    ld hl, $A400
    call .rd
    call .wait
    ld hl, $A000
    call .rd
    call .wait
    ld hl, $A400
    call .rd
    push de
    call CpRamSel
    pop de
    ld hl, $A000
    call .rd
    ld hl, $A400
    call .rd
    ld hl, CpBuildDiag
    jp CpCall
.rd
    ld b, 16
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    ret
.wait
    push de
    call CpDelay
    call CpDelay
    pop de
    ret

CpBuildDiag:
    ld hl, CpPostD
    ld b, CpPostDEnd - CpPostD
    call CpStart
    ld hl, CP_BUF
    ld b, 96
    jp CpOutC

CpRamOff:
    xor a
    ld [$1000], a                   ; RAM disabled
    ld [$4000], a
    ld a, [vMbc]
    cp 1
    ret nz
    xor a
    ld [$6000], a
    ret

; de = ROM bank: map it at 0x4000 (vMbc: 0 none, 1 MBC1, 3 MBC3, 5 MBC5)
CpMap:
    ld a, [vMbc]
    cp 5
    jr z, .m5
    cp 1
    jr z, .m1
    cp 3
    ret nz
    ld a, e
    ld [$2000], a
    ret
.m5
    ld a, e
    ld [$2000], a
    ld a, d
    and 1
    ld [$3000], a
    ret
.m1
    ld a, e
    and $1F
    ld [$2000], a
    ld a, e
    swap a
    rrca
    and 3
    ld [$4000], a
    ret

; read CP_CHUNK bytes at [vSrc] twice (compare) into buffer vCur, sum, then wait for the
; previous chunk's answer (CpFlush) and SEND this one without waiting: the next chunk is
; read while this request crosses the UART. z = ok (this chunk sent; it is checked later)
CpReadPost:
    ld a, LOW(CP_CHUNK)
    ld [vLen], a
    ld a, HIGH(CP_CHUNK)
    ld [vLen + 1], a
    ld a, 4
    ld [vRrLeft], a
    xor a
    ld [vRr], a
.read
    call CpRamSelIf                 ; save RAM: enable + bank again right before each pass
    call .src
    call .buf
:   ld a, [hli]
    ld [de], a
    inc de
    dec bc
    ld a, b
    or c
    jr nz, :-
    call CpRamSelIf
    call .src
    call .buf
:   ld a, [de]
    cp [hl]
    jr nz, .diff
    inc hl
    inc de
    dec bc
    ld a, b
    or c
    jr nz, :-
    jr .sum
.diff
    ld a, [vRr]
    inc a
    ld [vRr], a
    INC16 vRereads
    ld a, [vRrLeft]
    dec a
    ld [vRrLeft], a
    jr nz, .read
    ld a, $FF                       ; never read the same twice: sent, flagged
    ld [vRr], a
    INC16 vBad
.sum
    ld a, [vCur]
    call CpBufHL
    ld de, 0
    ld bc, CP_CHUNK
:   ld a, [hli]
    add e
    ld e, a
    jr nc, @ + 3
    inc d
    dec bc
    ld a, b
    or c
    jr nz, :-
    ld a, e
    ld [vSum], a
    ld a, d
    ld [vSum + 1], a
    ld a, [vKind]
    cp CP_KIND_RAM_C
    jr z, .post
    ld hl, vGsum                    ; ROM: the cart's global checksum
    ld a, [hl]
    add e
    ld [hli], a
    ld a, [hl]
    adc d
    ld [hl], a
.post
    ld a, [vCur]                    ; this chunk's header goes with its buffer
    call CpHdrHL
    ld d, h
    ld e, l
    ld hl, vKind
    ld b, 10
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    call CpFlush                    ; the previous chunk: answered OK (or resent)
    ret nz
    ld a, [vCur]
    ld [vBuildBuf], a
    call CpBuildChunk
    call CpSend
    ld a, 1
    ld [vInfl], a
    ld a, [vCur]
    ld [vInfBuf], a
    xor 1
    ld [vCur], a
    INC16 vChunks
    xor a
    ret
.src
    ld hl, vSrc
    ld a, [hli]
    ld h, [hl]
    ld l, a
    ret
.buf                                ; de = buffer vCur, bc = CP_CHUNK (hl kept)
    push hl
    ld a, [vCur]
    call CpBufHL
    ld d, h
    ld e, l
    pop hl
    ld bc, CP_CHUNK
    ret

; a = buffer index -> hl = its data / its chunk header
CpBufHL:
    ld hl, $D000
    or a
    ret z
    ld h, $D4
    ret
CpHdrHL:
    ld hl, vHdr0
    or a
    ret z
    ld hl, vHdr1
    ret

; the chunk request in flight (if any): wait for its answer; not OK -> resend it from its
; buffer with the usual retries. z = ok
CpFlush:
    ld a, [vInfl]
    or a
    ret z
    xor a
    ld [vInfl], a
    call CpWait
    ret z
    ld a, [vInfBuf]
    ld [vBuildBuf], a
    ld hl, CpBuildChunk
    jp CpCall

; ---- request builders (fill TX, PTR = length)
CpBuildHdr:
    ld hl, CpPostH
    ld b, CpPostHEnd - CpPostH
    call CpStart
IF DEF(CP_FLASHID)
    ld a, CP_VERSION_C
    ldh [c], a
    ld a, 1                         ; flags: this build can run the flash-ID step
ELSE
    ld a, CP_VERSION_C
    ldh [c], a
    xor a
ENDC
    ldh [c], a
    ld hl, vHdr
    ld b, 80
    jp CpOutC

CpBuildChunk:
    ld hl, CpPostR
    ld b, CpPostREnd - CpPostR
    call CpStart
    ld a, [vBuildBuf]
    call CpHdrHL
    ld b, 10
    call CpOutC
    ld a, [vBuildBuf]
    call CpBufHL
    ld de, CP_CHUNK
:   ld a, [hli]
    ldh [c], a
    dec de
    ld a, d
    or e
    jr nz, :-
    ret

CpBuildExt:
    ld hl, CpPostX
    ld b, CpPostXEnd - CpPostX
    call CpStart
    ld hl, vExtFirst
    ld b, 2
    call CpOutC
    ld a, [vExtCnt]
    ldh [c], a
    ld b, a                         ; count x 18 bytes
    ld hl, CP_BUF
.e  push bc
    ld b, 18
    call CpOutC
    pop bc
    dec b
    jr nz, .e
    ret

CpBuildEnd:
    ld hl, CpPostE
    ld b, CpPostEEnd - CpPostE
    call CpStart
    ld hl, vResult
    ld b, 13
    jp CpOutC

; hl = builder. Builds and sends, up to 60 tries (~0.25 s apart). z = OK
CpCall:
    ld a, 60
    ld [vTry], a
.again
    push hl
    call .jphl
    call CpTransact
    pop hl
    ret z
    push hl
    INC16 vRetries
    call CpDelay
    pop hl
    ld a, [vTry]
    dec a
    ld [vTry], a
    jr nz, .again
    inc a                           ; nz
    ret
.jphl
    jp hl

; TX is filled and PTR = length: SEND, wait for the answer (~12 s at most).
; z and a = 0: status OK. nz: a = status, or $FF for a window error / timeout.
CpTransact:
    call CpSend
    jr CpWait
CpSend:
    xor a
    ldh [NW_CMD], a                 ; CLR_ERR
    inc a
    ldh [NW_CMD], a                 ; SEND
    ret
; wait for the answer to the request sent; z and a = 0: status OK
CpWait:
    ld d, 10
.outer
    ld bc, 0
.inner
    ldh a, [NW_STAT]
    bit 1, a
    jr nz, .ready
    bit 2, a
    jr nz, .err
    dec bc
    ld a, b
    or c
    jr nz, .inner
    dec d
    jr nz, .outer
.err
    ld a, 3
    ldh [NW_CMD], a                 ; ABORT
    xor a
    ldh [NW_CMD], a                 ; CLR_ERR
    dec a                           ; $FF, nz
    ret
.ready
    xor a
    ldh [NW_PTRL], a
    ldh [NW_PTRH], a
    ld c, LOW(NW_DATA)
    ldh a, [c]                      ; ver
    ldh a, [c]                      ; status
    or a
    ret

CpDelay:
    ld bc, 16384
:   dec bc
    ld a, b
    or c
    jr nz, :-
    ret

; wait until (STAT & e) == e, ~9 s at most. z = yes
CpWaitStat:
    ld d, 70                        ; ~60 s
.outer
    ld bc, 0
.inner
    ldh a, [NW_STAT]
    and e
    cp e
    ret z
    dec bc
    ld a, b
    or c
    jr nz, .inner
    dec d
    jr nz, .outer
    inc d                           ; nz
    ret

; PTR = 0, then b bytes from [hl] to NW_DATA (c = LOW(NW_DATA) after)
CpStart:
    xor a
    ldh [NW_PTRL], a
    ldh [NW_PTRH], a
    ld c, LOW(NW_DATA)
; b bytes from [hl] to [$FF00+c]
CpOutC:
    ld a, [hli]
    ldh [c], a
    dec b
    jr nz, CpOutC
    ret

IF DEF(CP_FLASHID)
; ---- step 2, OWNER APPROVAL: flash autoselect ID + CFI. Writes command bytes to the
; cart (AA/55/90, 98, F0 at 0x555/0x2AA or 0xAAA/0x555): it cannot erase or program
; (that needs 80/A0 after a second unlock, never written), but the cart's mapper is
; unknown. Four variants: two unlock address pairs x D0/D1 straight or swapped. After
; each: reset (F0, then AA/55/F0), and the header must read back as before (else nz:
; stop; a power cycle returns any AMD-type chip to read mode). ModRetro carts (FlashGBX
; profiles, CART_FLASH_DESIGN.md) use variant 1 (0xAAA/0x555, straight data lines).
CpFlashId:
    xor a
    ld [vVariant], a
.variant
    ld de, 0                        ; bank 0 at 0x4000 (MBC5) = flash 0
    call CpMap
    ld a, [vVariant]
    call CpUnlock
    ld a, $90
    call CpCmdA1
    ld hl, CpIdBase
    ld a, [vMbc]
    cp 5
    jr z, :+
    ld hl, $0100                    ; other mappers: autoselect codes repeat per 256 bytes (most AMD chips)
:   push hl
    ld de, CP_BUF
    ld b, 16
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    call CpReset
    ld a, [vVariant]                ; CFI query: 98 at 0x55 (x8) / 0xAA (x16 in byte mode)
    and 1
    ld hl, $1055
    jr z, :+
    ld hl, $10AA
:   ld a, $98
    call CpSwapA
    ld [hl], a
    pop hl
    push hl
    ld de, CP_BUF + 16              ; 128 bytes from flash 0: x8 and x16-in-byte-mode layouts
    ld b, 128
:   ld a, [hli]
    ld [de], a
    inc de
    dec b
    jr nz, :-
    call CpReset
    pop hl
    call CpCheckHdr
    jr z, .ok
    ld a, [vVariant]                ; second reset: the full AA/55/F0 sequence
    call CpUnlock
    ld a, $F0
    call CpCmdA1
    call CpCheckHdr
.ok
    ld a, 0
    jr nz, :+
    inc a
:   ld [vResetOk], a
    ld hl, CpBuildId
    call CpCall
    ret nz
    ld a, [vResetOk]
    or a
    jr z, .stuck
    ld a, [vVariant]
    inc a
    ld [vVariant], a
    cp 4
    jp nz, .variant
    xor a
    ret
.stuck
    inc a                           ; nz: stop the probe here
    ret

CpReset:
    ld a, $F0
    ld [$1000], a                   ; F0 anywhere (also RAM disable on the MBC)
    ret

; the unlock pair for variant a (bit 0: AAA/555, bit 1: D0/D1 swapped)
CpUnlock:
    push af
    and 1
    jr nz, .aaa
    ld a, $AA
    call CpSwapA
    ld [$1555], a
    ld a, $55
    call CpSwapA
    ld [$12AA], a
    pop af
    ret
.aaa
    ld a, $AA
    call CpSwapA
    ld [$1AAA], a
    ld a, $55
    call CpSwapA
    ld [$1555], a
    pop af
    ret

; command a to the first unlock address of the variant
CpCmdA1:
    call CpSwapA
    ld b, a
    ld a, [vVariant]
    and 1
    ld a, b
    jr nz, :+
    ld [$1555], a
    ret
:   ld [$1AAA], a
    ret

; swap bits 0 and 1 of a when the variant says so
CpSwapA:
    push bc
    ld b, a
    ld a, [vVariant]
    and 2
    ld a, b
    jr z, .done
    and $FC
    bit 0, b
    jr z, :+
    or 2
:   bit 1, b
    jr z, .done
    or 1
.done
    pop bc
    ret

; z if 0x100-0x14F reads as the header saved at the start
CpCheckHdr:
    ld hl, $0100
    ld de, vHdr
    ld b, 80
:   ld a, [de]
    cp [hl]
    ret nz
    inc hl
    inc de
    dec b
    jr nz, :-
    ret

CpBuildId:
    ld hl, CpPostI
    ld b, CpPostIEnd - CpPostI
    call CpStart
    ld a, [vVariant]
    ldh [c], a
    ld hl, CP_BUF
    ld b, 16 + 128
    call CpOutC
    ld a, [vResetOk]
    ldh [c], a
    ret

CpPostI:
    db 1, 4, 0, 0, "cp/i", 0
CpPostIEnd:
ENDC

CpKey:
    db "SGNW"
CpIdent:
    db 1, 2, 0, 0, "SHGCARTPROBE0001"
    dw CP_VERSION_C
    db "CARTPROBE", 0
CpIdentEnd:
CpHelloReq:
    db 1, 3, 0, 0, "cp/hello", 0
CpHelloReqEnd:
CpMagic:
    db "CARTPROBE-LOCAL1"
CpPostH:
    db 1, 4, 0, 0, "cp/h", 0
CpPostHEnd:
CpPostR:
    db 1, 4, 0, 0, "cp/r", 0
CpPostREnd:
CpPostX:
    db 1, 4, 0, 0, "cp/x", 0
CpPostXEnd:
CpPostD:
    db 1, 4, 0, 0, "cp/d", 0
CpPostDEnd:
CpPostE:
    db 1, 4, 0, 0, "cp/e", 0
CpPostEEnd:
CpWramEnd:
ENDL


ASSERT CpWramEnd <= $CE00, "probe image overlaps its variables"
