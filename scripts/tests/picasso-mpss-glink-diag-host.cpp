// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host regression for the controlled-write GLINK liveness diagnostic.
// CHECK (not assert) so -DNDEBUG cannot disable. Synthetic SMEM; asserts the
// EXACT write set (TX FIFO 480 head0 = {0x00010000,0}, desc478+4=8, kick
// 0x20000) and that precondition failures write NOTHING.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "WlanSmem.h"
#include "PicassoMpssGlinkDiag.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"CHECK failed: %s (line %d)\n",#c,__LINE__); std::exit(1); } } while (0)

static uint8_t g_smem[0x200000];
static uint32_t g_writes[16][2]; static unsigned g_nwrites = 0;
static uint32_t g_kicks[8]; static unsigned g_nkicks = 0;
static unsigned g_barriers = 0;

static void put32(uint32_t off,uint32_t v){
    g_smem[off]=uint8_t(v);g_smem[off+1]=uint8_t(v>>8);g_smem[off+2]=uint8_t(v>>16);g_smem[off+3]=uint8_t(v>>24);
}
static void put16(uint32_t off,uint16_t v){g_smem[off]=uint8_t(v);g_smem[off+1]=uint8_t(v>>8);}
static int rd(void*,uint32_t off,void*out,uint32_t len){
    if(off>WLAN_SMEM_SIZE||len>WLAN_SMEM_SIZE-off)return -1;
    std::memcpy(out,g_smem+off,len);return 0;
}
static int wr32(void*,uint32_t off,uint32_t v){
    if(g_nwrites<16){g_writes[g_nwrites][0]=off;g_writes[g_nwrites][1]=v;++g_nwrites;}
    put32(off,v);return 0;
}
static void bar(void*){++g_barriers;}
static int kick(void*,uint32_t v){if(g_nkicks<8)g_kicks[g_nkicks++]=v;return 0;}

// Build a (0,1) partition with desc478(32B)/tx480(16384B)/rx611(4096B).
// desc payload written with the four u32 in descCtrl; item data offsets returned.
static void partition(uint32_t descCtrl[4], uint32_t &descOff, uint32_t &txOff, uint32_t &rxOff) {
    std::memset(g_smem,0,sizeof g_smem);
    put32(0x5c,12u<<16);
    const uint32_t hdr=0x1000;
    put32(hdr,0x54525024);put16(hdr+4,0);put16(hdr+6,1);
    uint32_t at=hdr+32;
    auto add=[&](uint16_t id,uint32_t sz)->uint32_t{
        uint32_t p=at;put16(p,0xa5a5);put16(p+2,id);put32(p+4,sz);
        put16(p+8,0);put16(p+10,0);at=p+16+sz;return p+16;
    };
    descOff=add(478,32);txOff=add(480,16384);rxOff=add(611,4096);
    uint32_t unc=at-hdr;
    put32(hdr+8,unc+64);put32(hdr+12,unc);put32(hdr+16,unc+64);put32(hdr+20,64);
    const uint32_t toc=WLAN_SMEM_SIZE-4096;
    put32(toc,0x434f5424);put32(toc+4,1);put32(toc+8,1);
    uint32_t e=toc+32;put32(e,hdr);put32(e+4,unc+64);put16(e+12,0);put16(e+14,1);put32(e+16,64);
    for(unsigned i=0;i<4;++i)put32(descOff+4*i,descCtrl[i]);
    g_nwrites=0;g_nkicks=0;g_barriers=0;
}

static picasso_mpss_glink_diag_io makeIo(){
    picasso_mpss_glink_diag_io io={};
    io.smem.read=rd;io.smem_write32=wr32;io.barrier=bar;io.ipcc_kick=kick;io.context=nullptr;
    return io;
}

static void caseAllZero(){
    uint32_t c[4]={0,0,0,0},d,t,r;partition(c,d,t,r);
    auto io=makeIo();picasso_mpss_glink_diag_result out={};
    CHECK(picasso_mpss_glink_diag_write_version(&io,&out)==0);
    CHECK(out.wrote && out.error==0 && out.descId==478 && out.txId==480 && out.rxId==611);
    CHECK(out.kickValue==0x00020000u);
    CHECK(g_nkicks==1 && g_kicks[0]==0x00020000u);
    CHECK(g_barriers>=2);
    // exact write set: tx480+0=0x00010000, tx480+4=0, desc478+4=8
    CHECK(g_nwrites==3);
    CHECK(g_writes[0][0]==t+0 && g_writes[0][1]==0x00010000u);
    CHECK(g_writes[1][0]==t+4 && g_writes[1][1]==0);
    CHECK(g_writes[2][0]==d+4 && g_writes[2][1]==8);
    CHECK(out.afterHead==8);
    CHECK(!out.peerResponded); // rx head still 0 -> no peer response in fixture
}

