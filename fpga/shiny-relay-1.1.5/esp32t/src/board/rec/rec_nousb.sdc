//Copyright (C)2014-2026 GOWIN Semiconductor Corporation.
//All rights reserved.
//File Title: Timing Constraints file
//Tool Version: V1.9.9.03 
//Created Time: 2026-07-08 13:22:26
create_clock -name sclk -period 25 -waveform {0 12.5} [get_ports {QSPI_CLK}]
create_clock -name exclk -period 29.802 -waveform {0 14.901} [get_ports {CLK_FPGA}]
create_generated_clock -name xclk2 -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 1 -multiply_by 4 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT0}]
create_generated_clock -name pclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 1 -multiply_by 1 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT1}]
create_generated_clock -name gclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 4 -multiply_by 1 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT3}]
create_generated_clock -name xclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 1 -multiply_by 2 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT4}]
create_generated_clock -name hclk -source [get_ports {CLK_FPGA}] -master_clock exclk -divide_by 2 -multiply_by 1 [get_pins {u_Gowin_PLL/PLLA_inst/CLKOUT2}]
// Audio serialization uses gclk with an alternate-cycle enable.
// AUD_BCLK is an output signal; there is no internal audio_bclk clock domain.
// pclk, hclk and gclk share the main PLL reference.
// Preserve their relationships so transfers between them are timed.
// Historical exceptions, disabled in v18.11: these related domains contain
// direct transfers that require setup/hold analysis (including cartridge-to-CPU).
// Keep disabled; any future exception needs a verified crossing/enable contract.
// The original file contained the same three declarations twice.
#set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {hclk}]
#set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {gclk}]
#set_clock_groups -asynchronous -group [get_clocks {hclk}] -group [get_clocks {gclk}]
#set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {hclk}]
#set_clock_groups -asynchronous -group [get_clocks {pclk}] -group [get_clocks {gclk}]
#set_clock_groups -asynchronous -group [get_clocks {hclk}] -group [get_clocks {gclk}]
# USB and video use independent oscillators. uvc_capture transfers ownership
# through two-flop synchronizers; each RGB line bank is held until acknowledged.
# Keep all relationships within the main PLL timed (see above).
# Cartridge read contract: data is stable for two pclk periods before CPU
# consumption, so CART_DIN_r1 already captured the same byte on the preceding
# falling edge. This is an assumed bus guarantee, not enforced by these commands.
# Use the launch clock: setup 2 adds one pclk period (29.802 ns), giving
# 44.703 ns nominal falling-pclk to rising-hclk setup time. Hold 1 restores
# the original hold relationship. Limit the exception to cartridge -> CPU;
# DMA destinations and other pclk/hclk transfers retain their normal checks.
# List nested CPU instances explicitly: V1.9.12.03 matched cpu/* only at
# that level in the routed exception report. These are HDL instance names,
# independent of source filenames. Check Regs endpoints in report_exceptions.
#set_false_path -from [get_pins {BTN_MENU_filtered_s0/F}] -to [get_regs {u_system_monitor/btnMenu_r1_s0}] 
set_max_delay -from [get_ports {CART_D[*]}] -to [get_clocks {hclk}] 13
set_max_delay -from [get_clocks {hclk}] -to [get_ports {CART_A[*]}] 14
set_max_delay -from [get_clocks {hclk}] -to [get_ports {CART_WR}] 14
set_max_delay -from [get_clocks {hclk}] -to [get_ports {CART_RD}] 14
set_max_delay -from [get_clocks {hclk}] -to [get_ports {CART_CS}] 14
set_max_delay -from [get_clocks {hclk}] -to [get_ports {LINK_SD}] 14
set_max_delay -from [get_clocks {hclk}] -to [get_ports {CART_D[*]}] 14
