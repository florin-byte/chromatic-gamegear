## v1.1.12

1.1.11 plus a CUSTOM page for custom firmware. **Goes with FPGA 18.33** (no FPGA change). Boot loader and partition table unchanged.

- SYSTEM has a CUSTOM item next to UPDATE, for firmware made at shinyrelay.com/custom-firmware. Not linked: A links this Chromatic (it shows a code to type on the website; the same pairing as ONLINE). Linked: it shows this Chromatic's ID, A checks for the build you sent, shows its name and version, and asks before it downloads and before it installs. On a custom build, B goes back to the official Shiny Relay.
- UPDATE is for official releases only. It never downloads or installs a custom build, and on a custom build it offers the official release.
- SYSTEM INFO shows the whole version. On the MCU / FPGA view the version has a row of its own under MCU:, so a custom build's version (for example v1.1.12-cfw.8104) is no longer cut off.
- SYSTEM INFO shows the FPGA version alone (FPGA: v18.33). It used to add "(dbg)", a mark every Shiny Relay FPGA carries because it is not ModRetro's own build; it said nothing a person needs.

## v1.1.11

1.1.10 plus warm whites for Game Boy games and game clocks that keep real time. **Goes with FPGA 18.33.**

- The warmth setting (Neutral, Warm 1 to 5) also warms Game Boy games. The COLOR TEMP page is there in Game Boy mode too.
- A game with a clock (Gold, Silver, Crystal) played from the Chromatic or its card keeps its in-game time across power-offs. The unit gets the real time over Wi-Fi and catches the clock up when the game loads, and never moves a clock back.
- SYSTEM > CLOCK sets the date and time by hand when Wi-Fi has not given it.
- SAVE TO MY GAMES now shows on a Chromatic that is not paired or has no Wi-Fi saved, and pressing it says what to do first.
- SAVE TO MY GAMES leaves more memory for its secure connection, so NOT ENOUGH MEMORY comes up less often.

## v1.1.10

1.1.9 plus SAVE TO MY GAMES. **Goes with FPGA 18.32** (no FPGA change).

- A game kept ON CHROMATIC gets a SAVE TO MY GAMES row: its newest save goes up to the paired account's My Games, where it can be downloaded. Nothing playing, Wi-Fi saved, paired.
- A clock game's save carries its clock, the same 48 bytes a card copy has.
- The same save sent again is not stored twice; a game holding 8 saves refuses and deletes nothing.

## v1.1.9

1.1.8 plus updates that stay. **Goes with FPGA 18.32** (no FPGA change).

- A new version confirms itself about 2 seconds after it starts with the FPGA link up (it was 20 seconds), and the unit no longer naps until it has. Before, switching off too soon after an update quietly put the old version back.

## v1.1.8

1.1.7 plus faster saves. **Goes with FPGA 18.32** (no FPGA change).

- A game that writes only when it saves reaches flash about 1.5 seconds after the save, measured on Pokemon Crystal. On 1.1.7 it waited for 1.5 seconds of quiet first and the write was slower.
- The flash write is faster: one link reset per save instead of one per 2 KiB, and the next save's area is erased ahead of time.
- A game that writes its save memory all the time still reaches flash every 10 seconds or so, so that the flash does not wear out.
- The unit no longer naps in the middle of a save copy.
- The log gives how long each save took to reach flash.

## v1.1.7

1.1.6 plus a fix for lost saves. **Goes with FPGA 18.32** (no FPGA change).

- A game playing from the unit's flash or card, with Wi-Fi quiet, could leave the unit asleep for minutes. While it slept nothing checked whether the game had saved, so a save stayed only in the unit's memory and was lost when the unit was switched off from inside the game. The unit now wakes about once a second while a game plays, checks for a new save, and copies it.
- The unit no longer goes to sleep between seeing a save and finishing its copy.
- `pwr` shows a new count, `wake_timer`, for these wake-ups.

## v1.1.0

1.0.9 plus Shiny Relay's own update over Wi-Fi (owner 2026-10-20). **Goes with FPGA 18.28.**

- **Two app slots** (partitions_ota.csv): ota_0 0x10000, otadata 0x200000 (8 KiB), ota_1 0x210000,
  0x1E0000 each; NVS and PHY data stay where they were, so the USB install from 1.0.x keeps the Wi-Fi
  network, the registration and the pairing. The image set clears otadata: a USB install always boots ota_0.
