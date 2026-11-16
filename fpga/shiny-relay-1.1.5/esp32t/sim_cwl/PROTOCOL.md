# CWL2: Chromatic Wi-Fi Link, version 2

Goal: unmodified GB/GBC games use the normal link port (`FF01` SB, `FF02` SC) and
play over Wi-Fi. The peer is another Chromatic or a desktop emulator speaking the
same packets (`tools/cwl_peer`). Both run the same protocol core
(`mcu/components/cwl/cwl_proto.c`).

## Model: the slave decides

A link-cable transfer swaps the master's SB with the slave's SB. Over a network
the master cannot clock bits it does not have, so:

1. **Master starts.** The master game writes SC = `0x81`. Its side **holds the
   transfer** (SC bit 7 stays set, no IRQ) and sends `REQ(seq, master_byte)`.
2. **Slave decides.** The peer offers the byte to its link port. If the port is
   armed (SC = `0x80`) the exchange happens there and then: the slave game gets
   `master_byte` and an IRQ, and the side replies `ACK(seq, slave_byte)` with the
   byte its port actually shifted out. If the port is not armed, the request
   waits and is offered again the moment the game arms.
3. **Master completes** on `ACK`: SB = `slave_byte`, IRQ.

The slave's port is the only authority on whether an exchange happened, so the
two games can never disagree, even when a game arms and then withdraws to become
master itself (Tetris DX does this every byte).

**Timeout.** After `master_timeout_ms` (default 250 in the core, 1500 on the
Chromatic: a modem-sleeping slave needs up to ~300 ms) the master sends `CANCEL`.
The slave answers `ACK` if the exchange already happened (the master takes it) or
`CANCELLED` (the master completes with `0xFF`, what real hardware reads with no
partner). With no live peer the master completes with `0xFF` at once
(cable unplugged).

**Restarted transfers.** A game may write SC = `0x81` again while its master
transfer is still held (Tetris DX does it every frame until the first byte of a
packet completes; on hardware that byte took 1 ms, so the rewrite never overlapped).
That rewrite continues the held transfer: the FPGA emits no second `MASTER`
event, and the core ignores `MASTER` while a request is in flight. Starting a new
request instead made the slave exchange twice and desynced Tetris DX right after
the 2PLAYER handshake.

**Write barrier.** A game that writes SB or SC while its master is held (other
than the restart above) expects that transfer to be over: on hardware it took
1 ms. The port parks the write and stalls the CPU (as a GDMA does), and the side
asks the peer at once (`CANCEL`, `Cwl_OnLocalMasterAbandon`): `ACK` completes the
master with the peer's byte and its IRQ, `CANCELLED` releases it silently (no
IRQ, SB untouched; `MASTER_DROP`); then the parked write lands and the CPU runs
on. One round trip of stall. Dropping the transfer at the write instead let the
peer exchange a byte this side never received (Super Mario Bros. Deluxe VS), and
completing it with `0xFF` raised the IRQ after the game's own SB write (Pokemon
Gold/Silver read its own `0x02` as the partner's, both sides became master).
**Stop then restart.** `SC = 0x01` (stop, internal clock) under a held master is
absorbed (FPGA 18.19, cwl_peer `--fpga`): no stall, no event, the transfer stays
held, and the `0x81` that normally follows continues it. On hardware that byte
finished 1 ms after it started, so the game is only stopping a finished transfer
to start the next (Pokemon Gold/Silver `LinkTransfer` / `Serial_ExchangeByte` write
`0x01, 0x81` every frame). Treating it as a barrier write stalled the master a
Wi-Fi round trip whenever a byte was still in flight at the next frame; under real
Wi-Fi jitter and loss the master game fell frames behind the slave, and Gen 2's
frame-timed handshakes (each byte sent for 10 frames) slid out of step: after the
save the slave entered the trade room while the master waited at "Please wait"
forever (on hardware with the Chromatic as master, and in Mac pairs with
`--jitter-ms 15 --loss-pct 5`: 4/4 stuck, 0/4 with the absorb). Writes that change
the clock source (`0x00`, `0x80`) or SB still park and stall.

An `ABANDON` for a master the side never heard of (its `MASTER` event was lost
on the UART), while no reply is on its way to the port (none sent in the last
10 ms), is answered with `MASTER_DROP` at once: no request went out, so nobody
exchanged. Ignoring it left the CPU stalled until the FPGA's ~4 s safety timeout,
which completed the master with `0xFF` and an IRQ after the parked write had
landed (see the event queue below).

