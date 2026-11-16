// boot_rom_alt.v: the ALTERNATE CGB boot image (lane cartflash1, DRAFT, not in the build
// unless gb.v's boot_alt port is driven). The ESP32 selects it with link cmd 10 before a
// cart power cycle (cmd 5) for a "Send to my Chromatic" cart job: the image is the stock
// CGB boot ROM plus the netboot stub (cartflash sm83/cf_netboot.inc), which fetches the
// cart writer into WRAM over the network window and never runs code from the cartridge.
// Same shape and read timing as dpram_difV port A (2 KiB, registered output).
module boot_rom_alt (
    input             clock,
    input      [11:0] address_a,
    output reg  [7:0] q_a
);
    reg [7:0] rom [2047:0];
    initial $readmemh("BootROMs/cf_netboot.mif.vmem", rom);
    always @(posedge clock) q_a <= rom[address_a[10:0]];
endmodule
