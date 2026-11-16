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
    unsigned shiftn=0, shiftph=0;   // CG-117: --shift=N[,P]: the read bursts with index % N == P come one word late
    int gphase=0; uint64_t warm=0; bool abtn=false, menu=false; long aheld=-1;   // CG-117: a start from the menu (A held, OSD open)
    int a=1;
    while(a<argc && std::string(argv[a]).rfind("--",0)==0) {
        std::string opt=argv[a++];
        if(opt=="--content") content=true;
        else if(opt=="--nomagic") nomagic=true;
        else if(opt=="--gphase1") gphase=1;
        else if(opt=="--warm") warm=3000000;
        else if(opt=="--abtn") abtn=true;
        else if(opt.rfind("--aheld=",0)==0) { aheld=std::stol(opt.substr(8)); menu=true; warm=3000000; }   // A held N/10 frames after the start from the menu
        else if(opt.rfind("--shift=",0)==0) { auto v=opt.substr(8); auto c=v.find(","); shiftn=std::stoul(v.substr(0,c)); shiftph=c==std::string::npos ? shiftn-1 : std::stoul(v.substr(c+1)); }
        else if(opt=="--menu") { menu=true; warm=3000000; }
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
    auto out=std::filesystem::path("sim_gg/out")/((noload?std::string("noload"):nomagic?std::string("nomagic"):rompath.stem().string())+(gphase?"-g1":"")+(abtn?"-abtn":"")+(aheld>=0?"-aheld"+std::to_string(aheld):"")+(menu?"-menu":warm?"-warm":"")+(shiftn?"-shift"+std::to_string(shiftn)+"_"+std::to_string(shiftph):""));
    std::filesystem::create_directories(out);
    Vtb_gg t;
    uint64_t cycle=0, misses=0, waits=0, first_frame=0,last_frame=0;
    std::set<uint64_t> dbgset;
    std::vector<uint64_t> missv;   // CG-106: (frame << 32) | PSRAM address of every cache fill
    unsigned max_buffer=0;
    uint64_t shifted_bursts=0, ram_writes=0; bool late=false;
    bool busy=false; unsigned delay=0, words=0, pos=0; uint32_t address=0;
    int frames=0,lines=0,pixels=0;
    // --warm: the frame cut by the load (hold in the middle of an idle frame) is counted, not fatal.
    long transition_errors=0;
    auto softreq=[&](bool ok,const std::string& msg) { if(ok) return; if(warm && cycle<warm+4000000) { ++transition_errors; return; } throw std::runtime_error(msg); };
    // Panel side (gclk): rows rebuilt from LCD_DE / LCD_DB, the row period, the frame starts the panel saw.
    std::vector<std::vector<uint8_t>> prows; std::vector<uint8_t> prow;
    bool prev_hs=true; long row_gclk=0; int pvs=0, newlines=0; bool prev_nl=false;
    long panel_missed=0, panel_extra=0, bad_period=0, bad_de=0, bad_rows=0, bad_image=0, bad_addr=0, compared=0;
    int lock_shift=-1, lock_perm=-1;
    static const int perms[6][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    auto row_matches=[&](const std::vector<uint8_t>& got,const uint16_t* want,int shift,int perm) {
        for(int x=0;x<160;x++) { uint8_t c[3]={uint8_t((want[x]&31)<<1),uint8_t(((want[x]>>5)&31)<<1),uint8_t(((want[x]>>10)&31)<<1)};
            for(int k=0;k<3;k++) if(got[shift+3*x+k]!=c[perms[perm][k]]) return false; }
        return true; };
    auto compare_panel=[&](const std::vector<uint16_t>& image) {
        if(prows.size()!=144) { ++bad_rows; return; }
        // lock the position on a frame with content only (a single-colour frame matches at any shift)
        if(lock_shift<0) { bool flat=true; for(size_t i=1;i<image.size() && flat;i++) flat=image[i]==image[0];
            if(flat) return;
            for(int s=0;s+480<=720 && lock_shift<0;s++) for(int p=0;p<6;p++) { bool ok=true;
              for(int y=0;y<144 && ok;y++) ok=row_matches(prows[y],&image[160*y],s,p);
              if(ok) { lock_shift=s; lock_perm=p; break; } }
            if(lock_shift<0) ++bad_image; return; }
        for(int y=0;y<144;y++) if(!row_matches(prows[y],&image[160*y],lock_shift,lock_perm)) { ++bad_image; return; } };
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
        const bool g_rise=((cycle+(gphase?2:0))&3)==0;
        // --warm: as on the console, the core first runs idle (no game), then the firmware holds, loads and releases.
        t.reset_n=cycle>16; t.rc_hold=cycle>=warm && cycle<warm+300; t.rc_on=!noload && cycle>warm+100;
        t.inval=cycle>=warm+120;
        t.btn_a=(abtn && frames<40) || (aheld>=0 && cycle<warm+300+uint64_t(aheld)*56000); t.menu_closed=!(menu && cycle<warm+300+70000);
        t.start=start_at>=0 && frames>=start_at && frames<start_at+20;
        if((cycle&0xfffff)==0) dbgset.insert(t.gg_dbg);   // gg14diag: the pages `rc dbg` would show
        t.x_done=0; t.x_dout_valid=0;
        if(busy) {
            if(delay) --delay;
            else if(pos<words) {
                require(address+2*pos+1<psram.size(),"PSRAM address overflow");
                { size_t ra=address+2*pos-(late?2:0);   // late: position k holds word k-1 (measured on the unit)
                  t.x_dout=psram[ra] | (psram[ra+1]<<8); }
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
                    if(synced && frames>=3 && image.size()==23040) {
                        if(pvs==0) ++panel_missed; else if(pvs>1) ++panel_extra;
                        if(newlines!=144) ++bad_addr;
                        compare_panel(image); ++compared;
                    }
                    pvs=0; newlines=0; prows.clear();
                    if(synced && !image.empty()) {
                        if(prev_m) { softreq(pixels==160,"Last line width mismatch"); ++lines; pixels=0; }
                        softreq(lines==144 && image.size()==23040,"Frame geometry mismatch: lines="+std::to_string(lines)+" pixels="+std::to_string(image.size()));
                        ++frames;
                        if(!first_frame) first_frame=cycle;
                        last_frame=cycle;
                        if(frames%60==0 || frames==wanted) save();
                        if(frames==wanted) break;
                    }
                    synced=true; image.clear(); lines=0; pixels=0;
                }
                if(synced) {
                    if(prev_m && !m) {softreq(pixels==160,"Line width="+std::to_string(pixels));++lines;pixels=0;}
                    if(t.valid) {softreq(m,"Pixel outside mode 3");image.push_back(t.pixel);++pixels;softreq(pixels<=160,"Too many pixels in line");}
                }
            }
            prev_v=v; prev_m=m;
            // vid_system_top: the frame buffer address after the end of row k must be 0x10000 + 320 * k.
            if(prev_nl && !t.core_reset && synced) { if(t.gb_addr!=0x10000u+320u*unsigned(newlines)) ++bad_addr; ++newlines; }
            prev_nl=t.gb_newline;
        }
        if(g_rise && t.lcd_init_done) {   // the values gclk sees at this edge
            if(t.p_vs) ++pvs;
            ++row_gclk;
            if(prev_hs && !t.lcd_hs) {
                if(!prow.empty()) { if(prow.size()!=720) { ++bad_de; std::cerr<<"BAD_DE size="<<prow.size()<<" frames="<<frames<<" cycle="<<cycle<<" rows_in_frame="<<prows.size()<<std::endl; } else prows.push_back(prow);
                    if(row_gclk!=912 && prows.size()>1) ++bad_period; }
                prow.clear(); row_gclk=0;
            }
            if(t.lcd_de) prow.push_back(t.lcd_db);
            prev_hs=t.lcd_hs;
        }
        t.gclk=((cycle+(gphase?2:0))&3)<2;
        t.hclk=!(cycle&1); t.pclk=1; t.eval();
        if(t.x_req) {
            require(!busy,"Overlapping PSRAM requests");
            busy=true; address=t.x_addr; words=t.x_len/2;pos=0;
            if(!t.x_rnw) {   // CG-107: a save RAM word written (one word, as ramcart sends it)
                require(t.x_len==2 && address>=0x040000 && address<0x048000,"Cart write outside the save RAM");
                psram[address]=t.x_din&255; psram[address+1]=t.x_din>>8; words=0; ++ram_writes;
            }
            late=shiftn && (misses%shiftn)==shiftph; shifted_bursts+=late;
            delay=5+(misses%19); ++misses; missv.push_back((uint64_t(frames)<<32)|t.x_addr);
        }
        if(t.rom_wait) ++waits;
        max_buffer=std::max(max_buffer,unsigned(t.buffered));
        if(nomagic && t.running) throw std::runtime_error("An image without the marker was started as a game");
    }
    if(nomagic) require(frames==wanted && !t.core_reset && !t.running,"Idle test failed: frames="+std::to_string(frames));
    else require(frames==wanted,"Frame timeout: "+std::to_string(frames));
    std::set<uint16_t> colors(image.begin(),image.end());
    if(content) require(colors.size()>1,"Last frame has a single color");
    const bool panel_ok=compared>0 && !panel_missed && !panel_extra && !bad_de && !bad_rows && !bad_image && !bad_addr && (waits ? true : !bad_period);
    if(nomagic) require(colors.size()==1 && *colors.begin()==0,"Idle frame is not black");
    double period=frames>1?1000.0*(last_frame-first_frame)/33554432.0/(frames-1):0;
    std::ofstream report(out/"summary.txt");
    report<<"frames="<<frames<<"\nlines_per_frame="<<(frames?144:0)<<"\npixels_per_line="<<(frames?160:0)
      <<"\ncache_misses="<<misses<<"\nrom_stopped_pclk_cycles="<<waits<<"\naverage_frame_ms="<<period
      <<"\nlast_frame_colors="<<colors.size()<<"\nmaximum_buffered_pixels="<<max_buffer<<"\nnomagic="<<nomagic
      <<"\npanel_frames_compared="<<compared<<"\npanel_frame_starts_missed="<<panel_missed
      <<"\npanel_frame_starts_extra="<<panel_extra<<"\npanel_rows_not_912_gclk="<<bad_period
      <<"\npanel_rows_de_not_720="<<bad_de<<"\npanel_frames_not_144_rows="<<bad_rows
      <<"\npanel_frames_image_mismatch="<<bad_image<<"\nframe_buffer_address_errors="<<bad_addr
      <<"\npanel_rows_waited="<<(t.panel_dbg&0xffff)<<"\npanel_shift="<<lock_shift<<"\npanel_perm="<<lock_perm
      <<"\ngclk_phase="<<gphase<<"\npanel_ok="<<panel_ok<<"\n";
    report.close();
    { std::ofstream df(out/"dbg.txt"); for(auto v: dbgset) df<<std::hex<<((v>>32)&0xffff)<<" "<<(v&0xffffffffu)<<std::dec<<std::endl; }
    { std::ofstream sf(out/"save.bin",std::ios::binary); sf.write((const char*)&psram[0x040000],32768); }
    { std::ofstream mf(out/"misses.txt"); for(auto v: missv) mf<<(v>>32)<<" "<<std::hex<<(v&0xffffffffu)<<std::dec<<std::endl; }
    std::cout<<out<<" frames="<<frames<<" geometry="<<(frames?"144x160":"none")<<" misses="<<misses<<" stopped="<<waits<<" period_ms="<<period<<" colors="<<colors.size()<<" shifted_bursts="<<shifted_bursts<<" ram_writes="<<ram_writes<<" buffer="<<max_buffer<<"\n";
    std::cout<<"panel: compared="<<compared<<" missed="<<panel_missed<<" extra="<<panel_extra<<" rows!=912="<<bad_period
      <<" de!=720="<<bad_de<<" frames!=144rows="<<bad_rows<<" image_mismatch="<<bad_image<<" addr_errors="<<bad_addr
      <<" gphase="<<gphase<<" ok="<<panel_ok<<"\n";
    t.final(); return panel_ok ? 0 : 3;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
