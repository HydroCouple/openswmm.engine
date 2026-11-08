/* SPDX-License-Identifier: Apache-2.0 */
#include <openswmm/engine/openswmm_passive_exchange.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_split.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void check(int yes,const char* text,int line) {
    if(!yes) {fprintf(stderr,"line %d: %s; facade: %s; checkpoint: %s\n",line,text,swmm_passive_exchange_last_error(),swmm_checkpoint_last_error_msg());exit(1);}
}
#define CHECK(x) check(!!(x),#x,__LINE__)
static SWMM_PassiveStatusV1 status(SWMM_Engine e) {
    SWMM_PassiveStatusV1 s;CHECK(swmm_passive_exchange_status_v1(e,sizeof s,&s)==SWMM_OK);return s;
}
static SWMM_PassivePoolV1 pool(SWMM_Engine e) {
    SWMM_PassivePoolV1 p;CHECK(swmm_passive_exchange_pool_v1(e,0,sizeof p,&p)==SWMM_OK);return p;
}
static SWMM_Engine start(const char* inp,uint64_t capacity,int preflight,int checkpoint) {
    SWMM_Engine e=swmm_engine_create();CHECK(e);
    CHECK(swmm_engine_open(e,inp,NULL,NULL,NULL)==SWMM_OK);
    CHECK(swmm_engine_initialize(e)==SWMM_OK);CHECK(swmm_engine_start(e,0)==SWMM_OK);
    CHECK(swmm_passive_exchange_enable(e,capacity,preflight)==SWMM_OK);
    if(checkpoint)CHECK(swmm_passive_exchange_enable_checkpoint(e)==SWMM_OK);
    return e;
}
static void close_engine(SWMM_Engine e) {CHECK(swmm_engine_close(e)==SWMM_OK);swmm_engine_destroy(e);}
static void equal_files(const char* a,const char* b) {
    FILE* x=fopen(a,"rb");FILE* y=fopen(b,"rb");CHECK(x&&y);
    for(;;) {int u=fgetc(x),v=fgetc(y);CHECK(u==v);if(u==EOF)break;}
    fclose(x);fclose(y);
}
static void save(SWMM_Engine e,const char* dir,const char* prefix,int step,char* path) {
    CHECK(snprintf(path,4096,"%s/%s-%d.bin",dir,prefix,step)>0);CHECK(swmm_checkpoint_save(e,path)==SWMM_OK);
}
static void amount(FILE* f,const SWMM_PassiveAmountV1* a) {
    CHECK(a->count<=64);fprintf(f," %" PRIu64,a->count);
    for(uint64_t i=0;i<a->count;++i)fprintf(f," %a",a->components[i]);
    for(uint64_t i=a->count;i<64;++i)CHECK(a->components[i]==0);
}
static void observe(SWMM_Engine e,FILE* f,int step,int units) {
    SWMM_PassiveStatusV1 s=status(e);SWMM_PassivePoolV1 p=pool(e);
    CHECK(!s.failed&&!s.window_open&&!s.delivery_frozen&&!s.unsealed_leg_count);CHECK(s.units==units&&p.units==units);
    CHECK(s.pollutant==0&&strcmp(swmm_pollutant_id(e,s.pollutant),"Dye")==0);
    fprintf(f,"P %d %a %a %a %a",step,p.settled_volume_ft3,p.settled_concentration,p.settled_physical_amount,p.hydraulic_volume_ft3);
    amount(f,&p.roundoff);amount(f,&p.pending_drain);amount(f,&p.pending_spill);fprintf(f,"\n");
    uint64_t global=0;
    for(uint64_t i=0;i<s.receipt_count;++i) {
        SWMM_PassiveReceiptV1 r;CHECK(swmm_passive_exchange_receipt_v1(e,i,sizeof r,&r)==SWMM_OK);CHECK(r.first_leg==global);
        for(int remaining=0;remaining<=1;++remaining)for(uint64_t j=0;j<r.row_count;++j) {
            SWMM_PassiveRowV1 row;CHECK(swmm_passive_exchange_row_v1(e,i,remaining,j,sizeof row,&row)==SWMM_OK);CHECK(row.units==units&&row.node==0);
            fprintf(f,"R %d %" PRIu64 " %d %" PRIu64,step,r.id,remaining,row.point);
            amount(f,&row.drain_water);amount(f,&row.spill_water);amount(f,&row.drain_amount);amount(f,&row.spill_amount);fprintf(f,"\n");
        }
        for(uint64_t j=0;j<r.leg_count;++j) {
            SWMM_PassiveLegV1 l;CHECK(swmm_passive_exchange_leg_v1(e,global++,sizeof l,&l)==SWMM_OK);CHECK(l.sealed&&l.applied);
            fprintf(f,"L %d %" PRIu64 " %" PRIu64 " %" PRIu64 " %d %a %a %a %a %a\n",step,r.id,j,l.point,l.drain,l.water_ft3,l.native_amount,l.remaining_water_ft3,l.remaining_native_amount,l.water_m3);
        }
    }
    CHECK(global==s.leg_count);
}
static void untouched(const void* value,size_t size,int byte) {
    const unsigned char* p=value;for(size_t i=0;i<size;++i)CHECK(p[i]==byte);
}
static void guards(int off) {
    SWMM_Engine e=swmm_engine_create();CHECK(e);SWMM_PassiveStatusV1 s,before;
    memset(&s,0xa5,sizeof s);memcpy(&before,&s,sizeof s);
    CHECK(swmm_passive_exchange_status_v1(NULL,sizeof s,&s)==SWMM_ERR_BADHANDLE);CHECK(memcmp(&s,&before,sizeof s)==0);
    CHECK(swmm_passive_exchange_status_v1(e,sizeof s-1,&s)==SWMM_ERR_BADPARAM);CHECK(memcmp(&s,&before,sizeof s)==0);
    CHECK(swmm_passive_exchange_enable(e,UINT64_MAX,0)==SWMM_ERR_BADPARAM);
    CHECK(swmm_passive_exchange_enable(e,0,0)==SWMM_ERR_BADPARAM);CHECK(swmm_passive_exchange_enable(e,64,2)==SWMM_ERR_BADPARAM);
    CHECK(swmm_passive_exchange_enable(e,64,0)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));
    CHECK(swmm_passive_exchange_status_v1(e,sizeof s,&s)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));CHECK(memcmp(&s,&before,sizeof s)==0);
    if(off) {
        SWMM_PassivePoolV1 p;SWMM_PassiveReceiptV1 r;SWMM_PassiveRowV1 row;SWMM_PassiveLegV1 l;
        memset(&p,0xa5,sizeof p);memset(&r,0xa5,sizeof r);memset(&row,0xa5,sizeof row);memset(&l,0xa5,sizeof l);
        CHECK(swmm_passive_exchange_pool_v1(e,0,sizeof p,&p)==SWMM_ERR_PLUGIN);untouched(&p,sizeof p,0xa5);
        CHECK(swmm_passive_exchange_receipt_v1(e,0,sizeof r,&r)==SWMM_ERR_PLUGIN);untouched(&r,sizeof r,0xa5);
        CHECK(swmm_passive_exchange_row_v1(e,0,0,0,sizeof row,&row)==SWMM_ERR_PLUGIN);untouched(&row,sizeof row,0xa5);
        CHECK(swmm_passive_exchange_leg_v1(e,0,sizeof l,&l)==SWMM_ERR_PLUGIN);untouched(&l,sizeof l,0xa5);
        CHECK(swmm_passive_exchange_attempt_v1(e,sizeof l,&l)==SWMM_ERR_PLUGIN);untouched(&l,sizeof l,0xa5);
        CHECK(swmm_passive_exchange_enable_checkpoint(e)==SWMM_ERR_PLUGIN);
    }
    swmm_engine_destroy(e);
}
static void callback(void* e,double date,double dt,void* user) {
    (void)date;(void)dt;SWMM_PassiveStatusV1 s,before;memset(&s,0x5a,sizeof s);memcpy(&before,&s,sizeof s);
    CHECK(swmm_passive_exchange_status_v1(e,sizeof s,&s)==SWMM_ERR_LIFECYCLE);CHECK(memcmp(&s,&before,sizeof s)==0);++*(int*)user;
}
static void failure_case(const char* inp,uint64_t capacity,int arithmetic) {
    SWMM_Engine e=start(inp,capacity,0,0);double elapsed=0;
    if(arithmetic)for(int i=0;i<4;++i)CHECK(swmm_engine_step(e,&elapsed)==SWMM_OK);
    SWMM_PassivePoolV1 before=pool(e);int rc=swmm_engine_step(e,&elapsed);
    if(!arithmetic)for(int i=1;i<4&&rc==SWMM_OK;++i)rc=swmm_engine_step(e,&elapsed);
    CHECK(rc!=SWMM_OK);SWMM_PassiveStatusV1 s=status(e);CHECK(s.failed);
    SWMM_PassivePoolV1 after=pool(e);CHECK(before.settled_volume_ft3==after.settled_volume_ft3&&before.settled_concentration==after.settled_concentration);
    if(arithmetic)CHECK(after.hydraulic_volume_ft3!=after.settled_volume_ft3);
    else {SWMM_PassiveLegV1 l,a;CHECK(s.window_open&&s.receipt_count==0&&s.unsealed_leg_count==1);CHECK(swmm_passive_exchange_leg_v1(e,0,sizeof l,&l)==SWMM_OK);CHECK(l.applied&&!l.sealed);CHECK(swmm_passive_exchange_attempt_v1(e,sizeof a,&a)==SWMM_OK);CHECK(!a.applied&&a.water_ft3>0);}
    char root[4096];snprintf(root,sizeof root,"%s",swmm_get_last_error_msg(e));
    CHECK(swmm_passive_exchange_pool_v1(e,-1,sizeof after,&after)==SWMM_ERR_BADINDEX);CHECK(strcmp(root,swmm_get_last_error_msg(e))==0);
    CHECK(swmm_engine_end(e)!=SWMM_OK);CHECK(status(e).failed);CHECK(strcmp(root,swmm_get_last_error_msg(e))==0);
    close_engine(e);
}
int main(int argc,char** argv) {
    if(argc==2&&!strcmp(argv[1],"--guards")) {guards(0);return 0;}
    if(argc==2&&!strcmp(argv[1],"--off")) {guards(1);return 0;}
    CHECK(argc==6);const char* input=argv[1];const char* dir=argv[2];int units=atoi(argv[3]);const char* refusal=argv[4];int sign=atoi(argv[5]);
    SWMM_Engine e=start(input,64,0,1),restored=NULL;SWMM_PassivePoolV1 original=pool(e),sentinel=original;
    CHECK(swmm_passive_exchange_pool_v1(e,-1,sizeof sentinel,&sentinel)==SWMM_ERR_BADINDEX);CHECK(memcmp(&sentinel,&original,sizeof sentinel)==0);
    CHECK(swmm_passive_exchange_enable(e,64,0)==SWMM_ERR_LIFECYCLE);
    CHECK(swmm_passive_exchange_enable_checkpoint(e)==SWMM_ERR_LIFECYCLE);
    SWMM_PassiveRowV1 badrow;SWMM_PassiveReceiptV1 badreceipt;SWMM_PassiveLegV1 badleg;
    memset(&badrow,0x5a,sizeof badrow);memset(&badreceipt,0x5a,sizeof badreceipt);memset(&badleg,0x5a,sizeof badleg);
    CHECK(swmm_passive_exchange_row_v1(e,UINT64_MAX,0,0,sizeof badrow,&badrow)==SWMM_ERR_BADINDEX);untouched(&badrow,sizeof badrow,0x5a);
    CHECK(swmm_passive_exchange_row_v1(e,0,2,0,sizeof badrow,&badrow)==SWMM_ERR_BADPARAM);untouched(&badrow,sizeof badrow,0x5a);
    CHECK(swmm_passive_exchange_receipt_v1(e,UINT64_MAX,sizeof badreceipt,&badreceipt)==SWMM_ERR_BADINDEX);untouched(&badreceipt,sizeof badreceipt,0x5a);
    CHECK(swmm_passive_exchange_leg_v1(e,UINT64_MAX,sizeof badleg,&badleg)==SWMM_ERR_BADINDEX);untouched(&badleg,sizeof badleg,0x5a);
    char path[4096],copy[4096],trace[4096];snprintf(trace,sizeof trace,"%s/trace.txt",dir);FILE* f=fopen(trace,"w");CHECK(f);
    double elapsed=0;for(int step=0;step<=8;++step) {
        if(step) {CHECK(swmm_engine_step(e,&elapsed)==SWMM_OK);if(restored)CHECK(swmm_engine_step(restored,&elapsed)==SWMM_OK);}
        observe(e,f,step,units);save(e,dir,"source",step,path);
        if(restored) {save(restored,dir,"restored",step,copy);equal_files(path,copy);}
        if(step==6) {
            restored=start(input,64,0,1);save(restored,dir,"fresh-before",0,copy);
            SWMM_CheckpointInspection info=NULL;CHECK(swmm_checkpoint_inspect(restored,path,&info)==SWMM_OK);CHECK(info);swmm_checkpoint_inspection_destroy(info);
            char after[4096];save(restored,dir,"fresh-after",0,after);equal_files(copy,after);
            CHECK(swmm_checkpoint_restore(restored,path)==SWMM_OK);save(restored,dir,"restored",step,copy);equal_files(path,copy);
        }
    }
    fclose(f);CHECK(sign==1||sign==-1);CHECK(swmm_engine_end(e)==SWMM_OK);CHECK(swmm_engine_end(restored)==SWMM_OK);
    CHECK(!status(e).failed&&!status(restored).failed);SWMM_PassivePoolV1 ended=pool(e),ended_copy=pool(restored);
    CHECK(ended.settled_volume_ft3==ended_copy.settled_volume_ft3&&ended.settled_concentration==ended_copy.settled_concentration);
    snprintf(copy,sizeof copy,"%s/not-an-ended-checkpoint.bin",dir);CHECK(swmm_checkpoint_save(e,copy)==SWMM_ERR_LIFECYCLE);
    close_engine(restored);close_engine(e);
    /* Ordinary water split still refuses this material model before selection. */
    e=swmm_engine_create();CHECK(swmm_engine_open(e,input,NULL,NULL,NULL)==SWMM_OK);CHECK(swmm_split_configure(e,SWMM_SPLIT_NETWORK)!=SWMM_OK);close_engine(e);
    e=start(input,4096,1,1);save(e,dir,"budget-before",0,path);elapsed=12345;
    CHECK(swmm_engine_step(e,&elapsed)!=SWMM_OK&&elapsed==12345);SWMM_PassiveStatusV1 bounded=status(e);
    CHECK(!bounded.failed&&bounded.history_refused&&bounded.elapsed_ms==0);save(e,dir,"budget-after",0,copy);equal_files(path,copy);close_engine(e);
    e=start(input,64,0,0);int calls=0;CHECK(swmm_set_step_begin_callback(e,callback,&calls)==SWMM_OK);CHECK(swmm_engine_step(e,&elapsed)==SWMM_OK);CHECK(calls==1);CHECK(swmm_set_step_begin_callback(e,NULL,NULL)==SWMM_OK);close_engine(e);
    failure_case(input,1,0);failure_case(refusal,64,1);
    return 0;
}
