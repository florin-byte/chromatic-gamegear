module link #(
  parameter CLK_DIV_SINGLE_SPEED = 9'd511,
  parameter CLK_DIV_HIGH_SPEED = 9'd15,
  parameter NET_TIMEOUT_BITS = 24
)(
  // system signals
  input clk_sys,
  input ce,
  input rst,

  input sel_sc,
  input sel_sb,
  input cpu_wr_n,
  input sc_start_in,
  input sc_speed_in,
  input sc_int_clock_in,

  input [7:0] sb_in,
  output [7:0] sc_r,

  input serial_clk_in,
  input serial_data_in,

  output serial_clk_out,
  output serial_data_out,
  output [7:0] sb,
  output serial_irq,
  output reg sc_start,
  output reg sc_int_clock,
  
  // CWL2 network link (see PROTOCOL.md). When net_en is set the physical
  // port is ignored: a master transfer is held until the network supplies the
  // peer's byte; a peer master's byte offered by the network completes an
  // armed slave (SLAVE_DONE, with the byte shifted out) or is refused
  // (SLAVE_NAK) - this port is the authority on whether the exchange happened.
  input        net_en,
  input        net_master_reply_pend,
  input  [7:0] net_master_reply_byte,
  input        net_master_reply_drop,  // MASTER_DROP: abandoned master, peer did not exchange
  // CONFIG bit 2: the ESP32 firmware handles ABANDON / MASTER_DROP / event 7.
  // Off (an older ESP32 build): no write barrier, no fast-clock stall - the
  // 18.15 behaviour - so this FPGA cannot freeze games under an old ESP32.
  input        net_barrier_en,
  // CGB double speed: ce runs at twice the rate; the safety timeout counts
  // every other tick so it stays ~4 s (the ESP32's wake hold relies on it).
  input        net_double_speed,
  // CONFIG bit 3 (18.20): standing offers. An offer the port cannot take
  // (not armed) waits here and is exchanged the moment the game arms, until
  // the ESP32 withdraws it (cmd 6: answered SLAVE_NAK if still waiting,
  // nothing if it was exchanged - its SLAVE_DONE is the answer). Pokemon
  // Gold/Silver arm for ~20 us per frame while both sides search for a
  // partner: an offer crossing the UART and Wi-Fi never found them armed.
  input        net_standing_en,
  input        net_slave_withdraw_pend,
  output reg   net_slave_withdraw_ack,
  output reg   net_master_reply_ack,
  input        net_slave_xfer_pend,
  input  [7:0] net_slave_xfer_byte,
  output reg   net_slave_xfer_ack,
  output reg       net_ev_valid,
  output reg [2:0] net_ev_type,   // 1 ARMED 2 DISARM 3 MASTER 4 SLAVE_DONE 5 SLAVE_NAK 6 ABANDON 7 SLAVE_NAK_STALLED
  output reg [7:0] net_ev_byte,
  // Stall the CPU (gb.v gates cpu_clken, like a GDMA): a held master must
  // complete before the game can see the port change under it.
  output       net_cpu_stop,

  	// savestates
  input  [63:0] SaveStateBus_Din,
  input  [9:0]  SaveStateBus_Adr, 
  input         SaveStateBus_wren,
  input         SaveStateBus_rst, 
  output [63:0] SaveStateBus_Dout
);
reg sc_speed = 1'd0;
assign sc_r = {sc_start,5'h1F,sc_speed, sc_int_clock};

// savestates
wire [16:0] SS_Link;
wire [16:0] SS_Link_BACK;

eReg_SavestateV #(0, 8, 16, 0, 64'h0000000000000000) iREG_SAVESTATE_HDMA (clk_sys, SaveStateBus_Din, SaveStateBus_Adr, SaveStateBus_wren, SaveStateBus_rst, SaveStateBus_Dout, SS_Link_BACK, SS_Link);  


reg [7:0] sb_r = 0;
assign sb = sb_r;

reg [3:0] serial_counter;

reg serial_out_r = 0;
assign serial_data_out = serial_out_r;

reg serial_clk_out_r = 1;
assign serial_clk_out = serial_clk_out_r;

reg serial_irq_r;
assign serial_irq = serial_irq_r;

reg [8:0] serial_clk_div; //8192Hz

reg [1:0]  serial_clk_in_last;

assign SS_Link_BACK[    0] = sc_start        ;
assign SS_Link_BACK[    1] = sc_int_clock    ;
assign SS_Link_BACK[ 5: 2] = serial_counter  ;
assign SS_Link_BACK[    6] = serial_out_r    ;
assign SS_Link_BACK[    7] = serial_clk_out_r;
assign SS_Link_BACK[16: 8] = serial_clk_div  ;

wire [8:0] clk_div_value = sc_speed ? CLK_DIV_HIGH_SPEED[8:0] : CLK_DIV_SINGLE_SPEED[8:0];

// CWL2: held master transfers give up after ~4 s of ce ticks. The ESP32
// decides timeouts with the peer (cancel handshake) well before; this is
// only a safety net for a dead ESP32.
reg [NET_TIMEOUT_BITS-1:0] net_wait_cnt = {NET_TIMEOUT_BITS{1'b0}};
reg net_wait_half = 1'b0;
wire net_waiting_master = net_en && sc_start && sc_int_clock;
wire net_armed_slave    = net_en && sc_start && !sc_int_clock;

// CWL2 write barrier. On hardware a master byte takes 1 ms (30 us on the CGB
// fast clock), so a game that writes SB or SC later sees it complete first. A
// held master lasts a network round trip, so a write under it (other than the
// SC = 0x81 restart above) is parked and the CPU stalled; ABANDON asks the ESP
// for the peer's verdict at once; MASTER_REPLY completes the transfer (IRQ)
// or MASTER_DROP releases it (no exchange, no IRQ), then the parked write is
// applied and the CPU resumes. Dropping the transfer at the write instead let
// the peer exchange a byte this side never received (Super Mario Bros. Deluxe
// VS); completing it with 0xFF raised the IRQ after the game's own SB write
// (Pokemon Gold/Silver read its 0x02 as the partner's, both became master).
// Fast-clock masters (SC bit 1) also stall the CPU from the start: games
// using them only wait a short delay loop (SMB Deluxe clears the reply a frame
// later).
wire cpu_wr_sc  = sel_sc && !cpu_wr_n;
wire cpu_wr_sb  = sel_sb && !cpu_wr_n;
wire sc_restart = cpu_wr_sc && sc_start_in && sc_int_clock_in && net_waiting_master;
// SC = 0x01 (stop, internal clock) under a held master: on hardware that byte
// finished long before (1 ms), so the game is only stopping a finished
// transfer, normally to start the next at once (Pokemon Gold/Silver write
// 0x01 then 0x81 every frame in LinkTransfer / Serial_ExchangeByte). The write
// is absorbed: no stall, the transfer stays held and the 0x81 that follows
// continues it. A barrier stall here put the master a Wi-Fi round trip behind
// every frame, and Gen 2's frame-timed handshakes (each byte sent for 10
// frames) slid out of step with the slave: Chromatic Silver (master) stayed at
// "Please wait" after the save while Mac Gold went into the trade room.
// Writes that change the clock source or SB still park and stall.
wire sc_absorb  = net_barrier_en && cpu_wr_sc && !sc_start_in && sc_int_clock_in && net_waiting_master;
wire wr_barrier = net_barrier_en && net_waiting_master && (cpu_wr_sb || (cpu_wr_sc && !sc_restart && !sc_absorb));
reg       pw_valid = 1'b0;            // parked CPU write
reg       pw_is_sc = 1'b0;
reg [7:0] pw_val   = 8'd0;
reg       fast_hold = 1'b0;           // held master started on the fast clock
reg       st_collide_sent = 1'b0;     // event 7 reported for the standing offer
wire      pw_apply = pw_valid && !net_waiting_master;
assign net_cpu_stop = pw_valid || (fast_hold && net_waiting_master);
// the write that takes effect this cycle: the CPU's, or the parked one
wire       do_wr_sc  = (cpu_wr_sc && !wr_barrier && !sc_absorb) || (pw_apply && pw_is_sc);
wire       do_wr_sb  = (cpu_wr_sb && !wr_barrier) || (pw_apply && !pw_is_sc);
wire [7:0] wr_val    = pw_apply ? pw_val : sb_in;
wire       wr_start  = pw_apply ? pw_val[7] : sc_start_in;
wire       wr_speed  = pw_apply ? pw_val[1] : sc_speed_in;
wire       wr_int    = pw_apply ? pw_val[0] : sc_int_clock_in;

// serial master
always @(posedge clk_sys) begin
	net_ev_valid         <= 1'b0;
	net_master_reply_ack <= 1'b0;
	net_slave_xfer_ack   <= 1'b0;
	net_slave_withdraw_ack <= 1'b0;
	if(rst) begin
		sc_start         <= SS_Link[    0]; //1'b0;
		sc_int_clock     <= SS_Link[    1]; //1'b0;
		serial_counter   <= SS_Link[ 5: 2]; //4'd0;
		serial_out_r     <= SS_Link[    6]; //1'b0;
		serial_clk_out_r <= SS_Link[    7]; //1'b0;
		serial_clk_div   <= SS_Link[16: 8]; //9'd0;

		sb_r <= sb_in;
		serial_clk_in_last <= {1'b0,serial_clk_in};
	end else if (ce) begin 
		serial_irq_r <= 1'b0;
		if (pw_apply) pw_valid <= 1'b0;
		if (!net_slave_xfer_pend || !fast_hold) st_collide_sent <= 1'b0;
		if (wr_barrier) begin                    // park the write, stall, ask for the verdict
			pw_valid     <= 1'b1;
			pw_is_sc     <= cpu_wr_sc;
			pw_val       <= sb_in;
			net_ev_valid <= 1'b1;
			net_ev_type  <= 3'd6;               // ABANDON
			net_ev_byte  <= sb_r;
		end else if (do_wr_sc) begin     //cpu write
			sc_start <= wr_start;
            sc_speed <= wr_speed;
			sc_int_clock <= wr_int;
			if (wr_start) begin                   //enable transfer
				serial_clk_div <= clk_div_value;
				serial_counter <= 4'd8;
				serial_clk_out_r <= 1'b1;
			//serial_clk_in_last <= serial_clk_in;
				serial_clk_in_last <= {1'b0,serial_clk_in};
			end
			if (net_en) begin
				if (wr_start && wr_int && net_waiting_master) begin
					// Restart of the held master (Tetris DX writes SC = 0x81
					// every frame until its first byte completes; on hardware
					// that byte took 1 ms). Keep holding the transfer already
					// in flight: a second MASTER would make the peer exchange
					// twice. No event, the safety timeout keeps counting.
				end else if (wr_start && !wr_int && net_standing_en && net_slave_xfer_pend) begin
					// The game arms with a standing offer waiting: exchange in
					// this very cycle (the game may become master again a few
					// instructions later). SLAVE_DONE replaces the ARMED hint.
					sc_start           <= 1'b0;
					sb_r               <= net_slave_xfer_byte;
					serial_irq_r       <= 1'b1;
					net_slave_xfer_ack <= 1'b1;
					net_ev_valid       <= 1'b1;
					net_ev_type        <= 3'd4;
					net_ev_byte        <= sb_r;
				end else if (wr_start) begin      // ARMED (slave) or MASTER (held)
					net_ev_valid <= 1'b1;
					net_ev_type  <= wr_int ? 3'd3 : 3'd1;
					net_ev_byte  <= sb_r;
					net_wait_cnt <= {NET_TIMEOUT_BITS{1'b0}};
					net_wait_half <= 1'b0;
					fast_hold    <= wr_int && wr_speed && net_barrier_en;
				end else if (net_armed_slave) begin  // armed slave cancelled
					net_ev_valid <= 1'b1;
					net_ev_type  <= 3'd2;
					net_ev_byte  <= sb_r;
				end
			end
		end else if (do_wr_sb) begin
			sb_r <= wr_val;
			if (net_armed_slave) begin              // re-arm with the new byte
				net_ev_valid <= 1'b1;
				net_ev_type  <= 3'd1;
				net_ev_byte  <= wr_val;
			end
		end else if (net_en && net_slave_xfer_pend && (net_armed_slave || !net_standing_en || net_slave_withdraw_pend)) begin
			net_slave_xfer_ack <= 1'b1;
			net_ev_valid       <= 1'b1;
			net_ev_byte        <= sb_r;             // the byte shifted out
			if (net_armed_slave) begin
				net_ev_type  <= 3'd4;               // SLAVE_DONE
				sb_r         <= net_slave_xfer_byte;
				serial_irq_r <= 1'b1;
				sc_start     <= 1'b0;
			end else if (net_standing_en) begin
				net_ev_type  <= 3'd5;               // standing offer withdrawn
				net_slave_withdraw_ack <= 1'b1;
			end else begin
				// SLAVE_NAK: not armed. 7 = refused while holding a master with
				// the CPU stalled: both sides are masters and neither can arm
				// (the ESP asks the verdict at once instead of the timeout).
				net_ev_type  <= net_cpu_stop ? 3'd7 : 3'd5;
			end
		end else if (net_en && net_slave_withdraw_pend) begin
			// Nothing standing: it was exchanged (its SLAVE_DONE answered) or
			// never arrived. No event.
			net_slave_withdraw_ack <= 1'b1;
		end else if (net_en && net_standing_en && net_slave_xfer_pend && fast_hold && net_waiting_master && !st_collide_sent) begin
			// A standing offer while this side holds a fast-clock master with the
			// CPU stalled: neither side can ever arm (master collision), report it
			// once; the offer keeps standing until withdrawn.
			st_collide_sent <= 1'b1;
			net_ev_valid    <= 1'b1;
			net_ev_type     <= 3'd7;
			net_ev_byte     <= sb_r;
		end else if (net_waiting_master) begin
			net_wait_half <= ~net_wait_half;
			if (!net_double_speed || net_wait_half) net_wait_cnt <= net_wait_cnt + 1'b1;
			if (net_master_reply_pend) begin
				net_master_reply_ack <= 1'b1;
				sc_start             <= 1'b0;
				fast_hold            <= 1'b0;
				if (!net_master_reply_drop) begin   // MASTER_REPLY: complete
					sb_r         <= net_master_reply_byte;
					serial_irq_r <= 1'b1;
				end                                 // MASTER_DROP: just release
			end else if (&net_wait_cnt) begin
				sb_r         <= 8'hFF;                // no partner
				serial_irq_r <= 1'b1;
				sc_start     <= 1'b0;
				fast_hold    <= 1'b0;
			end
		end else if (sc_start && !net_en) begin // serial transfer
		if (sc_int_clock) begin   // internal clock
			serial_clk_div <= serial_clk_div - 9'd1;
	
			if (serial_counter != 0) begin
				if (serial_clk_div == clk_div_value[8:1]+1) begin
				serial_clk_out_r <= ~serial_clk_out_r;
				serial_out_r <= sb[7];
				end else if (!serial_clk_div) begin
				sb_r <= {sb[6:0], serial_data_in};
						serial_clk_out_r <= ~serial_clk_out;
				serial_counter <= serial_counter - 1'd1;
				serial_clk_div <= clk_div_value;
				end
			end else begin
				serial_irq_r <= 1'b1;
				sc_start <= 1'b0;
				serial_clk_div <= clk_div_value;
				serial_counter <= 4'd8;
			end
		end else begin  // external clock
				serial_clk_in_last[0] <= serial_clk_in;
				serial_clk_in_last[1] <=  serial_clk_in_last[0] ;
				if (serial_clk_in_last[1] != serial_clk_in_last[0]) begin
						if (serial_clk_in_last[1] == 0) begin
							serial_out_r <= sb[7];                 // send out bit to linked gb
							serial_counter <= serial_counter - 1'd1;
						end else begin                               // posedge external clock
						sb_r <= {sb[6:0], serial_data_in};        // capture bit into sb
						if (serial_counter == 0) begin            // read in 8 bits
							serial_irq_r <= 1'b1;                  // set interrupt, reset counter/sc_start for next read
							sc_start <= 1'b0;
							serial_counter <= 4'd8;
						end
					end
				end
			end
		end
		// Drop a master reply that no longer matches a transfer (safety
		// timeout) so it cannot complete a later one.
		if (net_master_reply_pend && !net_waiting_master) net_master_reply_ack <= 1'b1;
	end
end

endmodule
// vim:sw=3:ts=3:et:
