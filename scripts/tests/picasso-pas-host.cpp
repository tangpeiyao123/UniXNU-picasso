// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "PicassoPas.h"
// zig c++ defines NDEBUG even at -O0, so <assert.h> is compiled out; use an
// explicit check that can never be optimized away.
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed: %s (line %d)\n",#c,__LINE__); exit(1); } } while (0)
static unsigned calls,delays,mode;
static uint32_t wantPas;          // 1 = ADSP wrapper, 4 = modem ex()
static uint64_t wantAddr;         // cmd1 metadata address for the subsystem
static PicassoPasReply call(void *,uint32_t fn,uint32_t info,uint64_t pas,
                           uint64_t addr,uint64_t bytes,uint64_t session,bool first) {
    ++calls;CHECK(pas==wantPas && addr==wantAddr);
    // cmd1 uses arginfo 0x82 and no bytes; cmd2 uses arginfo 3 with the carveout size.
    if(addr==0x8bc18000) CHECK(info==0x82 && !bytes);
    else CHECK(info==3 && bytes==0xf800000ULL);
    if(mode==1 && calls<4) {
        assert(fn==(calls==1?0x42000201U:1U) && first==(calls==1));
        assert(session==(calls==1?0:0xaabbccdd00000000ULL+calls-1));
        return {1,0,0,0,0xaabbccdd00000000ULL+calls};
    }
    if(mode==1){assert(fn==1&&!first&&session==0xaabbccdd00000003ULL);return {};}
    if(mode==2){assert(first&&fn==0x42000201&&!session);return {uint32_t(-12),0,0,0,0};}
    if(mode==3)return {1,0,0,0,0x1234567800000000ULL+calls};
    if(mode==4)return {0,7,0,0,0};
    if(mode==5)return {uint32_t(-1),0,0,0,0};
    assert(fn==0x42000201 && first && !session);return {};
}
static void delay(void *,unsigned ms){assert(ms==1||ms==30);++delays;}
// modem cmd1 address is the shared metadata slot (provisional, validated on device).
static int modem_validate(unsigned command, uint64_t address, uint64_t bytes) {
    if ((command==1 && (address!=0x8bc18000 || bytes)) ||
        (command==2 && (address!=0x8c000000 || bytes!=0xf800000)) ||
        (command>=5 && (address || bytes))) return -22;
    return 0;
}
int main(){
    PicassoPasIo io={nullptr,call,delay};PicassoPasReply r={};
    // ADSP wrapper: unchanged behaviour, PAS id 1.
    wantPas=1; wantAddr=0x8bc18000;
    for(mode=0;mode<6;++mode) {
        calls=delays=0;int error=picasso_pas_execute(&io,1,0x8bc18000,0,&r);
        const int errors[]={0,0,-16,-115,-13,-5};CHECK(error==errors[mode]);
        if(mode==1)CHECK(calls==4&&delays==3);
        if(mode==2)CHECK(calls==68&&delays==68);
        if(mode==3)CHECK(calls==65&&delays==64);
    }
    calls=0;
    CHECK(picasso_pas_execute(&io,1,0,0,&r)==-22);
    CHECK(picasso_pas_execute(&io,2,0x89200000,0x2800001,&r)==-22);
    CHECK(picasso_pas_execute(&io,5,1,0,&r)==-22);
    CHECK(picasso_pas_execute(&io,7,0,0,&r)==-22);
    CHECK(!calls);

    // modem ex(): PAS id 4 on BOTH first call and interrupt-resume call.
    // mode 1 forces a resume (calls 1..3), so this is the regression that would
    // catch a pasId reverting to 1 on the second io->call.
    wantPas=4; wantAddr=0x8bc18000;
    for(mode=0;mode<6;++mode) {
        calls=delays=0;
        int error=picasso_pas_execute_ex(&io,1,0x8bc18000,0,4,modem_validate,&r);
        const int errors[]={0,0,-16,-115,-13,-5};CHECK(error==errors[mode]);
        if(mode==1)CHECK(calls==4&&delays==3);
    }
    // modem cmd2 uses the modem carveout, and passes pasId=4.
    wantAddr=0x8c000000;
    mode=0; calls=delays=0;
    CHECK(picasso_pas_execute_ex(&io,2,0x8c000000,0xf800000ULL,4,modem_validate,&r)==0);
    CHECK(calls==1);
    // adspValidate byte-for-byte equivalence: accepts ADSP triples, rejects others.
    CHECK(picasso_pas_adsp_validate(1,0x8bc18000,0)==0);
    CHECK(picasso_pas_adsp_validate(2,0x89200000,0x02800000)==0);
    CHECK(picasso_pas_adsp_validate(1,0x8c000000,0)==-22);
    CHECK(picasso_pas_adsp_validate(5,0,1)==-22);
    CHECK(picasso_pas_execute_ex(&io,1,0x8bc18000,0,4,nullptr,&r)==-22);
    puts("PASS PAS x6 ADSP + x6 modem (pasId=4 on resume) + adspValidate equivalence");
}
