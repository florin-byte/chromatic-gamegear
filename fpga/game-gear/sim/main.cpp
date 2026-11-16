#include "Vtb_gg.h"
#include "verilated.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const std::string& msg) { if(!ok) throw std::runtime_error(msg); }
int main(int argc, char** argv) try {
    Verilated::commandArgs(argc,argv);
    bool content=false, nomagic=false, noload=false;
    int a=1;
    while(a<argc && std::string(argv[a]).rfind("--",0)==0) {
        std::string opt=argv[a++];
        if(opt=="--content") content=true;
        else if(opt=="--nomagic") nomagic=true;
        else if(opt=="--noload") { nomagic=true; noload=true; }
        else throw std::runtime_error("Unknown option: "+opt);
    }
    require(argc-a>=2,"Need ROM and frame count");
    std::filesystem::path rompath=argv[a++];
    int wanted=std::stoi(argv[a++]), start_at=a<argc ? std::stoi(argv[a]) : -1;
    std::ifstream f(rompath,std::ios::binary);
    require(bool(f),"Cannot read ROM");
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});
    require(rom.size()>=1024 && rom.size()<=0x3f8000,"Unsupported ROM size");
    std::vector<uint8_t> psram(8*1024*1024,0xff);
    size_t total=32768;
    while(total<rom.size()+32768) total*=2;
    size_t base=0x100000;
    std::string title="GG SIMULATION";
    std::copy(title.begin(),title.end(),psram.begin()+base+0x134);
    psram[base+0x147]=0x19;
    unsigned code=0; for(size_t n=32768;n<total;n*=2) ++code;
    psram[base+0x148]=code; psram[base+0x149]=0;
    const std::string magic="GGCHROM1";
    if(!nomagic) std::copy(magic.begin(),magic.end(),psram.begin()+base+0x150);
    for(int i=0;i<4;i++) psram[base+0x158+i]=(rom.size()>>(8*i))&255;
    uint32_t crc=~0u;
    for(auto b:rom) { crc^=b; for(int j=0;j<8;j++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1))); }
    crc=~crc;
    for(int i=0;i<4;i++) psram[base+0x15c+i]=(crc>>(8*i))&255;
    uint8_t hc=0;
    for(size_t i=0x134;i<=0x14c;i++) hc=hc-psram[base+i]-1;
    psram[base+0x14d]=hc;
    std::copy(rom.begin(),rom.end(),psram.begin()+base+32768);
    uint16_t sum=0;
    for(size_t i=0;i<total;i++) if(i!=0x14e && i!=0x14f) sum+=psram[base+i];
    psram[base+0x14e]=sum>>8; psram[base+0x14f]=sum&255;
    auto out=std::filesystem::path("sim_gg/out")/(noload?"noload":nomagic?"nomagic":rompath.stem().string());
    std::filesystem::create_directories(out);
    Vtb_gg t;
    uint64_t cycle=0, misses=0, waits=0, first_frame=0,last_frame=0;
    unsigned max_buffer=0;
    bool busy=false; unsigned delay=0, words=0, pos=0; uint32_t address=0;
    int frames=0,lines=0,pixels=0;
    bool prev_v=true,prev_m=false, synced=false;
    std::vector<uint16_t> image;
    image.reserve(23040);
    auto save=[&] {
        char name[64]; snprintf(name,sizeof(name),"frame_%04d.ppm",frames);
        std::ofstream ppm(out/name,std::ios::binary);
        ppm<<"P6\n160 144\n255\n";
        for(auto p:image) for(int c=0;c<3;c++) { uint8_t v=(p>>(5*c))&31; ppm.put((v<<3)|(v>>2)); }
    };
    // The real toggle bridge sees a variable-latency, burst PSRAM port.
    // xclk=pclk here is intentionally slower than hardware's 75 MHz.
    const uint64_t limit=uint64_t(wanted+20)*1500000 + rom.size()*64;
    for(;cycle<limit;cycle++) {
        t.pclk=0; t.hclk=(cycle&1); t.eval();
        t.reset_n=cycle>16; t.rc_hold=cycle<300; t.rc_on=!noload && cycle>100;
        t.inval=cycle>=120;
        t.start=start_at>=0 && frames>=start_at && frames<start_at+20;
        t.x_done=0; t.x_dout_valid=0;
        if(busy) {
            if(delay) --delay;
            else if(pos<words) {
                require(address+2*pos+1<psram.size(),"PSRAM address overflow");
                t.x_dout=psram[address+2*pos] | (psram[address+2*pos+1]<<8);
                t.x_dout_valid=1; ++pos;
                delay=(pos%5==0)?2:0;
            } else {t.x_done=1;busy=false;}
        }
        t.eval();
        // Consume the hclk-domain output before its rising edge (NBA-equivalent).
        bool h_rise=!(cycle&1);
        if(h_rise) {
            bool v=t.vsync,m=t.mode&2;
            if(!t.core_reset) {
                if(v && !prev_v) {
                    if(synced && !image.empty()) {
                        if(prev_m) { require(pixels==160,"Last line width mismatch"); ++lines; pixels=0; }
                        require(lines==144 && image.size()==23040,"Frame geometry mismatch: lines="+std::to_string(lines)+" pixels="+std::to_string(image.size()));
                        ++frames;
                        if(!first_frame) first_frame=cycle;
                        last_frame=cycle;
                        if(frames%60==0 || frames==wanted) save();
                        if(frames==wanted) break;
                    }
                    synced=true; image.clear(); lines=0; pixels=0;
                }
                if(synced && !v) {
                    if(prev_m && !m) {require(pixels==160,"Line width="+std::to_string(pixels));++lines;pixels=0;}
                    if(t.valid) {require(m,"Pixel outside mode 3");image.push_back(t.pixel);++pixels;require(pixels<=160,"Too many pixels in line");}
                }
            }
            prev_v=v; prev_m=m;
        }
        t.hclk=!(cycle&1); t.pclk=1; t.eval();
        if(t.x_req) {
            require(!busy,"Overlapping PSRAM requests"); require(t.x_rnw,"Unexpected cart write");
            busy=true; address=t.x_addr; words=t.x_len/2;pos=0;
            delay=5+(misses%19); ++misses;
        }
        if(t.rom_wait) ++waits;
        max_buffer=std::max(max_buffer,unsigned(t.buffered));
        if(nomagic && t.running) throw std::runtime_error("An image without the marker was started as a game");
    }
    if(nomagic) require(frames==wanted && !t.core_reset && !t.running,"Idle test failed: frames="+std::to_string(frames));
    else require(frames==wanted,"Frame timeout: "+std::to_string(frames));
    std::set<uint16_t> colors(image.begin(),image.end());
    if(content) require(colors.size()>1,"Last frame has a single color");
    if(nomagic) require(colors.size()==1 && *colors.begin()==0,"Idle frame is not black");
    double period=frames>1?1000.0*(last_frame-first_frame)/33554432.0/(frames-1):0;
    std::ofstream report(out/"summary.txt");
    report<<"frames="<<frames<<"\nlines_per_frame="<<(frames?144:0)<<"\npixels_per_line="<<(frames?160:0)
      <<"\ncache_misses="<<misses<<"\nrom_stopped_pclk_cycles="<<waits<<"\naverage_frame_ms="<<period
      <<"\nlast_frame_colors="<<colors.size()<<"\nmaximum_buffered_pixels="<<max_buffer<<"\nnomagic="<<nomagic<<"\n";
    report.close();
    std::cout<<out<<" frames="<<frames<<" geometry="<<(frames?"144x160":"none")<<" misses="<<misses<<" stopped="<<waits<<" period_ms="<<period<<" colors="<<colors.size()<<" buffer="<<max_buffer<<"\n";
    t.final(); return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