**Fast clock.** A master started on the CGB fast clock (SC bit 1, 8 bits in
~30 us on hardware) stalls the CPU until the reply: games using it wait only a
short delay loop (SMB Deluxe sends `FE`, waits ~1.7 ms, and clears the reply a
frame later). Normal-clock masters keep the CPU running while held (Tetris DX,
Pokemon poll for the IRQ). If both sides hold stalled masters, neither can ever
arm: a refused offer reports the collision and both masters complete with `0xFF`
after one round trip (what two masters clocking at once read on hardware)
instead of freezing for the master timeout.

**Standing offers** (FPGA 18.20 + ESP32 0.13.14, CONFIG bit 3; cwl_peer `--fpga`).
An offer the port cannot take (not armed) stays at the port and is exchanged in
the very cycle the game arms (`SLAVE_DONE` instead of `ARMED`), until the side
withdraws it (cmd 6 `SLAVE_WITHDRAW`: `SLAVE_NAK` if it still waited, nothing if it
was exchanged, its `SLAVE_DONE` being the answer). The side withdraws on the
peer's `CANCEL` (then answers `CANCELLED` or `ACK` by the port's verdict), on a
newer request, on a peer restart and when the peer goes away. Pokemon Gold/Silver
search for a partner by arming for ~20 us per frame and becoming master again;
an offer that has to cross the UART after an `ARMED` never finds them armed, so two
sides that start searching together never connected (they only met when one side
entered the search, which first arms for 3 frames, while the other was already in
it). With standing offers the next brief arm takes the peer's request. A standing
offer while the port holds a fast-clock master with the CPU stalled reports
event 7 once (collision) and keeps standing. Older FPGA builds ignore bit 3 and
answer offers at once; the core then behaves as before.

**Offers without ARMED.** `ARMED` is only a hint. A pending peer request is also
offered to the port every 20 ms without it (the port answers `SLAVE_NAK` if not
armed), so a port that armed before the link came up, or an `ARMED` lost on the
UART, cannot stall the peer. An offer the port never answers counts as refused
after 500 ms.

CWL1 (the master consumed an advisory ARM) desynced Tetris DX: an ARM could be
consumed after the slave had withdrawn it.

## UDP packets (port 47820)

All packets: `'C' 'W' 0x02 type seq byte` (6 bytes).

