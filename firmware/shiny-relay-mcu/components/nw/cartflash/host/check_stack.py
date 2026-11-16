#!/usr/bin/env python3
"""check_stack.py BUILD_DIR: the stack the cart code needs in each task, from the compiler's own
figures (-fstack-usage, the .su files of the nw component), against each task's configured stack.

usb-cart-1 (2026-10-12) restarted the ESP32 on every `cart usb` command: cf_usb_cmd had a
2,272-byte frame (two 1 KiB chunk buffers) under cmd_cart's 1,088, in the console task, whose
stack is 4,096 bytes in the release build. The host simulation has no task stacks and could not
see it. This runs as part of the firmware build (CMake target cart_stage_room) and fails it.

What it checks:
  1. no function that runs in the console task for `cart usb` has a frame above FRAME_MAX;
  2. the deepest chain known for each task, plus what libc's printf and the task's own base
     need, leaves MARGIN bytes of that task's stack.
The chains are written out below by hand (there is no call graph here): when a handler gains a
call, add it.
"""
import glob
import os
import sys

build = sys.argv[1]
frames = {}
for su in glob.glob(os.path.join(build, "esp-idf", "nw", "**", "*.su"), recursive=True):
    src = os.path.basename(su)[:-3]
    for line in open(su):
        f = line.rstrip("\n").split("\t")
        if len(f) < 2:
            continue
        name = f[0].split(":")[-1]
        frames[(src, name)] = int(f[1])
if not frames:
    sys.exit("check_stack: no .su files under %s (is -fstack-usage on for the nw component?)" % build)


def fr(src, name):
    # fwshrink1 (-Os inlines more): "a+b" = a static callee b that the compiler may inline into a (same file):
    # the frames of those in the build, at least one; "b?" = a leaf that may be inlined into the chain's previous
    # function (its frame is then part of that one's): 0 when not in the build.
    if name.endswith("?"):
        return frames.get((src, name[:-1]), 0)
    parts = name.split("+")
    have = [frames[(src, n)] for n in parts if (src, n) in frames]
    if not have:
        sys.exit("check_stack: %s:%s is not in the build (renamed? update the chains)" % (src, name))
    return sum(have)