- **SYSTEM > UPDATE**: A: CHECK FOR UPDATE asks `POST /link_device/fw/check` (TLS, the registration token;
  the unit's version, FPGA, and the sha256 of its partition table and boot loader). An update that needs
  another FPGA, table or boot loader says UPDATE OVER USB. Otherwise DOWNLOADING NN% (plain http, Range
  resumes, a chunked answer is fine; the whole image hashed against the sha256 from the TLS call, then the
  IDF's own image check), UPDATE READY (A: INSTALL, B: LATER; a waiting update is checked again before use).
- **Rollback**: a new version confirms itself after 20 s with its FPGA link up; one that restarts before
  that is left for the old one by the boot loader, which then says UPDATE DID NOT START / THE OLD VERSION
  IS BACK. Until a new version confirms, nothing is staged (`info` says stg=000000).
- **Cart staging follows the slot that is not running**: the ROM at 0x210000 while ota_0 runs (the 1.0.x
  addresses) or at 0x10000 while ota_1 runs; the save at 0x208000 either way. `CBU1 info` says stg=.
- The boot loader is built without its build time, so every 1.1.x build makes the same boot loader.
- The device call body is 256 bytes (the update check's is about 190).
- Bench: `cart fw [show|a|b]` (SYSTEM > UPDATE from the console); TEST build option NW_OTA_TEST_NO_CONFIRM.

## v1.0.3

1.0.2 plus "Send to my Chromatic" over Wi-Fi in receive mode, and the cartridge written by the FPGA.
The development builds were called 1.0.3-rc1 to rc3 (lanes wifirecv1, wifimem1, wifispeed1) and
1.0.4-cs1 / cs2 (lane cartspeed1); their notes follow below. 1.0.4 never existed as a release:
it was a working name.

**Goes with FPGA 18.28** (the 1.0.3 release image, fpga branch release-1.0.3). Every pairing still
works: on any other FPGA the cartridge is written the classic way (the Game Boy CPU and the 115,200
baud link), as in 1.0.2.

- **SYSTEM > DOWNLOAD** (A: RECEIVE GAME): the unit shows a six-character code; type it at
  shinyrelay.com and the game is sent to this Chromatic. No pairing (an account-less registration).
  B cancels; the code is then given back and the site says it expired.
- **One kept TLS connection** for the receive calls (register, start, every poll), so the free heap
  stays flat while the unit waits (no TIME-WAIT pile-up).
- **One download.** The image comes in ONE plain-http GET of AES-256-CTR ciphertext (the key arrives
  in the TLS call and lives in RAM only), resumed with a Range request after a drop, decrypted into
  the ESP32's staging area and checked (crc32 + sha256) before the cartridge is touched.
- **The express cart write** (FPGA 18.26 to 18.28 only): the staged bytes go over the QSPI bus into
  the FPGA's ring and the FPGA erases, programs and reads back the flash itself. About 26 s for
  2 MiB (classic route: about 5 minutes). Any other FPGA minor, older or newer, keeps the classic
  route until a firmware names it.
- **The cart is written while the download is still running** (express route only).
- **A on the result screen is NEXT GAME**: receive mode again at once (B: DONE).
- **Memory:** the receive start is asked again twice when the first TLS handshake gets no
  connection; TIME-WAIT 6 s instead of 120 s; the staging erase runs on the HTTP task (no 4 KiB
  stack from the heap); objects made on first use are made at boot; the static RAM given back
  (release DRAM 160,220 bytes) so a game's ONLINE ticket call (gated on 21,000 bytes free) is
  never held.
- `cart usb status` names the loaded FPGA image (`fpga=18.28`), so the cart page skips its own
  engine load on a unit that already runs it.
- Release builds talk https only to the API (api.shinyrelay.com / betaapi.shinyrelay.com). The only
  plain-http request is the image download, which carries ciphertext (owner-approved design,
  2026-10-12). The http-API build is a separate TEST image and is never released.

## 1.0.3 development: cartspeed1 (built as 1.0.4-cs1 / cs2): the cart written by the FPGA

**Needs FPGA 18.26** (fpga branch cartspeed1 d839589). Any other pairing keeps the route below
(the classic one): firmware 1.0.0-1.0.3 on 18.26, or this firmware on 18.21-18.25. Nothing has to
change in lockstep.

**What changed.** A staged image (the USB page's, source 2; a Wi-Fi install's download, source 3)
no longer goes byte by byte through the Game Boy CPU and the 115,200-baud UART. The ESP32 writes
it over the QSPI bus it already uses for the menu screen (40 MHz, 4 lines) into a 4 KiB ring in
the FPGA, with address bit 31 set so it never lands in PSRAM. The FPGA's express stream
(cart_xs.v) erases, programs and reads back by itself, through the cart bus engine. The writer
waits meanwhile.
- The UART carries only the ops and the posts (ESP32 -> FPGA, address 0x12) and a 32-byte status
  per KiB or per op (FPGA -> ESP32, address 0x0C). Its rate is unchanged, so the link cable's
  timing is unchanged.
- Nothing stops and waits: up to 3 ops are in the FPGA at once, and 1 KiB posts follow one another.
  Each post carries a crc32, and the FPGA checks it before it uses the bytes; a refused post is sent
  again from where the FPGA stands.
- Order: block 0 is erased first. Then, per 64 KiB block: erase, program its non-FF range (every
  byte read back by the FPGA), and read it back for a crc32 that must be the staged bytes'. The
  block is then journalled done. Blocks past the image are read back and erased if not blank. The
  save RAM is written and read back in 1 KiB pieces. Block 0 comes last, and the 4 entry bytes at
  0x100 are the last write. Its crc32 must match, or it is erased again.
- Any FPGA error or a crc32 that does not match: ABORT, RESET and the block again, with the safe
  bus timing from then on. After max_retries + 1 such restarts the install stays unfinished (it
  resumes later; the done blocks are kept). A result that a merged status missed is read again,
  never erased again.
- Bus timing 2 (the FPGA's shortest: 1 / 2 / 1 hclk write, 3 hclk read) by default. Override with
  `cart xs timing N` (Wi-Fi) or `cart usb install timing=N`. `cart xs off` and
  `cart usb install classic` force the old route.
- The OSD's rows and the ring's polling writes share one mutex. On unit 2 the OSD's queue_trans hit
  a polling transaction: the assert in send_lines rebooted the ESP32 mid-install, and the install
  resumed and finished.

**Measured on unit 2** (SST39VF1682, local backend; the before route is wifirecv1 rc3 b4e3391,
whose writer is v4):

| | before | after |
|---|---|---|
| Wi-Fi 256 KiB, cart write + verify | 94.7 s | 14.4 s |
| Wi-Fi 2 MiB, cart write + verify | 392 s | 25.9 s |
| USB 256 KiB, install | 91.2 s (128 s end to end) | 13.9 s (55 s) |
| USB 2 MiB, install | 383 s (445 s end to end) | 26.0 s (87.7 s) |
| the chip, measured by the FPGA | | program wait 7.73 us per byte, block erase <= 17.9 ms |

- aa961e8's v5 writer (CGB double speed) brought the classic 2 MiB write to 306-308 s on unit 1.
- A cut mid-write (FPGA reload at 40 % of a 2 MiB USB install) resumed with no new copy (12 blocks,
  14.3 s). The full cart read back afterwards equals the file.
- Simulation (cf_sim --xs, FPGA model host/cf_xs_model.inc): power cuts mid-erase and mid-program,
  lost status packets (1 in 2), a bad QSPI bit, a stuck flash bit and a small image over a full cart
  all pass.
- What limits it now: the chip's byte-program time (7.7 us) plus four bus writes (1.9 us at timing
  2), against the ~11.7 us per byte measured; the remaining ~2 us is the ESP32 side between blocks.
  On Wi-Fi, the download now takes longer than the cart write.

## 1.0.3 development: rc3 (lane wifimem1)

**Receive mode's memory (unit 1, PARTY BATTLE v8 in the cart, against production).** The slide in free
heap was not a leak: every connection this side closes leaves its TCP PCB in TIME-WAIT for 2 x MSL
(120 s), ~212 bytes each. With a new TLS connection every 4-second poll they piled up to 26 (free heap
32.6 KB -> 25.5 KB in two minutes, then flat), and each handshake took the heap to 1.5-7.6 KB (median
2.1 KB; one call failed outright).

- Receive mode keeps ONE TLS connection (HTTP/1.1 keep-alive, the edge says `Connection: keep-alive`)
  for register, recv/start and every recv/poll that is not an offer: one handshake per receive
  session. Closed: by any other call (an offer, resume, cancel, the acks), 12 s after the last
  receive call, when a cart job holds TLS, when receive mode ends, and on `Connection: close`. A kept
  connection the server dropped fails before any answer and the call goes once more on a new one.
  A call over the kept connection needs 8,000 bytes free (no handshake); a new one still 21,000.
- `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` 4096 -> 2048: the outgoing record buffer is allocated whole for
  every write (DYNAMIC_BUFFER); our largest write is the HTTP client's 1,536-byte header buffer and
  esp-tls splits longer ones. The incoming side stays 16 KB. Certificate checking is unchanged.
- Measured (production, 404 answers, the full call each time): 330 polls over ~22 minutes on ONE
  connection, every poll ~120 ms, free heap flat at 23.3 KB, largest block 20.5 KB, lowest during a
  poll 17.6-17.9 KB, no TIME-WAIT PCB. The one handshake per session still dips to ~7.6 KB.
- Every device call prints its heap: before (largest block), lowest during the call, after, and the
  TIME-WAIT PCBs; `cwl status` has https_conns / reuses / retries / kept and the last call's heap.
- Bench: `cart rxprobe N [MS] [fresh]` (receive mode's link hold + N recv/poll calls with no token,
  `fresh` = a new connection each as before), `cart rxprobe off`.

**Wi-Fi install speed (lane wifispeed1, owner 2026-10-15: no stop and wait).** Before: one ranged
2 KiB GET per chunk, asked only when the cart write needed it, then the whole cart read back over the
115,200-baud FPGA link: a 256 KiB install took 272 s from the send (unit 2, local server), ~2 KB/s.
Now the image comes in ONE plain-http GET (`Range: bytes=N-` after a drop: the hashes carry on),
decrypted into the ESP32's staging area (the USB staged install's, 0x210000; a 64 KiB block is
checked blank and erased only if it is not), its crc32 + sha256 checked against the offer BEFORE the
cart job is armed. The cart is then written from the staged bytes (new source 3,
CF_SRC_WIFI_STAGED) with the staged install's fast verify (CRC16 per bank on the Game Boy side,
against the hashed bytes; the header still last). A full 2 MiB image's last 64 KiB (the protected top
block) must be FF: checked and hashed, not stored. The staged copy is erased (and read back blank)
when the install is over or given up; an unfinished install keeps it and a restart re-hashes it
instead of downloading again. The install machine: `arm` may answer CF_ARM_WAIT (downloading,
INSTALLING 0-9 %) or CF_ARM_NO_DATA (10 minutes with no byte: paused, resumes by recv/resume like a
stopped write, never reported failed). The download state is heap (static DRAM 159,568: +88 over rc2).
The cart write itself is bound by the FPGA UART (115,200 baud, divisor fixed in the RTL).

