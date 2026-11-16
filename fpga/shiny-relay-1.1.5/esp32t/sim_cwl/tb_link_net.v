// tb_link_net.v: link.v (CWL1 network mode) + link_net_cdc.
// Drives CPU writes to SB/SC and ESP32 commands; checks events and results.
`timescale 1ns/1ps

module tb_link_net;
    reg hclk = 0, gclk = 0;
    always #29.8 hclk = ~hclk;               // ~16.78 MHz
    always @(posedge hclk) gclk <= ~gclk;    // hclk / 2

    reg rst = 1;
    reg [1:0] ce_div = 0;
    always @(posedge hclk) ce_div <= ce_div + 1;
    wire ce = (ce_div == 2'd3);              // CPU clock enable, 1 in 4

    // CPU bus
    reg sel_sc = 0, sel_sb = 0, cpu_wr_n = 1;
    reg [7:0] cpu_do = 0;

    wire [7:0] sb, sc_r;
    wire serial_irq, sc_start, sc_int_clock, clk_out, data_out;

    wire h_net_en, h_mr_pend, h_mr_ack, h_sx_pend, h_sx_ack, h_ev_valid, h_mr_drop, cpu_stop, h_barrier_en;
    wire h_standing_en, h_wd_pend, h_wd_ack;
    reg  double_speed = 0;
    wire [7:0] h_mr_byte, h_sx_byte, h_ev_byte;
    wire [2:0] h_ev_type;

    link #(.NET_TIMEOUT_BITS(10)) dut (
        .clk_sys(hclk), .ce(ce), .rst(rst),
        .sel_sc(sel_sc), .sel_sb(sel_sb), .cpu_wr_n(cpu_wr_n),
        .sc_start_in(cpu_do[7]), .sc_speed_in(cpu_do[1]), .sc_int_clock_in(cpu_do[0]),
        .sb_in(cpu_do), .sc_r(sc_r),
        .serial_clk_in(1'b1), .serial_data_in(1'b1),
        .serial_clk_out(clk_out), .serial_data_out(data_out),
        .sb(sb), .serial_irq(serial_irq), .sc_start(sc_start), .sc_int_clock(sc_int_clock),
        .net_en(h_net_en),
        .net_master_reply_pend(h_mr_pend), .net_master_reply_byte(h_mr_byte), .net_master_reply_ack(h_mr_ack),
        .net_master_reply_drop(h_mr_drop), .net_cpu_stop(cpu_stop),
        .net_barrier_en(h_barrier_en), .net_double_speed(double_speed),
        .net_slave_xfer_pend(h_sx_pend), .net_slave_xfer_byte(h_sx_byte), .net_slave_xfer_ack(h_sx_ack),
        .net_standing_en(h_standing_en), .net_slave_withdraw_pend(h_wd_pend), .net_slave_withdraw_ack(h_wd_ack),
        .net_ev_valid(h_ev_valid), .net_ev_type(h_ev_type), .net_ev_byte(h_ev_byte),
        .SaveStateBus_Din(64'd0), .SaveStateBus_Adr(10'd0), .SaveStateBus_wren(1'b0),
        .SaveStateBus_rst(1'b0), .SaveStateBus_Dout()
    );

    reg g_cmd_valid = 0; reg [7:0] g_cmd = 0, g_cmd_byte = 0;
    wire g_ev_valid; wire [2:0] g_ev_type; wire [7:0] g_ev_byte; reg g_ev_taken = 0;
    wire g_net_en, g_may_sleep;

    link_net_cdc cdc (
        .gclk(gclk), .g_reset(rst), .hclk(hclk), .h_reset(rst),
        .g_cmd_valid(g_cmd_valid), .g_cmd(g_cmd), .g_cmd_byte(g_cmd_byte),
        .g_ev_valid(g_ev_valid), .g_ev_type(g_ev_type), .g_ev_byte(g_ev_byte),
        .g_ev_taken(g_ev_taken), .g_net_en(g_net_en), .g_esp_may_sleep(g_may_sleep),
        .h_net_en(h_net_en),
        .h_master_reply_pend(h_mr_pend), .h_master_reply_byte(h_mr_byte), .h_master_reply_ack(h_mr_ack),
        .h_master_reply_drop(h_mr_drop), .h_barrier_en(h_barrier_en),
        .h_slave_xfer_pend(h_sx_pend), .h_slave_xfer_byte(h_sx_byte), .h_slave_xfer_ack(h_sx_ack),
        .h_standing_en(h_standing_en), .h_slave_withdraw_pend(h_wd_pend), .h_slave_withdraw_ack(h_wd_ack),
        .h_ev_valid(h_ev_valid), .h_ev_type(h_ev_type), .h_ev_byte(h_ev_byte)
    );

    integer errors = 0;
    integer irq_count = 0;
    always @(posedge hclk) if (serial_irq && ce) irq_count = irq_count + 1;

    // Event log (gclk side): the UART sender takes each event after a delay.
    reg [10:0] ev_log [0:255];
    integer ev_n = 0;
    // slow_uart: each event takes a real UART packet (~0.52 ms at 115200,
    // 6 bytes) before the next is taken, as system_monitor does on hardware.
    reg slow_uart = 0;
    integer uart_busy = 0;
    always @(posedge gclk) begin
        g_ev_taken <= 0;
        if (uart_busy > 0) uart_busy = uart_busy - 1;
        else if (g_ev_valid && !g_ev_taken) begin
            ev_log[ev_n] = {g_ev_type, g_ev_byte};
            ev_n = ev_n + 1;
            g_ev_taken <= 1;
            if (slow_uart) uart_busy = 4370;
        end
    end

    task cpu_write(input is_sc, input [7:0] v);
        begin
            @(posedge hclk); #1; while (ce_div != 2'd3) begin @(posedge hclk); #1; end
            sel_sc <= is_sc; sel_sb <= !is_sc; cpu_wr_n <= 0; cpu_do <= v;
            @(posedge hclk);           // the ce edge
            @(posedge hclk);
            sel_sc <= 0; sel_sb <= 0; cpu_wr_n <= 1;
        end
    endtask

    task esp_cmd(input [7:0] c, input [7:0] b);
        begin
            @(posedge gclk); g_cmd_valid <= 1; g_cmd <= c; g_cmd_byte <= b;
            @(posedge gclk); g_cmd_valid <= 0;
        end
    endtask

    task wait_cycles(input integer n);
        integer i; begin for (i = 0; i < n; i = i + 1) @(posedge hclk); end
    endtask

    task check(input cond, input [8*64-1:0] msg);
        begin
            if (!cond) begin errors = errors + 1; $display("FAIL: %0s (sb=%02h sc_start=%b irq_count=%0d ev_n=%0d)", msg, sb, sc_start, irq_count, ev_n); end
            else $display("ok:   %0s", msg);
        end
    endtask

    task check_ev(input integer idx, input [2:0] t, input [7:0] b, input [8*64-1:0] msg);
        begin
            if (idx >= ev_n || ev_log[idx] !== {t, b}) begin
                errors = errors + 1;
                $display("FAIL: %0s (event %0d = %03h, want %03h, have %0d events)", msg, idx, ev_log[idx], {t, b}, ev_n);
            end else $display("ok:   %0s", msg);
        end
    endtask

    integer irq0, ev0;
    initial begin
        wait_cycles(10); rst = 0; wait_cycles(10);

        // 1. Network off: master transfer with nothing attached reads 0xFF.
        irq0 = irq_count;
        cpu_write(0, 8'h29); cpu_write(1, 8'h81);
        wait_cycles(4 * 512 * 9);
        check(irq_count == irq0 + 1 && sb == 8'hFF && !sc_start, "net off: unplugged master reads FF");
        check(ev_n == 0, "net off: no events");

        // 2. Enable the network link.
        esp_cmd(3, 1); wait_cycles(20);
        check(h_net_en && g_net_en && !g_may_sleep, "CONFIG enables net link");
        esp_cmd(3, 3); wait_cycles(20);
        check(h_net_en && g_net_en && g_may_sleep, "CONFIG(3): net link on, ESP may sleep");
        esp_cmd(3, 1); wait_cycles(20);
        check(h_net_en && g_net_en && !g_may_sleep, "CONFIG(1): ESP awake again");

        // 3. Master is held until MASTER_REPLY.
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h29); cpu_write(1, 8'h81);
        wait_cycles(4 * 800);           // under the 1024-tick sim timeout
        check(sc_start && irq_count == irq0, "master held while no reply");
        check_ev(ev0, 2'd3, 8'h29, "MASTER(29) event");
        esp_cmd(1, 8'h55); wait_cycles(40);
        check(!sc_start && sb == 8'h55 && irq_count == irq0 + 1, "MASTER_REPLY(55) completes master");

        // 3b. A held master restarted by the game (SC = 0x81 again, as Tetris
        //     DX does every frame) keeps the transfer: no second MASTER event,
        //     one reply completes it once.
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h00); cpu_write(1, 8'h81);
        wait_cycles(4 * 100);
        cpu_write(1, 8'h81);
        wait_cycles(4 * 100);
        cpu_write(1, 8'h81);
        wait_cycles(40);
        check_ev(ev0, 2'd3, 8'h00, "MASTER(00) event");
        check(ev_n == ev0 + 1, "SC rewrite while held: no second MASTER");
        check(sc_start && irq_count == irq0, "SC rewrite while held: still held");
        check(!cpu_stop, "SC rewrite while held: no CPU stall (restart is not a barrier)");
        esp_cmd(1, 8'h5A); wait_cycles(40);
        check(!sc_start && sb == 8'h5A && irq_count == irq0 + 1, "one MASTER_REPLY completes the restarted master once");

        // 4. Slave arms, re-arms on SB write, completes on SLAVE_XFER.
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h77); cpu_write(1, 8'h80);
        wait_cycles(40);
        cpu_write(0, 8'h78);
        wait_cycles(40);
        check_ev(ev0, 2'd1, 8'h77, "ARMED(77) event");
        check_ev(ev0 + 1, 2'd1, 8'h78, "re-ARMED(78) on SB write");
        check(sc_start && irq_count == irq0, "slave waits");
        esp_cmd(2, 8'h29); wait_cycles(40);
        check(!sc_start && sb == 8'h29 && irq_count == irq0 + 1, "SLAVE_XFER(29) completes slave");
        check_ev(ev0 + 2, 3'd4, 8'h78, "SLAVE_DONE(78) reports the byte shifted out");

        // 5. Disarm, then a stale SLAVE_XFER must not complete anything.
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h10); cpu_write(1, 8'h80); cpu_write(1, 8'h00);
        wait_cycles(40);
        check_ev(ev0, 2'd1, 8'h10, "ARMED(10)");
        check_ev(ev0 + 1, 2'd2, 8'h10, "DISARM back to back (FIFO)");
        esp_cmd(2, 8'h33); wait_cycles(40);
        check(sb == 8'h10 && irq_count == irq0 && !h_sx_pend, "SLAVE_XFER while not armed refused");
        check_ev(ev0 + 2, 3'd5, 8'h10, "SLAVE_NAK reported");

        // 6. Master timeout: completes with FF after 2^10 ce ticks.
        irq0 = irq_count;
        cpu_write(0, 8'h42); cpu_write(1, 8'h81);
        wait_cycles(4 * 1100);
        check(!sc_start && sb == 8'hFF && irq_count == irq0 + 1, "master times out to FF");
        esp_cmd(1, 8'h66); wait_cycles(40);   // late reply
        check(sb == 8'hFF && !h_mr_pend && irq_count == irq0 + 1, "late MASTER_REPLY dropped");

        // The ESP32 announces it handles the write barrier (CONFIG bit 2).
        esp_cmd(3, 5); wait_cycles(20);
        check(h_net_en && h_barrier_en && !g_may_sleep, "CONFIG(5): net link on, write barrier on");

        // 8. Write barrier, peer exchanged: SB written under a held master is
        //    parked, the CPU stalls, ABANDON goes out; MASTER_REPLY completes
        //    the master (IRQ, peer byte) and only then the parked SB lands.
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h29); cpu_write(1, 8'h81);
        wait_cycles(40);
        cpu_write(0, 8'hDD);
        wait_cycles(40);
        check_ev(ev0, 3'd3, 8'h29, "barrier: MASTER(29)");
        check_ev(ev0 + 1, 3'd6, 8'h29, "barrier: SB write under the held master -> ABANDON");
        check(cpu_stop && sc_start && sb == 8'h29 && irq_count == irq0, "barrier: CPU stalled, write parked, still held");
        esp_cmd(1, 8'h55); wait_cycles(40);
        check(!cpu_stop && !sc_start && irq_count == irq0 + 1, "barrier: MASTER_REPLY completes (IRQ), CPU resumes");
        check(sb == 8'hDD, "barrier: then the parked SB write lands");
        check(ev_n == ev0 + 2, "barrier: no other events");

        // 9. Write barrier, peer did not exchange: SC = 0x80 (arm) under a
        //    held master; MASTER_DROP releases it without an IRQ, then the
        //    parked arm applies (ARMED with the master's old SB).
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h31); cpu_write(1, 8'h81);
        wait_cycles(40);
        cpu_write(1, 8'h80);
        wait_cycles(40);
        check_ev(ev0 + 1, 3'd6, 8'h31, "drop: SC write under the held master -> ABANDON");
        check(cpu_stop && sc_start && sc_int_clock, "drop: CPU stalled, master still held");
        esp_cmd(4, 8'h00); wait_cycles(40);
        check(!cpu_stop && irq_count == irq0, "drop: MASTER_DROP releases without IRQ");
        check(sc_start && !sc_int_clock && sb == 8'h31, "drop: parked SC = 0x80 applied (armed slave)");
        check_ev(ev0 + 2, 3'd1, 8'h31, "drop: ARMED(31) after the release");
        cpu_write(1, 8'h00); wait_cycles(40);   // disarm

        // 10. CGB fast clock (SC = 0x83): the CPU stalls from the start until
        //     the reply (30 us on hardware).
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'hFE); cpu_write(1, 8'h83);
        wait_cycles(40);
        check_ev(ev0, 3'd3, 8'hFE, "fast: MASTER(FE)");
        check(cpu_stop && sc_start, "fast: CPU stalled while held");
        esp_cmd(2, 8'hFC); wait_cycles(40);   // the peer is a stalled master too
        check_ev(ev0 + 1, 3'd7, 8'hFE, "fast: offer refused while stalled -> SLAVE_NAK_STALLED");
        check(cpu_stop && sc_start && irq_count == irq0, "fast: still held after the collision report");
        esp_cmd(1, 8'hDD); wait_cycles(40);
        check(!cpu_stop && !sc_start && sb == 8'hDD && irq_count == irq0 + 1, "fast: reply completes, CPU resumes");

        // 11. Normal clock (SC = 0x81) keeps the CPU running while held.
        irq0 = irq_count;
        cpu_write(0, 8'h12); cpu_write(1, 8'h81);
        wait_cycles(40);
        check(!cpu_stop && sc_start, "slow: CPU runs while held");
        esp_cmd(1, 8'h34); wait_cycles(40);
        check(!sc_start && sb == 8'h34 && irq_count == irq0 + 1, "slow: reply completes");

        // 12. CGB double speed: the safety timeout counts every other tick
        //     (~4 s of wall clock in both speeds; the ESP32 wake hold needs it).
        double_speed = 1;
        irq0 = irq_count;
        cpu_write(0, 8'h43); cpu_write(1, 8'h81);
        wait_cycles(4 * 1100);
        check(sc_start && irq_count == irq0, "double speed: still held after 2^10 ticks");
        wait_cycles(4 * 1000);
        check(!sc_start && sb == 8'hFF && irq_count == irq0 + 1, "double speed: times out to FF after 2^11 ticks");
        double_speed = 0;

        // 13. An older ESP32 (CONFIG without bit 2): no barrier, no stall, no
        //     ABANDON / event 7 - the 18.15 behaviour, so a new FPGA cannot
        //     freeze games under an old ESP32 build.
        esp_cmd(3, 1); wait_cycles(20);
        check(h_net_en && !h_barrier_en, "CONFIG(1): write barrier off");
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h29); cpu_write(1, 8'h81);
        wait_cycles(40);
        cpu_write(0, 8'hDD);
        wait_cycles(40);
        check(!cpu_stop && sb == 8'hDD && sc_start, "no barrier: SB write lands at once, no stall");
        check(ev_n == ev0 + 1, "no barrier: no ABANDON");
        esp_cmd(1, 8'h55); wait_cycles(40);
        check(!sc_start && irq_count == irq0 + 1, "no barrier: reply completes");
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'hFE); cpu_write(1, 8'h83);
        wait_cycles(40);
        check(!cpu_stop && sc_start, "no barrier: fast-clock master does not stall");
        esp_cmd(2, 8'hFC); wait_cycles(40);
        check_ev(ev0 + 1, 3'd5, 8'hFE, "no barrier: refused offer is a plain SLAVE_NAK");
        esp_cmd(1, 8'h11); wait_cycles(40);
        check(!sc_start && sb == 8'h11 && irq_count == irq0 + 1, "no barrier: fast reply completes");

        // 14. Pokemon Gold/Silver partner search (WaitForLinkedFriend), with
        //     a real-speed UART: from an armed slave the game writes SB=2,
        //     SC=0x00, SC=0x80, SB=1, SC=0x01, SC=0x81 back to back (6 events). Every event must
        //     reach the ESP32, the MASTER above all (the 4-deep queue dropped
        //     it: the held master was unknown to the ESP32, its ABANDON was
        //     ignored and the CPU stalled until the safety timeout).
        esp_cmd(3, 5); wait_cycles(20);
        slow_uart = 1;
        cpu_write(0, 8'h02); cpu_write(1, 8'h00); cpu_write(1, 8'h80);   // entry: armed with SB=2
        wait_cycles(4 * 40000);                                        // let the UART drain
        ev0 = ev_n;
        uart_busy = 4370;                // the UART is sending another packet (status stream)
        cpu_write(0, 8'h02); cpu_write(1, 8'h00); cpu_write(1, 8'h80);
        cpu_write(0, 8'h01); cpu_write(1, 8'h01); cpu_write(1, 8'h81);
        wait_cycles(2 * 4370 * 2 * 8);
        check_ev(ev0,     3'd1, 8'h02, "gen2 loop: re-ARMED(02)");
        check_ev(ev0 + 1, 3'd2, 8'h02, "gen2 loop: DISARM(02)");
        check_ev(ev0 + 2, 3'd1, 8'h02, "gen2 loop: ARMED(02)");
        check_ev(ev0 + 3, 3'd1, 8'h01, "gen2 loop: re-ARMED(01)");
        check_ev(ev0 + 4, 3'd2, 8'h01, "gen2 loop: DISARM(01)");
        check_ev(ev0 + 5, 3'd3, 8'h01, "gen2 loop: MASTER(01) reaches the ESP32");
        // A burst of 20 hint events (a game rewriting SC=0x80) must not
        // crowd out the MASTER that follows.
        esp_cmd(1, 8'h02); wait_cycles(40);                 // complete that master
        ev0 = ev_n;
        cpu_write(1, 8'h80);
        repeat (20) cpu_write(0, 8'h05);                    // re-ARMED x20
        cpu_write(1, 8'h81);
        wait_cycles(2 * 4370 * 2 * 18);
        check(ev_n >= ev0 + 2 && ev_n <= ev0 + 17 && ev_log[ev_n - 1] === {3'd3, 8'h05}, "hint flood: hints dropped, MASTER(05) kept, last");
        esp_cmd(1, 8'h00); wait_cycles(40);
        slow_uart = 0;

        // 15. SC = 0x01 then 0x81 under a held master (Gen 2 LinkTransfer /
        //     Serial_ExchangeByte every frame): the stop is absorbed - no
        //     stall, no event, still held - and the 0x81 continues it; one
        //     reply completes it once. SC = 0x00 (clock source change) is
        //     still a barrier write.
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h61); cpu_write(1, 8'h81);
        wait_cycles(40);
        cpu_write(1, 8'h01);
        wait_cycles(40);
        check(!cpu_stop && sc_start && sc_int_clock && irq_count == irq0, "SC=01 under held master: absorbed, no stall, still held");
        cpu_write(1, 8'h81);
        wait_cycles(40);
        check(ev_n == ev0 + 1 && !cpu_stop && sc_start, "SC=01,81: no ABANDON, no second MASTER");
        esp_cmd(1, 8'hD1); wait_cycles(40);
        check(!sc_start && sb == 8'hD1 && irq_count == irq0 + 1, "SC=01,81: one reply completes it once");
        ev0 = ev_n;
        cpu_write(0, 8'h62); cpu_write(1, 8'h81);
        wait_cycles(40);
        cpu_write(1, 8'h00);
        wait_cycles(40);
        check(cpu_stop && ev_n == ev0 + 2 && ev_log[ev0 + 1] === {3'd6, 8'h62}, "SC=00 under held master: still a barrier (ABANDON, stall)");
        esp_cmd(4, 8'h00); wait_cycles(40);
        check(!cpu_stop && !sc_start, "SC=00: MASTER_DROP releases, parked SC=00 lands");

        // 16. Standing offers (CONFIG bit 3). An offer while the port holds a
        //     master waits; the Gen 2 search arms for a few cycles (SC=0x00,
        //     0x80) then SB=1 and SC=0x81 again: the offer is exchanged in that
        //     instant (SLAVE_DONE with the armed SB, IRQ). A withdrawal of a
        //     waiting offer gives SLAVE_NAK; of an exchanged one, nothing.
        esp_cmd(3, 8'h0D); wait_cycles(20);
        check(h_standing_en && h_barrier_en, "CONFIG(0D): standing offers on");
        irq0 = irq_count; ev0 = ev_n;
        cpu_write(0, 8'h01); cpu_write(1, 8'h81);           // searching side: master 01 held
        wait_cycles(40);
        esp_cmd(2, 8'h01); wait_cycles(40);                 // the peer's master byte 01
        check(h_sx_pend && ev_n == ev0 + 1, "standing: offer waits, no NAK");
        cpu_write(0, 8'h02);                                // SB=2 under the held master: barrier
        wait_cycles(40);
        esp_cmd(4, 8'h00); wait_cycles(40);                 // MASTER_DROP: SB=2 lands
        check(h_sx_pend && !cpu_stop, "standing: offer still waiting after the drop");
        cpu_write(1, 8'h00); cpu_write(1, 8'h80);           // armed for an instant...
        cpu_write(0, 8'h01); cpu_write(1, 8'h01); cpu_write(1, 8'h81);
        wait_cycles(40);
        check(!h_sx_pend && irq_count == irq0 + 1, "standing: exchanged the instant the port armed (IRQ)");
        check(ev_log[ev0 + 2] === {3'd4, 8'h02} && ev_log[ev0 + 3] === {3'd3, 8'h01}, "standing: SLAVE_DONE(02) (the armed SB), then MASTER(01)");
        check(sc_start && sc_int_clock && sb == 8'h01, "standing: game went on to its next master (SB=1 written after)");
        esp_cmd(1, 8'h02); wait_cycles(40);                 // complete that master
        ev0 = ev_n;
        esp_cmd(2, 8'h33); wait_cycles(40);                 // not armed: waits
        esp_cmd(6, 8'h00); wait_cycles(40);                 // withdraw
        check(!h_sx_pend && !h_wd_pend && ev_n == ev0 + 1 && ev_log[ev0] === {3'd5, 8'h02}, "standing: withdraw -> SLAVE_NAK");
        ev0 = ev_n;
        esp_cmd(6, 8'h00); wait_cycles(40);                 // withdraw with nothing standing
        check(!h_wd_pend && ev_n == ev0, "standing: withdraw of nothing -> no event");
        // fast-clock master holding the CPU + standing offer: event 7 once
        ev0 = ev_n;
        cpu_write(0, 8'hFE); cpu_write(1, 8'h83);
        wait_cycles(40);
        esp_cmd(2, 8'hFC); wait_cycles(200);
        check(ev_n == ev0 + 2 && ev_log[ev0 + 1] === {3'd7, 8'hFE} && h_sx_pend, "standing: fast-clock collision reported once, offer stands");
        esp_cmd(1, 8'hDD); wait_cycles(40);
        esp_cmd(6, 8'h00); wait_cycles(40);
        check(!h_sx_pend && ev_log[ev_n - 1] === {3'd5, 8'hDD}, "standing: collision offer withdrawn");
        esp_cmd(3, 5); wait_cycles(20);
        check(!h_standing_en, "CONFIG(5): standing offers off");

        // 7. Disable: back to the physical port.
        esp_cmd(3, 0); wait_cycles(20);
        irq0 = irq_count;
        cpu_write(0, 8'h01); cpu_write(1, 8'h81);
        wait_cycles(4 * 512 * 9);
        check(!h_net_en && irq_count == irq0 + 1 && sb == 8'hFF, "net off again: physical path");

        if (errors == 0) $display("ALL PASSED");
        else $display("%0d FAILED", errors);
        $finish;
    end
endmodule