LIBC_PRINTF = 80 + 800 + 320      # printf + _vfprintf_r + what it calls (newlib, measured frames 80 and 800)
FRAME_MAX = 512
# (task, configured stack, base the task uses before our code, margin wanted, [chains])
TASKS = [
    ("console_repl", 4096, 900, 1024, [
        # esp_console_repl_task -> linenoise -> esp_console_run (base) -> cmd_cart -> cmd_cart_usb -> cf_usb_cmd -> say -> usb_line -> printf
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_cmd"), ("cf_usb.c", "say"), ("cf_dev.c", "usb_line")],
        # ... -> cf_usb_cmd -> arm -> cf_esp_arm -> logf_ -> printf
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_cmd"), ("cf_usb.c", "arm"), ("cf_esp.c", "cf_esp_arm"), ("cf_esp.c", "logf_")],
        # ... -> cf_usb_cmd (stage) -> cf_sha256_update -> block
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_cmd"), ("cf_sha256.c", "cf_sha256_update"), ("cf_sha256.c", "block")],
        # ... -> cmd_cart_usb -> cf_usb_rx -> want_again -> want_say -> say -> usb_line -> printf   (the binary reader)
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_rx"), ("cf_usb.c", "say"), ("cf_dev.c", "usb_line")],
        # ... -> cmd_cart_usb -> cf_usb_hold -> usb_net_hold -> printf   (the link hold, usb-cart-6; CWL_CartHold / CwlOnline_Quiet / NW_DropClient only wait)
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_hold"), ("cf_dev.c", "usb_net_hold")],
        # ... -> cf_usb_cmd -> arm -> busy_mem -> say -> usb_line -> printf   (a heap refusal)
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_cmd"), ("cf_usb.c", "arm"), ("cf_usb.c", "busy_mem"), ("cf_usb.c", "say"), ("cf_dev.c", "usb_line")],
        # the bench commands (their own frame, never under the usb one)
        [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_bench")],
    ]),
    ("nw", 6144, 600, 1024, [
        # NwTask -> nw_esp (handle_request) -> the cart request -> on_result -> plan_next -> begin_write_plan -> logf_ -> printf
        [("nw_dev.c", "NwTask"), ("nw_esp.c", "nw_esp_on_packet+handle_request"), ("cf_esp.c", "on_result"), ("cf_esp.c", "plan_next"), ("cf_esp.c", "begin_write_plan"), ("cf_esp.c", "logf_")],
        # NwTask -> cf_esp_tick / the request -> stream_result -> stream_next -> finish -> logf_ -> printf
        [("nw_dev.c", "NwTask"), ("nw_esp.c", "nw_esp_on_packet+handle_request"), ("cf_esp.c", "on_result+stream_result"), ("cf_esp.c", "stream_next"), ("cf_esp.c", "logf_")],
        # NwTask -> CfDev_Tick -> cf_usb_tick -> say -> usb_line -> printf
        [("nw_dev.c", "NwTask"), ("cf_dev.c", "CfDev_Tick"), ("cf_usb.c", "cf_usb_tick"), ("cf_usb.c", "say"), ("cf_dev.c", "usb_line")],
        # ... -> cf_usb_tick -> phase_time -> say -> usb_line -> printf   (the per-phase time lines)
        [("nw_dev.c", "NwTask"), ("cf_dev.c", "CfDev_Tick"), ("cf_usb.c", "cf_usb_tick"), ("cf_usb.c", "phase_time"), ("cf_usb.c", "say"), ("cf_dev.c", "usb_line")],
        # receive mode (wifirecv1): NwTask -> CfDev_Tick -> cf_install_tick -> take_offer -> cf_install_parse -> jfind
        [("nw_dev.c", "NwTask"), ("cf_dev.c", "CfDev_Tick"), ("cf_install.c", "cf_install_tick"), ("cf_install.c", "take_offer"), ("cf_install.c", "cf_install_parse"), ("cf_install.c", "blob_url_ok?")],
        # ... -> cf_install_tick -> finish -> report_try -> api -> ci_api_start (the device call is queued, not made here)
        [("nw_dev.c", "NwTask"), ("cf_dev.c", "CfDev_Tick"), ("cf_install.c", "cf_install_tick"), ("cf_install.c", "finish"), ("cf_install.c", "report_try"), ("cf_install.c", "api"), ("cf_dev.c", "ci_api_start")],
        # ... -> cf_install_tick -> rx_fail -> logf_ -> ci_log -> printf ; -> rx_begin -> rx_on -> ci_hold -> printf
        [("nw_dev.c", "NwTask"), ("cf_dev.c", "CfDev_Tick"), ("cf_install.c", "cf_install_tick"), ("cf_install.c", "rx_fail"), ("cf_install.c", "logf_"), ("cf_dev.c", "ci_log")],
        [("nw_dev.c", "NwTask"), ("cf_dev.c", "CfDev_Tick"), ("cf_install.c", "cf_install_tick"), ("cf_install.c", "rx_begin"), ("cf_install.c", "rx_on?"), ("cf_dev.c", "ci_hold_rx+ci_hold+ci_hold.isra")],
    ]),
    # lane savecloud1: the nw HTTP task (nw_dev.c HttpTask, kHttpStack 6144) steps SAVE TO MY GAMES's upload next to the stream's GET
    ("nw_http", 6144, 600, 1024, [
        # HttpTask -> CfDev_HttpWork (up_work and net_req are inlined into it) -> cf_sup_step -> up_open (the connection; the handshake's own frames are the same as net_req's) -> printf
        [("nw_dev.c", "HttpTask"), ("cf_dev.c", "CfDev_HttpWork"), ("cf_sup.c", "cf_sup_step"), ("cf_dev.c", "up_open")],
        # ... -> cf_sup_step -> up_answer / up_read (CfRc_LibShareRead -> CfRc_GameAreaRead)
        [("nw_dev.c", "HttpTask"), ("cf_dev.c", "CfDev_HttpWork"), ("cf_sup.c", "cf_sup_step"), ("cf_dev.c", "up_answer")],
        # the precedent that runs on this task today: the stream's GET (TLS gateway route)
        [("nw_dev.c", "HttpTask"), ("cf_dev.c", "CfDev_HttpWork")],
    ]),
]
CONSOLE_FUNCS = [("cf_dev.c", "cmd_cart"), ("cf_dev.c", "cmd_cart_usb"), ("cf_usb.c", "cf_usb_cmd"), ("cf_usb.c", "cf_usb_rx"),
                 ("cf_usb.c", "say"), ("cf_usb.c", "arm"), ("cf_dev.c", "usb_line"), ("cf_esp.c", "cf_esp_arm"), ("cf_esp.c", "logf_")]

bad = 0
for k in CONSOLE_FUNCS:
    v = fr(*k)
    flag = "" if v <= FRAME_MAX else "   <-- above %d" % FRAME_MAX
    bad += v > FRAME_MAX
    print("frame %5d  %s:%s%s" % (v, k[0], k[1], flag))
for task, size, base, margin, chains in TASKS:
    worst = 0
    for ch in chains:
        total = base + sum(fr(*k) for k in ch) + LIBC_PRINTF
        worst = max(worst, total)
        left = size - total
        flag = "" if left >= margin else "   <-- less than %d left" % margin
        bad += left < margin
        print("task %-12s stack %5d  needs %5d (base %d + frames %d + printf %d)  left %5d  %s%s" % (
            task, size, total, base, sum(fr(*k) for k in ch), LIBC_PRINTF, left, " -> ".join(k[1] for k in ch), flag))
    print("task %-12s worst case %d of %d, %d to spare" % (task, worst, size, size - worst))
sys.exit(1 if bad else 0)