static void casePeerResponded(){
    uint32_t c[4]={0,0,0,0},d,t,r;partition(c,d,t,r);
    // Simulate peer writing RX head into desc+12 between our kick and readback:
    // do it via a custom kick callback that mutates desc+12.
    struct Ctx{uint32_t descOff;} ctx{d+12};
    auto io=makeIo();
    // use a kick that writes desc+12=4 (peer response) via wr32
    auto kickResp=[](void*v,uint32_t val)->int{ auto*c=(Ctx*)v; wr32(nullptr,c->descOff,4); g_kicks[g_nkicks++]=val; return 0;};
    io.ipcc_kick=kickResp;io.context=&ctx;
    picasso_mpss_glink_diag_result out={};
    CHECK(picasso_mpss_glink_diag_write_version(&io,&out)==0);
    CHECK(out.peerResponded && out.afterRxHead==4);
}

static void caseNonZeroAbort(){
    uint32_t c[4]={0,16,0,0},d,t,r;partition(c,d,t,r); // TX head already 16
    auto io=makeIo();picasso_mpss_glink_diag_result out={};
    CHECK(picasso_mpss_glink_diag_write_version(&io,&out)==-1);
    CHECK(!out.wrote);CHECK(g_nwrites==0 && g_nkicks==0); // NOTHING written
}

static void caseRxNonZeroAbort(){
    uint32_t c[4]={0,0,32,0},d,t,r;partition(c,d,t,r); // RX tail non-zero
    auto io=makeIo();picasso_mpss_glink_diag_result out={};
    CHECK(picasso_mpss_glink_diag_write_version(&io,&out)==-1);
    CHECK(!out.wrote && g_nwrites==0 && g_nkicks==0);
}

// Double-read instability: peer updates desc between our two reads -> -11,
// and critically ZERO writes / ZERO kicks (we must not write into a ring
// another agent is actively changing).
static uint32_t g_flipOff = 0; static int g_flipCount = 0;
static int rdFlip(void* ctx,uint32_t off,void*out,uint32_t len){
    int rc=rd(ctx,off,out,len);
    if(rc==0 && off==g_flipOff && len>=16 && ++g_flipCount==2)
        (static_cast<uint8_t*>(out))[0]^=0x01;
    return rc;
}
static void caseUnstable(){
    uint32_t c[4]={0,0,0,0},d,t,r;partition(c,d,t,r);
    g_flipOff=d;g_flipCount=0;
    auto io=makeIo();io.smem.read=rdFlip;
    picasso_mpss_glink_diag_result out={};
    CHECK(picasso_mpss_glink_diag_write_version(&io,&out)==-11);
    CHECK(!out.wrote && g_nwrites==0 && g_nkicks==0);
    g_flipOff=0;g_flipCount=0;
}

static void caseNoPartition(){
    std::memset(g_smem,0,sizeof g_smem);put32(0x5c,12u<<16);
    const uint32_t toc=WLAN_SMEM_SIZE-4096;put32(toc,0x434f5424);put32(toc+4,1);put32(toc+8,0);
    auto io=makeIo();picasso_mpss_glink_diag_result out={};
    CHECK(picasso_mpss_glink_diag_write_version(&io,&out)==-2);
    CHECK(!out.wrote && g_nkicks==0);
}

int main(int argc,char**argv){
    const char*m=argc>1?argv[1]:"all";
    if(!std::strcmp(m,"all")||!std::strcmp(m,"zero"))caseAllZero();
    if(!std::strcmp(m,"all")||!std::strcmp(m,"peer"))casePeerResponded();
    if(!std::strcmp(m,"all")||!std::strcmp(m,"nonzero"))caseNonZeroAbort();
    if(!std::strcmp(m,"all")||!std::strcmp(m,"rxnonzero"))caseRxNonZeroAbort();
    if(!std::strcmp(m,"all")||!std::strcmp(m,"unstable"))caseUnstable();
    if(!std::strcmp(m,"all")||!std::strcmp(m,"nopart"))caseNoPartition();
    std::puts("PASS MPSS GLINK controlled-write VERSION diag (exact write set, abort-on-precondition, peer liveness)");
    return 0;
}