**On unit 1 (2026-10-15, after the units came back).**

- The download: AES-CTR now runs over each whole read (mbedtls_aes_crypt_ctr, the counter carried
  from read to read; was one hardware call per 16 bytes), its 2 KiB per-chunk buffer is freed, reads
  are 2 KiB. The 'downloading' report reuses receive mode's kept TLS connection (it is no longer closed
  when the offer comes; a TLS hold closes it only after 3 s, longer than the header read).
- The cart writer v5 runs in CGB double speed (KEY1 + STOP at its entry; DelayMs counts for 2x; the
  power cycle at the job's end puts the console back): its loops (window copy, program, compare, CRC)
  take half the time; flash cycles stay the FPGA engine's own timing.
- Beta (betaapi.shinyrelay.com, the blob over Cloudflare), from the send to INSTALLED:
  2 MiB 358.6 s (arrived 1.5 s; 'downloading' 0.2 s on the kept connection; download 33.3 s =
  network 12.8, decrypt 6.7, flash 10.3; the cart session 1.5 s; write + fast verify 308 s; first block
  13.5 s), with writer v4 ~432 s (write + verify 375 s); 256 KiB 75.6 s. Before (rc2, unit 2, local
  server): 256 KiB 272 s.
- A 60 s server outage 5 s into a 2 MiB download (local server): it stopped at 162,777, waited,
  resumed at that byte on a new connection, the hashes held, installed; the cart read back equal to the
  image (sha256 8dcf63b2...).
- What bounds it: the cart side. The simulator (which matched the unit within 2 %: 383 vs 392 s for
  2 MiB with writer v4) puts writer v4 at 383 s, 240 s with a 4x FPGA UART, 196 s with no UART limit;
  writer v5 at 288 / 145 / 101 s. The FPGA UART's divisor is fixed in the RTL (uart.v), no flow control.
  cf_sim --fpga-bpms X models another UART rate.

Tests: cf_install_test 152/152 (ASan + UBSan; new: the download wait, a stopped download, an image
gone), cart simulator 106/106 (new: strm_wstg, strm_wstg_cut: 2 MiB from the staged download, a power
cut mid-erase and the resume, not one byte asked from the network), osd 70 literals 0 over, cwl host
tests. Both configs build; stack + flash checks pass.

## 1.0.3 development: rc2 (lane wifirecv1)

"Send to my Chromatic" over Wi-Fi in an explicit RECEIVE MODE with a code (owner-approved design
2026-10-14). Nothing polls the server in the background any more, and there is no "press A to
accept an offer" question.

- New SYSTEM item **DOWNLOAD**, right below NETWORK (only in the Wi-Fi install build), one action:
  **A: RECEIVE GAME**. It stops the link (game link, room, relay seat, ESP-NOW, LAN, the game's
  network window) so the TLS calls have the heap, asks the server for a receive code
  (`POST /link_device/recv/start`) and shows it: YOUR CODE, the six characters, TYPE IT AT
  SHINYRELAY.COM, WAITING FOR A GAME, minutes left, B: CANCEL. It polls that session every 4 s
  (`/recv/poll`, the server's `poll_ms`). The code is 6 characters of BCDFGHJKMNPQRSTVWXZ23456789,
  valid 10 minutes; CODE EXPIRED has A: NEW CODE. B cancels any time before the write starts (the
  code is given back, `/recv/cancel`; a game that already came is reported "declined").
- When a game arrives the install runs as in 1.0.3-rc1 (header read, BATTERY LOW waits for the
  charger, "downloading" before the write, the AES-256-CTR image over plain http with the key in RAM
  only and wiped on every exit, INSTALLING NN%, INSTALLED / NOT INSTALLED + why). The link comes
  back when it ends.
- The unit needs no pairing: every receive call goes with an account-less REGISTRATION token
  (`POST /link_device/register`, NVS `cwlrel/rtok`, never printed), got once on first use; a 401
  registers again once, then NOT REGISTERED.
- INSTALL NOT FINISHED: a pause in receive mode resumes by itself up to 3 times
  (`/recv/resume {id}`, not a poll); after a restart it waits for A (RESUME) / B (CANCEL).
- Errors on the page: NO WI-FI (none saved / not joined in 30 s), NO CONNECTION, NOT REGISTERED,
  TOO MANY TRIES, SERVER BUSY, UPDATE THIS CHROMATIC FIRST (not FPGA 18.24+: no call is made).
- Every cart gate is unchanged (the SST39VF1681 write test, the FPGA rule, battery and USB power).
- Console: `cart receive` (A on the menu), `cart answer yes|no` (A / B on the page), `cart inst`
  (codes, polls, registration, receive-mode heap: before the hold, after it, lowest).

Found on the units (2026-10-14): a game that arrived was "refused to arm" right after its header
read (the header's connection still closing; 'downloading' now waits for it); on a unit with a SHLINK
game in the cart the polls were held under the TLS floor (the room client and the online task's HTTP
client are now freed in receive mode). Console `cart code` (the code on the page, on request) and
`cart receive` also opens SYSTEM > DOWNLOAD (the hardware suite's camera).

Tests: host `cf_install_test` 141/141 (ASan + UBSan), `cf_ctr_test`, the cart simulator suite;
osd `make test` (every row fits) and `make dl-shots` (every DOWNLOAD screen, from the real page
over the real install machine).

## v1.0.2

1.0.1 plus three window ops so a game can do online from its own screens (lane tetrispatch1,
PARTY BATTLE v7: HOST shows its code on the game's screen, JOIN types the code with the D-pad,
PARTY goes straight to the public match). Additive: a game that never sends them (every game
before PARTY BATTLE v7: v3-v6, NO-WAIT 2-PLAYER, the Shiny GBC SHLINK games) sees 1.0.1 exactly;
firmware before 1.0.2 answers them NOT_IDENTIFIED (the game reads the version first).

- `0x0E LINK_ROOM` mode + arg: the game chose its room (host, join a code, the quick match, a
  room by id, leave), as the room menu's choice, with no menu. A game that did this never gets
  the room menu from an ask afterwards (until another game attaches).
- `0x0F LINK_STATUS`: what the game needs to explain itself: online, a Wi-Fi network saved,
  paired, the room's state and why it ended (no such code, full, closed), host, peers, the
  room's code, and the pairing state and code (the game shows SHINYGEN.AI/LINK and the code).
- `0x10 LINK_UI` what: 1 opens SYSTEM > WI-FI on the screen (the Wi-Fi password is the one
  thing a game cannot take), 2 starts pairing, 3 cancels it, 4 opens the room menu.

Tests: host `shl_test` 88/88 (new: the three ops, unchanged XCHG / JOIN for an old game),
`party_test`, `shl_burst_sim`; the tetrispatch1 harness (the device's own nw_link / shl_client
code) with v7 on 1.0.2, v3-v7 carts on 1.0.0 / 1.0.1 / 1.0.2 mixed in one room.

### Cartridges with the SST39VF1681 flash chip (lane chip1681, owner 2026-10-13)

A customer's ModRetro Tetris v1.4 cart reads `BF C8` (SST39VF1681); 1.0.0 / 1.0.1 refuse it with
code=2 ("not verified"). 1.0.2 writes it. Its WP# pin, when held low, protects the BOTTOM 64 KiB
(DS25040A), where every game starts; whether a 1681 Tetris board holds it low is not known. So
before the first erase of every job that writes the flash, the **write test** (cf_esp.c "WP TEST")
programs one bit of one byte of that block and reads it back:

- the byte: the highest one that ends a run of 16 FF bytes (padding), else the highest FF, else
  the highest byte with a 1 bit outside the header, else a header byte; v -> v & (v - 1);
- it took: the job goes on (that block is erased next anyway);
- it did not: **new code 13 (CF_D_LOCKED)**, `CBU1 fail rom code=13 (the cartridge's first block is
  write-protected: nothing was erased or written)`; nothing erased, nothing else programmed, the
  journal idle (nothing unfinished); a first block of only 00 bytes cannot be tested: code 13 too;
- a test that does not finish cleanly (the read-back is neither the old nor the new value, or the
  block does not read back): **code 14 (CF_D_WPTEST)**, `CBU1 fail rom code=14 (the cartridge's write
  test did not read back cleanly: nothing was erased)`, journal idle; never code 4, which means a cart
  in flux (erase / program / verify keep code 4 exactly as before);
- when: STREAM (USB write, staged install, Wi-Fi) after its data channel check, right before
  block 0 is erased; INSTALL / RESTORE / ROUNDTRIP after the verified-backup gate and the power
  check, before the journal says "in flux". A resumed write skips it. Backup, read, save-only
  (FRAM) and selftest jobs never run it.

Everything else is unchanged: the SST39VF1682 (protect_bottom 0) never runs the test, the
journal layout is the same, the other chips are refused as before. Host `cart_usb.py` stops on
code=13 (a refusal that will not change) and has `savonly --sav`. cf_sim models WP# (`--chip 1681`,
`--wp-low`; cf_cart `protect_bottom`); `run_tests.sh` adds the wp_* scenarios and the USB / staged
scenarios on a 1681 with WP# high and low.

## v1.0.1

1.0.0 plus two changes for party games (family `SHPTY1:`, e.g. the PARTY BATTLE Tetris patch);
everything else is as 1.0.0 (HOST / JOIN CODE for 2-player games, other families, other menus,
packets, the relay protocol).

- **PARTY goes straight into the public match** (owner 2026-10-12). When a party game asks for
  its room (lk_ask, or the LINK_JOIN menu bit) and no room is live, the ESP32 takes the quick
  match (MY DEVICES, mode 2) at once and the room menu does not open. Before, the menu opened
  with the cursor on HOST, and HOST made a private room per unit, so two units sat alone in
  separate rooms. A 1.0.1 unit lands in the same public room as a 1.0.0 unit that picked MY
  DEVICES (the same SJOIN). The public-match question still opens the menu first for an account
  that never answered (as when the player picks MY DEVICES). An app instruction (join CODE,
  Invite my Chromatic) still wins. The decision is `PartyAsk_AskMode` (party_ask.c).
- **The OSD's player count for a party room is right.** The relay's state item says `peers` =
  the OTHER players for a game room but ALL players (you included) for a party seat, and the OSD
  added 1 to both: a party lobby of 4 showed PLAYERS 5. Now `PartyAsk_ShownPlayers`. (The count
  also includes seats that sit out after a match until they leave or drop: that is the relay's
  count, not changed here.)

Tests: host `party_test` (48 checks, new ones for both), `shl_test`, `shl_burst_sim`; the
tetrispatch1 harness (the device's own nw_link / shl_client / party_ask code behind SameBoy)
with 1.0.1 and 1.0.0 units mixed and v3 / v4 carts mixed: every unit in ONE public room, the
match and a second match play.

## v1.0.0 (first public release)

The same code as 0.18.0-usb7 (e8d562d), released under a plain version number. Only the app
version string changes (sdkconfig.defaults CONFIG_APP_PROJECT_VER). The network window now
reports firmware 1.0 (fw_major 1, fw_minor 0).

## v0.18.0-usb7 (lane cartflash1, TEST)

0.18.0-usb6 plus: no light sleep during a cart console session. On unit 1 (usb5, the page) the
first console command after about 10 s without traffic got no answer twice: the MCU had gone
into light sleep, and the keystroke that wakes it loses its first characters. The power
manager now also asks the cart code (CfDev_KeepAwake): awake while the link hold is on, a job
is armed or finishing, the staged copy is being erased, and for 60 s after any `cart usb`
command. Outside a session it answers false and the system sleeps as before.

## v0.18.0-usb6 (lane cartflash1, TEST)

0.18.0-usb5 plus the link hold. On an ONLINE unit (unit 2, 2026-10-12: Wi-Fi joined, paired,
LINK CABLE ONLINE) every cart job was refused with `CBU1 fail busy memory`: the radio and its
HTTPS sessions held the heap the job needs. Now the first cart job command takes a RAM-only
hold before anything else: no new TLS, the link session ends as on a mode change (relay BYE,
Wi-Fi stopped), the online request in flight finishes and the kept HTTPS clients (online and
network window) are freed. 30 s after the last `cart usb` command with no job running the hold
is lifted and the session the LINK CABLE mode wants starts by itself (Wi-Fi joins, the relay
reseats, nearby comes back). A heap refusal now says `CBU1 fail busy memory (free N, needs M)`.
Tests: cwl `logic_test` (the link may not run while held, comes back in every mode after);
cf_sim `stg_online` (the job is refused until the hold is taken; the hold is lifted after).

## v0.18.0-usb5 (lane cartflash1, TEST: release candidate of the USB cart line)

0.18.0-usb4 (whose erase of the staged copy passed on a unit) plus:
- "tidy" about three times faster: the writer's blank check tests 8 bytes per turn (the slow
  pass still finds and counts wrong bytes when a block is not blank). Simulation, 385 KiB:
  22.9 s -> 7.2 s. Both verify modes; every check as before.
- 5.1 KiB of static RAM back (heap on every unit): 163,200 -> 158,088 bytes of .data + .bss
  (0.17.10: 156,064). The console write buffers are one heap block that exists only during a
  write over the console; the staging check and the bench commands allocate when used.
- The erase task has 4 KiB of stack (its read-back buffer is on it).

## v0.18.0-usb4 (lane cartflash1, TEST)

0.18.0-usb3 with the erase of the staged copy fixed. On a unit, usb-cart-2 and -3 **aborted**
when the staged ROM and save were to be erased (after an install and on `cart usb clear`), so
the copy stayed in the unit's flash: 0x208000-0x310000 is inside the running app's partition
(stock table: factory 0x10000 + 0x300000) and the IDF aborts on a raw erase there. The global
guard stays (CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS); the staging area is erased (and, for the
Wi-Fi install, written) through its own view of the flash chip whose only difference is the
region check: exactly 0x208000-0x400000, and only when the firmware image ends below it. After
the erase the area is read back; `CBU1 cleared` only when it is blank, else
`CBU1 fail clear code=37`. `cart_usb.py` exits with an error when an install or `clear` does not
end in `CBU1 cleared`. New build check `host/check_flash.py`: the staging area against the real
partition table, and no raw erase / write through the default chip (it fails on the usb3 tree).

## v0.18.0-usb3 (lane cartflash1, TEST)

0.18.0-usb2 plus, after the first run on a unit (256 KiB + save installed at the first attempt):
- `CBU1 time rom <phase> <ms>` when a phase of an install ends, and
  `CBU1 time rom total <ms> erases=N retries=N` before the `done` / `fail` line: where the time
  goes on a unit.
- A save piece the cart did not take is counted and said (`CBU1 note The game is installed, but
  the save could not be written ...`); before, only the log had it and the job said `done`.
- `stage rom` lets the idle task run every 64 KiB (no task watchdog warning on a 2 MiB file).
- Link: the two release fixes of relfix1 (full power restores "always awake" with no Wi-Fi
  network; LINK CABLE OFF -> AUTO wakes the link) replace the linkperf4 picks of usb1; the retry
  of a failed radio / ESP-NOW start is kept (relfix1 does not cover a start that fails). The
  linkperf test knobs (`cwl xlat`, `espnow rate`, `espnow chan`) are gone from this build.
- `cart_usb.py`: `code=36` is not final (stage and install again); the console counts as up
  only after two answered asks a second apart; every log line has the time since the start.

## v0.18.0-usb2 (lane cartflash1, TEST)

0.18.0-usb1 with one fix: **every `cart usb` command restarted the ESP32** (unit 1): the command
ran in the console task (4,096 bytes of stack) with a 2,272-byte frame of its own (two 1 KiB
chunk buffers on the stack) under the cart command's 1,088. No buffer is on that stack any more
(368 and 48 bytes), the bench commands keep a frame of their own, and the build now fails when a
cart command would need more stack than its task has (`components/nw/cartflash/host/check_stack.py`,
from the compiler's -fstack-usage figures, in the shipping configuration). `cart status` prints
the least stack the console and cart tasks have ever had left.

## v0.18.0-usb1 (lane cartflash1, TEST: the USB cart install; needs the cart-engine FPGA image in SRAM)

0.17.10 plus the USB cart route. On the normal FPGA it behaves as 0.17.10 (a `cart usb` job
answers code 238 after 25 s and restarts the game once). No Wi-Fi install, no patches, no
send-ahead in this build.

### Added
- **`cart usb` (protocol CBU1, docs/CART_USB_STAGED_INSTALL.md and CART_BACKUP_USB_PROTOCOL.md):**
  backup of the cart's ROM and save to the computer; **staged install**: the computer writes
  the ROM (0x210000) and the save (0x208000) into the ESP32's flash in bootloader mode, then
  `stage rom` / `stage sav` check them (crc32 + sha256) before anything on the cart is touched,
  `install [fast]` writes the cart from them (first block erased first, blocks journalled, the
  cart checked, the entry point written last) and the firmware erases the staged data when it
  is done; `clear` erases it at any time. `fast`: the cart is checked bank by bank with a CRC16
  worked out on the Game Boy side instead of reading every byte back.
- The FPGA's cart commands are chosen from the FPGA that is loaded, at run time: 18.21 / 18.22 use
  8 and 7, 18.24 and later 9 and 10, and on 18.23 no cart job starts at all.
- A build check and a run-time check that the firmware image ends below the staging area.

### Fixed (from lane linkperf4)
- **The link could stay asleep after a mode change.** When a link session ended on a mode change
  (LINK CABLE between AUTO / NEARBY / ONLINE, `cwl off` / `on`), the link task waited with the
  radio off until a game used the link port. In every release since 0.17.4. A failed radio or
  ESP-NOW start is now tried again. (With it come the test commands `cwl xlat`, `cwl espnow rate`
  and `cwl espnow chan`, which change nothing unless typed.)

## v0.17.10

### Fixed
- **The help pages' last row was cut** ("1/6 A: NEXT  B: BA", 0.17.9 on a unit). A row on SYSTEM >
  NETWORK holds 23 characters, and that footer was 25 (the same footer was in 0.17.7 and 0.17.8).
  It is now `1/6 A:NEXT B:BACK`. The Mac check of these texts tests the character limit as
  well as the pixel width. Nothing else changed from 0.17.9.

## v0.17.9

### Changed
- **HOW TO PLAY ONLINE is six short pages** (SYSTEM > NETWORK, B), down from 26: PAIR, INVITE
  (NO CODE), ROOM CODE, LINK-CABLE, NOT CONNECTING, and MORE HELP: SHINYRELAY.COM/DOCS. There is
  no index any more: A (or DOWN) is the next page, UP the one before, B closes, A on the last
  page closes. Nothing else changed from 0.17.8.

## v0.17.8

### Added
- **A game can ask for the room menu as it joins.** LINK_JOIN's mode byte: bit 7 set
  ("open the room menu now") opens SYSTEM > NETWORK at once when no room is live, the same
  as the game's later ask. For cartridges built with the link library's `net_menu_now()`,
  which ask at boot (the first exchange used to swallow that ask). Games that do not set the
  bit behave exactly as before.

## v0.17.7

### Added
- **SYSTEM > NETWORK > HOW TO PLAY ONLINE.** Press B on the NETWORK page (with or without a
  game running). An index of five parts (BEFORE YOU START, WAY 1: INVITE, WAY 2: ROOM CODE,
  LINK-CABLE GAMES, NOT CONNECTING): UP / DOWN to choose, A to open. In a part: A or DOWN
  for the next page, UP for the one before, B back to the index. The words are the one help
  text used in the app and on shinyrelay.com, in the menu's capitals. On the NETWORK page
  only A switches the service on and off now (B did too).
- **The one-time question before a public party match.** When the account has not yet
  said OK to public matches, picking MY DEVICES (the quick match) in a party game shows
  PUBLIC MATCH / YOUR NAME WILL BE VISIBLE / A: OK, SELECT: GENERIC NAME, B: BACK, instead
  of joining. It also comes when the relay refuses such a seat (ENDED reason 14,
  `party_consent`). A or SELECT saves the answer for the account
  (`POST /link_device/party_consent`), then the device buys a new ticket and joins once
  more. If the answer cannot be saved, or the relay refuses again, the room menu shows
  NO ROOM / TRY AGAIN LATER; it never retries by itself. B makes no call. HOST and JOIN
  CODE never ask. The device stores nothing: the answer is the account's.

### Fixed
- `cwl cartcycle` (a debug console command) could end with the Chromatic's menu open when
  it started open (5 of 7 runs on a unit). The MCU light-sleeps 100 ms after the FPGA goes
  quiet, which paused the FPGA tasks under the sequence. It now keeps the MCU awake for
  the sequence and waits for the menu to report each change, and prints whether it ended
  closed.
- `cwl relay ticket` (console) now brings a dormant radio up for its try, as a game's
  network use does. Before, it waited until something else woke Wi-Fi.

## v0.17.6

Two fixes on 0.17.5, from the device run of 2026-10-11 on two units. Nothing else changed.

### Fixed
- **The room menu now opens by itself when a game asks for a room.** A game's "play online"
  prepares SYSTEM > NETWORK with HOST selected and taps MENU for the player. The tap used
  button bit 9, which the FPGA does not have for presses (it keeps 9 bits; MENU is bit 8,
  and bit 9 only exists in the FPGA's status reports, where it means "the menu is
  closed"). On real units the menu therefore stayed closed. The tap now uses bit 8.
- `cwl cartcycle` (a debug console command) left the Chromatic's menu open after its reset
  combo, because releasing the combo's MENU bit toggles the menu. It now closes the menu
  again, and closes it first if it was open (the reset only works with the menu closed).
- **No HTTPS call is started with less than 21 KB of free memory.** The TLS handshake needs
  about that much; started with less it failed every time and took free memory down to
  1-4 KB while it tried. Such a call now counts as a failed try at once, so the usual
  2 to 60 s backoff applies. `cwl relay status` has `heap_holds` (calls skipped this way).
  On this firmware free memory is normally well above the limit (about 38 KB idle), so
  nothing changes in normal use.

### Added
- `components/button/button_poke.h` names the poke masks, and a Mac test pins them
  (`make -C components/button/host test`).

## v0.17.5

From tonight's device tests (lane fwfix2, on 0.17.4).

### Fixed
- The relay follows the API's stack: `cwl relay api https://betaapi.shinyrelay.com`
  now also moves the relay to betarelay.shinyrelay.com:6861 (prod API: relay.shinyrelay.com:5861).
  A ticket naming another known stack's relay (the beta API's tickets named the prod one,
  so a beta invite's room was asked of the prod relay: no_room) is used on the API's own
  relay; a relay host the backend names for its own stack is still taken, and
  `cwl relay relayhost` still overrides everything.
- A SHLINK seat keeps the radio at full power from the relay's welcome on, not only once
  the first room state arrives (the first acks are not held up by modem sleep).

### Changed
- `nw status` shl section: `held_back` and `items_in`.

## v0.17.4

Link-cable feel over Shiny Relay, part A (lane linkperf1, on top of 0.17.3): from the owner's Tetris Battle
test on two Chromatics (Nearby), where about as many master transfers were abandoned as
completed and presses felt lost.

### Changed
- The link task waits for work instead of sleeping in the socket read: an FPGA port event
  or an ESP-NOW frame wakes it at once, and the packet it makes goes out in the same pass
  (0.17.2 parked every packet made from a port event until the next 1 ms tick, and woke
  for a frame or event only at a tick). LAN / relay datagrams are still read every tick.
- The FPGA UART hands bytes to the ESP32 one symbol after the line goes quiet (was the
  driver's 10 symbols, ~0.9 ms per port event).
- At full power (a partner answering, the link used in the last 20 s) Wi-Fi power save is
  checked every second and switched off again if anything turned it on (counted).
- Expected: ~4-5 ms less per exchange round trip at the port (simulated: master hold at a
  2 ms radio round trip 9.9 -> 5.0 ms), which moves the point where a once-per-frame game
  starts losing frames from a ~7 ms to a ~13 ms radio round trip.

### Added
- `cwl xstat` / `cwl xstat reset`: exchanges per second (last 8 windows), master hold and
  CPU-park histograms (<1/<8/<17/<33/>=33 ms) and frames parked more than half, the last 8
  abandons (time since master start, park time, outcome), the slave's port state when a
  peer request arrives (armed / waited / refused), the exchange round-trip histogram,
  time per power level since the reset, REQ/ACK sent below full power, power-save fixes.
- The protocol core carries the send-ahead extension (PROTOCOL.md), OFF in this build: it
  needs the FPGA slave lock (18.22, not built). Wire traffic is identical to 0.17.2.

## v0.17.3

### Changed
- SYSTEM > WI-FI list: SET UP WITH PHONE, SAVED NETWORKS and SCAN AGAIN come first,
  above the scanned networks, so phone setup shows without scrolling (UP/DOWN still
  walks the whole list and wraps). A network's name now always starts right of the
  padlock (it overlapped the name's first letter on unselected rows).

## v0.17.2

Phone setup and a console clean-up, from the owner's test of 0.17.1 on unit 2 (lane fwux1).

### Changed
- SET UP WITH PHONE opens on a choice: SCAN A QR CODE or TYPE IT IN, each on its own
  screen with B: BACK; the choice screen shows what the phone is doing. The QR is drawn
  with ECC L (62 px, 2 px modules) so B: BACK fits under it.
- Phone setup is quicker: the setup page serves several connections at once (a silent
  one no longer holds the others for 2 s), DNS answers non-A questions with an empty
  answer at once, the home network is joined on its scanned channel (no 13-channel scan,
  and the setup network stays on the air meanwhile), the link rejoins on that channel,
  the phone's status page refreshes every second and the setup network closes 5 s after
  CONNECTED (was 8). Setup network: WPA2-AES only.

### Fixed
- No more task-watchdog "Backtrace:" dumps during the ~6 s TLS handshakes (pairing,
  tickets, gateway): the handshake's core is taken off the idle-task watchdog only for
  the handshake.

## v0.17.1

Security fixes from the pre-launch review (lane fwfix1). Same FPGA (18.21, network
window); nothing else changes.

### Security
- The `CWS?` UDP status probe is gone from release builds (Kconfig
  `CONFIG_SHINY_RELAY_DEBUG_STATUS_PROBE`, on only in `sdkconfig.debug`). It answered
  anyone on the link port with the Wi-Fi network name, the relay room code and ~1 KB
  of counters for a 4-byte request (an amplifier for a spoofed source). The counters
  stay on the USB console (`cwl status`, `nw status`); `tools/cws.py` needs a debug
  build.
- Phone setup network password: 15 random characters as `XXXXX-XXXXX-XXXXX` (62 bits,
  was 8 letters, ~40 bits) from `ACEFGHJKLNPRTX3479`, the capitals and digits the OSD
  font draws unmistakably. The OSD shows it on its own row under `PASSWORD (CAPS)`
  (time left moved to the button row); the Wi-Fi QR code is 70 px (version 4, 2 px
  modules). The 5 minute limit is unchanged.
- The device API (`cwl relay api`) and the network-window gateway (`nw url`) must be
  `https://`: every request to them carries a token. The console refuses `http://`,
  an `http://` URL already saved is dropped at boot (default API / the paired host),
  and no request is sent over one. `CONFIG_SHINY_RELAY_DEBUG_ALLOW_HTTP_API` (debug
  builds) allows `http://` for a LAN mock.

### Build
- `sdkconfig.debug`: bench overlay with both debug options
  (`idf.py -B build_debug -D SDKCONFIG=build_debug/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.debug" build`).
- App 1,612,832 bytes (0.17.0: 1,614,160), static DRAM/IRAM unchanged.

## v0.17.0

Integration of the 0.15/0.16 local-link work (branch ble1) and SHLINK on the
Chromatic (branch shlink1, lane chromshlink1) on cwl1.

### Added
- SHLINK: a Shiny Gen GBC game's multiplayer room through the network window
  (FPGA 18.21+). The window ops LINK_JOIN / LINK_XCHG / LINK_LEAVE (0x0B-0x0D) are
  answered by the ESP32 itself (components/nw/nw_link.c, CartridgeLink's session/ack
  rules, never HTTPS); the room client (shl_client.c) speaks SJOIN / SWELCOME / SDATA
  over UDP to Shiny Relay (reliable in-order stream, flow control by ack, rejoin by
  room id), with tickets from the pairing. Full power only while a room is live.
- OSD SYSTEM > NETWORK is the running SHLINK game's ROOM MENU (status, players, HOST
  shows the code, JOIN CODE, MY DEVICES, LEAVE); a game's lk_ask opens the OSD there.
- Console `nw room host|mine|join <code>|leave` (a request made before a SHLINK game
  attaches waits for its LINK_JOIN); `nw status` has a `shl` section (room, counters,
  free heap).
- Gateway fixes from nwgate1: one device token (the pairing's), persistent keep-alive
  HTTP client, response headers captured (X-Total-Length, Content-Range,
  X-Gbnet-Notify -> NOTIFY), base URL defaults to the paired API host + /gbnet/v1.

### Changed
- The ONLINE "VIA auto/lan/relay" setting is retired: the LINK CABLE mode (AUTO /
  NEARBY / ONLINE / OFF) alone picks the paths. A stored `lan` becomes NEARBY and
  `relay` becomes ONLINE once at boot (only over AUTO). `cwl relay mode` says it is
  retired; relay-only without P2P stays as the hidden console option
  `cwl relay force on`. OSD ONLINE shows `LINK <mode>`; its settings are ROOM, CODE,
  UNPAIR.
- SYSTEM order: FIRMWARE, PLAYER #, LINK CABLE, ONLINE, WI-FI, NETWORK (SERIAL #).
  OSD lists show a 5-row window that keeps the selected item inside the panel (the
  6th SYSTEM item used to be drawn over the panel's bottom edge).
- Heap: the SHLINK room client and its buffer (3.8 KB) live on the heap only while a
  SHLINK game is attached or a room is live (static DRAM 157256 -> 153536 B; 0.16.0:
  152632). 6 static Wi-Fi RX buffers instead of 10 (RX BA window 4).

### Measured (owner's Chromatic, FPGA 18.21, home Wi-Fi)
- Heap: idle with the link up 34.7 KB free (0.16.0: 29.4 KB); minimum after the
  boot-time TLS ticket fetches 13.8-15.3 KB (0.16.0: 8.2 KB); minimum with a live
  SHLINK room plus TLS ticket fetches 10.0-10.5 KB (with 10 RX buffers it was
  1.9-3.7 KB).
- SHLINK through the real GB CPU (test boot ROM, SRAM-only FPGA load) to a Mac echo
  partner: prod relay 48/48 messages, p50 102 ms, p95 135 ms round trip; beta relay
  72/72, p50 134 ms, p95 187 ms. 0 lost.
- LAN linktest 0 errors both directions, relay-only (prod) linktest 0 errors both
  directions, wake from dormant: IP 107 ms, partner 672 ms.

## v0.16.0

### Added
- SYSTEM > WI-FI: choose the network on the device. Scan (signal bars, a lock for
  networks with a password), pick one, type the password on a D-pad keyboard (letters,
  capitals, digits and symbols, SPACE, SHOW / HIDE, DEL, OK), join with a plain-words
  result (CONNECTED, WRONG  PASSWORD, NETWORK  NOT  FOUND, NETWORK  REFUSED  US,
  NO  IP  ADDRESS, NO  ANSWER). A network is saved (and used by the link) only when
  the join worked; a failed join leaves the old network in place.
- Saved networks: up to 5, newest first (NVS `cwlnet`); join or forget one from the
  menu. The single network of older builds becomes the first saved one. When the
  current network fails 3 joins, the strongest saved network in range is joined
  (scan at most every 30 s).
- Phone setup (SYSTEM > WI-FI > SET UP WITH PHONE, console `cwl net portal`): a
  temporary WPA2 network `ShinyRelay-XXXX` with an 8-letter password and a Wi-Fi QR
  code on screen, a captive page (any DNS name answers 192.168.4.1; phones' probe URLs
  redirect to it) listing the scanned networks, which takes the network and password,
  joins, saves and closes. 5 minute limit; B stops it. A tiny HTTP/1.0 server inside
  the setup task (esp_http_server cost ~9 KB of heap); heap left while it runs:
  ~17 KB (measured).
- Console `cwl net status|list|scan|join <i>|try <ssid> <pass>|forget <i>|portal [lan]|
  stop|backup|restore` (backup / restore copy the network settings inside NVS; nothing
  prints a password). CWS? `net=<ssid> saved=N`.

### Changed
- The link session pauses while a scan, join or phone setup runs (one radio); the MCU
  stays awake meanwhile.
- Trace ring 384 -> 256 entries (1 KB of RAM back).

## v0.15.0

### Added
- LINK CABLE modes (SYSTEM > LINK CABLE, was WI-FI LINK; console `cwl mode`): AUTO
  (ESP-NOW + LAN + relay at once; a nearby partner wins), NEARBY (ESP-NOW + LAN, no
  router needed), ONLINE (relay only), OFF. Old `enabled` maps to AUTO / OFF.
- ESP-NOW nearby link (components/cwl/cwl_espnow.c + the portable frame layer
  cwl_lx.c): CWL2 packets unchanged in ESP-NOW frames, broadcast discovery, a lock
  confirmed both ways, auto-pairing remembered in NVS with a LINKED toast, a pick list
  of nearby sides by name, channel: the AP's when joined, else a home channel plus a
  sweep. Console `cwl nearby|partner|name|espnow ...`, `cwl xrtt`, `cwl heap`.
- Path choice across ESP-NOW, LAN and relay (first heard wins; same partner moves to
  the better path; exchanges only on the chosen path).

### Notes
- Bluetooth LE is parked (components/cwl/parked, not built).
- Free heap with the link up: ~29.4 KB (0.14.0: ~32.0 KB); minimum during the relay's
  TLS ticket fetch ~8.2 KB (0.14.0: ~9.1 KB).

## v0.13.8

### Changed
- Updated FPGA version number

## v0.13.7

### Changed
- Change default value for usbcolorcorrection from 1->0.

## v0.13.5

### Note 
The FPGA must be updated to v18.9 or newer for PCB-aware battery voltage estimate

### Changed 
- Uses most significant bit of 16-bit ADC packet sent from FPGA to determine which
battery voltage calculation to execute.

## v0.13.4

### Note
The FPGA must be updated to v18.7 or newer for fixed sprite palette behavior

### Changed
- Fixed bug where sprite palettes are not set correctly when using palette
selection tab.

## v0.13.3

### Note
The FPGA must be updated to v18.6 or newer for palette selection and updated
frame blending behavior.

### Added
- The ability to change the color palette for non-GBC titles through a Palette tab.

### Changed
- Renamed the frame blending toggle button text to "1:1" from "3:1" to clarify
the updated FPGA approach.
- Added redundancy to two-option menu widgets. Press B to toggle previous choice.
- Added up/down roll over to bottom/top for OSD navigation.

## v0.13.2

### Added
- The ability to mock button to the FPGA.

### Changed
- Simplified the firmware version reported the system tab. Press A to see details.

## v0.13.0

### Added
- Option to ignore diagonal inputs from the D-Pad
- Option to use the background palette 0 color (prevents screen transition flashing)
- Option to set low battery icon display behavior
- Added message to brightness to indicate low power mode is active
- Added hot keys to the Controls tab

### Changed
- Tuned battery voltage thresholds for 1.2V NiMH AA's
- Prevent backlight adjustments when in low power mode
- Renamed 'USB Color Correction' menu option to 'Streaming'
- Redesigned Controls tab, toggle option text

## v0.12.3

### Note
The FPGA must be updated to v17.0 or newer as the underlying communication protocol changed.

## Added
- Sleep the MCU on power up to reduce power consumption.
- Support LiPo battery thresholds and charging indication in the battery menu.

## Fixed
- Highest brightness setting is not properly restored on power up.
- Hot key brightness adjustments are saved between power cycles

### Changed
- Supports updated UART protocol.

## Removed
- The Frame Blending text hint on the Display screen.

## v0.11.2
This is the first production release.