| type | name      | direction | meaning |
|------|-----------|-----------|---------|
| 1 | REQ       | master -> slave | exchange `byte` (master's SB); `seq` = master's counter |
| 2 | ACK       | slave -> master | exchanged; `byte` = slave's SB |
| 3 | CANCEL    | master -> slave | master wants to give up on `seq` |
| 4 | CANCELLED | slave -> master | `seq` was NOT exchanged |
| 5 | HELLO     | both | keepalive / discovery, every 500 ms; `seq`,`byte` = 16-bit session id (random per start, 0 = none) |
| 6 | HELLO_PROBE | both | a HELLO that asks for an immediate HELLO back; sent every 100 ms by a side whose radio just woke, until it hears the peer |

Reliability: `REQ` and `CANCEL` are re-sent every 20 ms until answered. The
slave remembers its last `ACK` and `CANCELLED` and repeats them for duplicate
requests; requests older than the newest seen are ignored. A request newer than any seen drops the remembered `ACK` / `CANCELLED` (the master has finished every older seq): after the 8-bit seq wraps, a `CANCELLED` remembered from 256 requests earlier must not answer a new request with the same number. A HELLO with a new
session id means the peer restarted: the slave forgets the peer's sequence
numbers (otherwise its new requests would be ignored as old, or answered with a
stale `ACK`). A peer request still waiting for an unarmed port is dropped once
the peer has not been heard for 2 s (its master gave up long ago); a resend of
it is then ignored as old, and its `CANCEL` gets `CANCELLED`.

## Power (ESP32)

Wi-Fi runs flat out only while the link is in use. "In use" = a link-port event
from the FPGA (ARMED, DISARM, MASTER, SLAVE_DONE; not SLAVE_NAK, which only
answers our own offer probe) or an exchange packet from the peer (REQ, ACK,
CANCEL, CANCELLED; not HELLO) in the last `active` seconds.

| level   | when | Wi-Fi | MCU |
|---------|------|-------|-----|
| full    | in use and the partner answers (or a wake is waiting for it, or the first 10 s of joining) | on, no power save | awake |
| low     | in use with no partner (a game polling the port), or within `standby` s of the last use, or the port armed as slave within `armed` s | on, modem sleep (listen interval 3 beacons, ~300 ms) | awake |
| dormant | otherwise | **off** | light sleep as stock firmware (100 ms after the last FPGA packet) |
| off     | the OSD switch (SYSTEM > WI-FI LINK) is off | off | stock |

Defaults `active` 20 s, `standby` 60 s, `armed` 300 s (`cwl power`). The radio
is never powered down while an exchange is in progress.

**Waking.** While dormant the ESP32 sends `CONFIG` with bit 1 set, and the FPGA
puts the stock wake preamble (20 x `0x00`, ~1.7 ms) in front of each link event,
so the first event wakes the MCU from light sleep and still arrives intact. The
ESP32 then starts Wi-Fi (known channel, the last DHCP address reused for up to an
hour, gratuitous ARPs) and **holds** a `MASTER` for the peer for up to
`3600 - master_timeout` ms (2 s with the 1500 ms default) instead of reading
`0xFF` at once; meanwhile it sends `HELLO_PROBE`s. Measured on hardware: the event
is processed 7 ms after the FPGA sent it, Wi-Fi associated with an address after
80-110 ms, the partner (a Mac) answered after 0.6-0.9 s (the Mac's own ARP
recovery), and the held first byte completed with the partner's byte.

A dormant side cannot hear its partner: a game that waits armed as slave for
longer than `armed` s, with no other link activity, needs its own side to use the
port again (or a re-arm) before a partner's request gets through.

## Chromatic internals

`FPGA <-> ESP32` over the existing packet UART (115200, header `0x8F`, CRC-8 J1850).

FPGA -> ESP32, TX channel 10 (address `0x0A`), 2 bytes `[event, byte]`:

| event | meaning |
|-------|---------|
| 1 ARMED      | game armed as slave (`byte` = SB) |
| 2 DISARM     | game cancelled its armed transfer |
| 3 MASTER     | game started a master transfer with `byte`; FPGA is holding |
| 4 SLAVE_DONE | offered byte accepted; `byte` = SB shifted out |
| 5 SLAVE_NAK  | offered byte refused: not armed |
| 6 ABANDON    | SB/SC written under the held master; CPU stalled, write parked (FPGA write barrier) |
| 7 SLAVE_NAK_STALLED | offer refused while the port holds a master with the CPU stalled: master collision, the ESP asks the verdict at once (both sides read `0xFF`) |

The FPGA queues up to 16 events (18.17; 4 before), each a ~0.5 ms UART packet.
Pokemon Gold/Silver's partner search writes SB=2, SC=0x00, SC=0x80, SB=1,
SC=0x01, SC=0x81 within ~20 us (ARMED DISARM ARMED ARMED DISARM MASTER): the
4-deep queue dropped the `MASTER`, so the ESP32 ignored the next frame's
`ABANDON`, the CPU stalled ~4 s (the music slowed), and the safety timeout's
`0xFF` + IRQ landed under the parked `SB = 2`: the game read its own `0x02` as the
partner's and "connected" alone, then closed the link for inactivity. `ARMED` /
`DISARM` are hints, so with 12 or more events queued they are dropped to keep
room for the rest.

ESP32 -> FPGA, RX address `0x10`, 2 bytes `[cmd, byte]`:

| cmd | meaning |
|-----|---------|
| 1 MASTER_REPLY | complete the held master transfer with `byte` |
| 2 SLAVE_XFER   | offer the peer master's `byte` (answered by SLAVE_DONE / SLAVE_NAK) |
| 3 CONFIG       | `byte` bit 0 = Wi-Fi link enabled (physical port ignored); bit 1 = the ESP32 may be in light sleep: send the wake preamble before each packet (FPGA 18.15+); bit 2 = the ESP32 handles `ABANDON` / `MASTER_DROP` / event 7, so the FPGA may use the write barrier and the fast-clock stall (FPGA 18.16+; without it the FPGA behaves as 18.15, so a new FPGA under an older ESP32 build never stalls the CPU waiting for a verdict that will not come); bit 3 = standing offers (18.20+, the ESP32 withdraws them with cmd 6) |
| 4 MASTER_DROP  | release the held (abandoned) master without completing it: no IRQ, SB untouched; then the parked write applies |
| 6 SLAVE_WITHDRAW | take back the standing offer (18.20+): `SLAVE_NAK` if it still waited, no event if it was exchanged |
| 5 CART_POWER_CYCLE | debug (18.18+, console `cwl cartcycle`): cartridge power off ~1 s as the power switch would, so a flash cart returns to its menu (a soft reset or an FPGA reload keeps the cart powered and replays the loaded game) |

The FPGA also completes a held master transfer with `0xFF` after ~4 s of no
reply (safety net for a dead ESP32). From 18.16 that is ~4 s in CGB double
speed too (it counted CPU ticks, so it was ~2 s there: shorter than the ESP32's
wake hold plus master timeout, 2 + 1.5 s, which assumes 4 s).
