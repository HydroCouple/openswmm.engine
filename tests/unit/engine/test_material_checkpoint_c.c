/* SPDX-License-Identifier: Apache-2.0 */
/* C11 caller owns persistence, callback storage and both fresh engine roles. */
#include "test_material_transport.h"
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_passive_exchange.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static void require(int ok,const char* text,int line) {
    if(!ok){fprintf(stderr,"line %d: %s; recovery: %s\n",line,text,swmm_split_material_checkpoint_last_error());exit(1);}
}
#define CHECK(x) require(!!(x),#x,__LINE__)
static void guards(int off) {
    SWMM_Engine e=swmm_engine_create();CHECK(e);unsigned long flags=123;
    SWMM_MaterialCheckpoint out=(SWMM_MaterialCheckpoint)(uintptr_t)1;const void* data=(void*)(uintptr_t)2;size_t n=333;
    CHECK(swmm_split_material_checkpoint_capabilities(NULL,&flags)==SWMM_ERR_BADHANDLE&&flags==123);
    CHECK(swmm_split_material_checkpoint_capabilities(e,NULL)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_capabilities(e,&flags)==SWMM_OK);
    CHECK(off?flags==0:(flags&SWMM_MATERIAL_CHECKPOINT_NUMERICAL)!=0);
    CHECK(swmm_split_material_checkpoint_prepare_outputs(NULL)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_checkpoint_prepare_outputs(e)==((flags&SWMM_MATERIAL_CHECKPOINT_OUTPUTS)?SWMM_ERR_LIFECYCLE:SWMM_ERR_PLUGIN));
    CHECK(swmm_split_material_checkpoint_enable(NULL,0)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_checkpoint_enable(e,2)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_enable(e,0)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));
    CHECK(swmm_split_material_checkpoint_capture(NULL,&out)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_checkpoint_capture(e,NULL)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_capture(e,&out)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));CHECK(out==(SWMM_MaterialCheckpoint)(uintptr_t)1);
    CHECK(swmm_split_material_checkpoint_from_bytes(NULL,2,&out)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_from_bytes("{}",0,&out)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_from_bytes("{}",(size_t)SWMM_MATERIAL_CHECKPOINT_MAX_BYTES+1,&out)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_from_bytes("{}",2,NULL)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_checkpoint_from_bytes("{}",2,&out)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_BADPARAM));CHECK(out==(SWMM_MaterialCheckpoint)(uintptr_t)1);
    CHECK(swmm_split_material_checkpoint_data(NULL,&data,&n)==SWMM_ERR_BADHANDLE);CHECK(data==(void*)(uintptr_t)2&&n==333);
    CHECK(swmm_split_material_checkpoint_restore(e,NULL)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_checkpoint_restore(NULL,NULL)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_checkpoint_restore_at(NULL,NULL,0)==SWMM_ERR_BADHANDLE);
    swmm_split_material_checkpoint_destroy(NULL);swmm_engine_destroy(e);
}
#ifndef MATERIAL_RECOVERY_OFF
typedef struct Harness {
    MaterialTestTransport transport;
    SWMM_Engine network,surface;
    SWMM_MaterialCheckpoint witness;
    int outputs,has_outputs,active_refusals,accepted,phase_refusals;
} Harness;
static void path(char* out,const char* dir,const char* name){CHECK(snprintf(out,4096,"%s/%s",dir,name)>0);}
static void write_bytes(const char* file,const void* data,size_t n){FILE* f=fopen(file,"wb");CHECK(f);CHECK(fwrite(data,1,n,f)==n);CHECK(!fclose(f));}
static void* read_bytes(const char* file,size_t* n){FILE* f=fopen(file,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>0&&(unsigned long)size<=SWMM_MATERIAL_CHECKPOINT_MAX_BYTES);rewind(f);void* data=malloc((size_t)size);CHECK(data);CHECK(fread(data,1,(size_t)size,f)==(size_t)size);CHECK(!fclose(f));*n=(size_t)size;return data;}
static void snapshot_bytes(SWMM_MaterialCheckpoint s,const void** data,size_t* n){CHECK(swmm_split_material_checkpoint_data(s,data,n)==SWMM_OK);CHECK(*data&&*n&&*n<=SWMM_MATERIAL_CHECKPOINT_MAX_BYTES);}
static void save(SWMM_MaterialCheckpoint s,const char* dir,const char* prefix,int step){char file[4096],name[80];const void* data;size_t n;snapshot_bytes(s,&data,&n);snprintf(name,sizeof name,"%s-%d.bin",prefix,step);path(file,dir,name);write_bytes(file,data,n);}
static SWMM_MaterialCheckpoint capture(Harness* h,const char* dir,const char* prefix,int step) {
    SWMM_MaterialCheckpoint s=(SWMM_MaterialCheckpoint)(uintptr_t)1;
    int rc=swmm_split_material_checkpoint_capture(h->network,&s);
    if(rc){CHECK(rc==SWMM_ERR_IO&&step>8);CHECK(s==(SWMM_MaterialCheckpoint)(uintptr_t)1);CHECK(strstr(swmm_split_material_checkpoint_last_error(),"staged archive profile"));return NULL;}
    save(s,dir,prefix,step);return s;
}
static void blocked(Harness* h) {
    SWMM_MaterialCheckpoint result=(SWMM_MaterialCheckpoint)(uintptr_t)1;
    CHECK(swmm_split_material_checkpoint_enable(h->network,h->outputs)==SWMM_ERR_LIFECYCLE);
    CHECK(swmm_split_material_checkpoint_prepare_outputs(h->network)==(h->has_outputs?SWMM_ERR_LIFECYCLE:SWMM_ERR_PLUGIN));
    CHECK(swmm_split_material_checkpoint_capture(h->network,&result)==SWMM_ERR_LIFECYCLE);CHECK(result==(SWMM_MaterialCheckpoint)(uintptr_t)1);
    CHECK(swmm_split_material_checkpoint_capture(h->surface,&result)==SWMM_ERR_LIFECYCLE);CHECK(result==(SWMM_MaterialCheckpoint)(uintptr_t)1);
    CHECK(swmm_split_material_checkpoint_restore(h->network,h->witness)==SWMM_ERR_LIFECYCLE);
    CHECK(swmm_split_material_checkpoint_restore_at(h->network,h->witness,0)==SWMM_ERR_LIFECYCLE);
    const void* bytes;size_t n;snapshot_bytes(h->witness,&bytes,&n); /* immutable observations remain legal */
}
static int transport(SWMM_Engine e,int stage,const char* request,size_t n,const char** out,size_t* length,void* user) {
    Harness* h=(Harness*)user;blocked(h);++h->active_refusals;
    return material_test_transport(e,stage,request,n,out,length,&h->transport);
}
static int exchange(SWMM_Engine e,int phase,SWMM_SplitFrame in,SWMM_SplitFrame* out,void* user) {
    Harness* h=(Harness*)user;blocked(h);++h->phase_refusals;
    return material_test_exchange(e,phase,in,out,&h->transport);
}
static void observe(SWMM_Engine e,int phase,double start,double dt,void* user) {
    (void)e;(void)start;(void)dt;Harness* h=(Harness*)user;
    if(phase==SWMM_PHASE_STEP_ACCEPTED){blocked(h);++h->accepted;}
}
static void open_pair(Harness* h,const char* input,const char* dir,int outputs,int enroll) {
    memset(h,0,sizeof *h);h->outputs=outputs;h->network=swmm_engine_create();h->surface=swmm_engine_create();CHECK(h->network&&h->surface);
    unsigned long flags=0;CHECK(swmm_split_material_checkpoint_capabilities(h->network,&flags)==SWMM_OK);h->has_outputs=(flags&SWMM_MATERIAL_CHECKPOINT_OUTPUTS)!=0;
    char binary[4096],mesh[4096];path(binary,dir,"network.out");path(mesh,dir,"surface.h5");
    CHECK(swmm_engine_open(h->network,input,NULL,outputs?binary:NULL,NULL)==SWMM_OK);
    CHECK(swmm_engine_open(h->surface,input,NULL,NULL,NULL)==SWMM_OK);
    CHECK(swmm_split_material_checkpoint_prepare_outputs(h->network)==(h->has_outputs?SWMM_ERR_LIFECYCLE:SWMM_ERR_PLUGIN));
    CHECK(swmm_split_material_configure(h->network,1)==SWMM_OK);CHECK(swmm_split_material_configure(h->surface,2)==SWMM_OK);
    if(outputs)CHECK(swmm_2d_set_results_file(h->surface,mesh)==SWMM_OK);
    CHECK(swmm_engine_initialize(h->surface)==SWMM_OK);SWMM_SplitFrame geometry=NULL;CHECK(swmm_split_geometry(h->surface,&geometry)==SWMM_OK);
    CHECK(swmm_engine_initialize(h->network)==SWMM_OK);CHECK(swmm_split_apply_geometry(h->network,geometry)==SWMM_OK);swmm_split_frame_destroy(geometry);
    if(outputs){CHECK(swmm_split_material_checkpoint_prepare_outputs(h->network)==SWMM_OK);CHECK(swmm_split_material_checkpoint_prepare_outputs(h->surface)==SWMM_OK);CHECK(swmm_split_material_checkpoint_prepare_outputs(h->network)==SWMM_ERR_LIFECYCLE);CHECK(swmm_split_material_checkpoint_prepare_outputs(h->surface)==SWMM_ERR_LIFECYCLE);}
    h->transport.surface=h->surface;
    CHECK(swmm_split_set_callback(h->network,exchange,h)==SWMM_OK);
    CHECK(swmm_engine_start(h->surface,0)==SWMM_OK);CHECK(swmm_engine_start(h->network,outputs)==SWMM_OK);
    CHECK(swmm_split_material_set_transport(h->network,transport,h)==SWMM_OK);
    CHECK(swmm_split_material_checkpoint_enable(h->network,outputs)==SWMM_ERR_LIFECYCLE);
    CHECK(swmm_split_material_bind(h->network,h->surface,64)==SWMM_OK);
    CHECK(swmm_split_material_checkpoint_enable(h->surface,outputs)==SWMM_ERR_LIFECYCLE);
    if(enroll){CHECK(swmm_split_material_checkpoint_enable(h->network,outputs)==SWMM_OK);CHECK(swmm_split_material_checkpoint_enable(h->network,outputs)==SWMM_ERR_LIFECYCLE);CHECK(swmm_split_material_checkpoint_capture(h->network,&h->witness)==SWMM_OK);}
    CHECK(swmm_set_phase_callback(h->network,observe,h)==SWMM_OK);
}
static void close_pair(Harness* h){CHECK(swmm_engine_close(h->network)==SWMM_OK);CHECK(swmm_engine_close(h->surface)==SWMM_OK);swmm_engine_destroy(h->network);swmm_engine_destroy(h->surface);material_test_transport_close(&h->transport);swmm_split_material_checkpoint_destroy(h->witness);}
static SWMM_Engine reference(const char* input,const char* dir,int outputs) {
    SWMM_Engine e=swmm_engine_create();CHECK(e);char binary[4096],mesh[4096];path(binary,dir,"native.out");path(mesh,dir,"native.h5");
    CHECK(swmm_engine_open(e,input,NULL,outputs?binary:NULL,NULL)==SWMM_OK);if(outputs)CHECK(swmm_2d_set_results_file(e,mesh)==SWMM_OK);
    CHECK(swmm_engine_initialize(e)==SWMM_OK);unsigned long flags=0;CHECK(swmm_split_material_checkpoint_capabilities(e,&flags)==SWMM_OK);
    CHECK(swmm_split_material_checkpoint_prepare_outputs(e)==((flags&SWMM_MATERIAL_CHECKPOINT_OUTPUTS)?SWMM_ERR_LIFECYCLE:SWMM_ERR_PLUGIN));
    CHECK(swmm_engine_start(e,outputs)==SWMM_OK);CHECK(swmm_passive_exchange_enable(e,64,0)==SWMM_OK);return e;
}
static void same(SWMM_Engine ref,Harness* h) {
    double a,b;CHECK(swmm_node_get_quality(ref,0,0,&a)==SWMM_OK);CHECK(swmm_node_get_quality(h->network,0,0,&b)==SWMM_OK);CHECK(a==b);
    CHECK(swmm_node_get_volume(ref,0,&a)==SWMM_OK);CHECK(swmm_node_get_volume(h->network,0,&b)==SWMM_OK);CHECK(a==b);
    int n=0,m=0;CHECK(swmm_2d_cell_count(ref,&n)==SWMM_OK);CHECK(swmm_2d_cell_count(h->surface,&m)==SWMM_OK);CHECK(n>0&&n==m);
    size_t bytes=(size_t)n*sizeof(double);double* x=(double*)malloc(bytes);double* y=(double*)malloc(bytes);CHECK(x&&y);
    CHECK(swmm_2d_get_depths_bulk(ref,x)==SWMM_OK);CHECK(swmm_2d_get_depths_bulk(h->surface,y)==SWMM_OK);CHECK(!memcmp(x,y,bytes));
    CHECK(swmm_2d_get_cell_conc(ref,0,x,n,&m)==SWMM_OK&&m==n);CHECK(swmm_2d_get_cell_conc(h->surface,0,y,n,&m)==SWMM_OK&&m==n);CHECK(!memcmp(x,y,bytes));free(x);free(y);
}
static void import_guards(SWMM_MaterialCheckpoint source) {
    const void* bytes;size_t n;snapshot_bytes(source,&bytes,&n);void* bad=malloc(n);CHECK(bad);memcpy(bad,bytes,n);
    SWMM_MaterialCheckpoint result=(SWMM_MaterialCheckpoint)(uintptr_t)1;const void* ptr=(void*)(uintptr_t)2;size_t len=99;
    CHECK(swmm_split_material_checkpoint_data(source,NULL,&len)==SWMM_ERR_BADPARAM&&len==99);
    CHECK(swmm_split_material_checkpoint_data(source,&ptr,NULL)==SWMM_ERR_BADPARAM&&ptr==(void*)(uintptr_t)2);
    CHECK(swmm_split_material_checkpoint_from_bytes(bytes,n-1,&result)==SWMM_ERR_BADPARAM);
    ((unsigned char*)bad)[n-1]^=1;CHECK(swmm_split_material_checkpoint_from_bytes(bad,n,&result)==SWMM_ERR_BADPARAM);
    CHECK(result==(SWMM_MaterialCheckpoint)(uintptr_t)1);free(bad);
}
int main(int argc,char** argv) {
    if(argc==2&&!strcmp(argv[1],"--guards")){guards(0);return 0;}
    CHECK(argc==7);const char* action=argv[1];const char* input=argv[2];const char* dir=argv[3];int outputs=atoi(argv[4]),capture_at=atoi(argv[5]);const char* parent=argv[6];CHECK(outputs==0||outputs==1);int last=outputs?12:8;
    Harness h;open_pair(&h,input,dir,outputs,1);double elapsed=0;
    if(!strcmp(action,"--original")) {
        SWMM_Engine ref=reference(input,dir,outputs);SWMM_MaterialCheckpoint states[13]={0};
        for(int k=0;k<=last;++k){if(k){CHECK(swmm_engine_step(ref,&elapsed)==SWMM_OK);CHECK(swmm_engine_step(h.network,&elapsed)==SWMM_OK);}same(ref,&h);states[k]=capture(&h,dir,"native",k);}
        CHECK(states[capture_at]);import_guards(states[capture_at]);
        char refused[4096];path(refused,dir,"refused-single-role.bin");CHECK(swmm_checkpoint_save(h.network,refused)!=SWMM_OK);
        CHECK(swmm_engine_end(ref)==SWMM_OK);CHECK(swmm_engine_end(h.network)==SWMM_OK);
        CHECK(swmm_engine_close(ref)==SWMM_OK);swmm_engine_destroy(ref);
        SWMM_MaterialCheckpoint independent=states[capture_at];const void* data;size_t n;snapshot_bytes(independent,&data,&n);
        void* copy=malloc(n);CHECK(copy);memcpy(copy,data,n);close_pair(&h);
        const void* after;size_t after_n;snapshot_bytes(independent,&after,&after_n);CHECK(n==after_n&&!memcmp(copy,after,n));free(copy);
        for(int k=0;k<=last;++k)swmm_split_material_checkpoint_destroy(states[k]);
    } else {
        CHECK(!strcmp(action,"--restore"));CHECK(capture_at>0&&capture_at<last);char file[4096],name[80];snprintf(name,sizeof name,"native-%d.bin",capture_at);path(file,parent,name);
        size_t n;void* raw=read_bytes(file,&n);SWMM_MaterialCheckpoint state=NULL;CHECK(swmm_split_material_checkpoint_from_bytes(raw,n,&state)==SWMM_OK);
        memset(raw,0,n);free(raw);CHECK(h.accepted==0&&h.transport.offers==0);
        CHECK(swmm_split_material_checkpoint_restore(h.surface,state)==SWMM_ERR_LIFECYCLE);
        CHECK(swmm_split_material_checkpoint_restore(h.network,state)==SWMM_OK);CHECK(h.accepted==0&&h.transport.offers==0);
        for(int k=capture_at;k<=last;++k){if(k>capture_at)CHECK(swmm_engine_step(h.network,&elapsed)==SWMM_OK);SWMM_MaterialCheckpoint s=capture(&h,dir,"restored",k);swmm_split_material_checkpoint_destroy(s);}
        CHECK(h.accepted==last-capture_at);CHECK(h.active_refusals>0&&h.phase_refusals>0);
        CHECK(swmm_split_material_checkpoint_restore(h.network,state)==SWMM_ERR_LIFECYCLE);
        CHECK(swmm_engine_end(h.network)==SWMM_OK);close_pair(&h);swmm_split_material_checkpoint_destroy(state);
    }
    char report[4096];path(report,dir,"result.txt");FILE* f=fopen(report,"w");CHECK(f);fprintf(f,"%d %d %d %d\n",h.accepted,h.active_refusals,h.phase_refusals,!strcmp(action,"--restore"));CHECK(!fclose(f));return 0;
}
#else
int main(void){guards(1);return 0;}
#endif
