/* SPDX-License-Identifier: Apache-2.0 */
#include "test_material_transport.h"
#include <openswmm/engine/openswmm_split_packet.h>
#include <openswmm/engine/openswmm_passive_exchange.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
static void require(int yes,const char* text,int line){if(!yes){fprintf(stderr,"line %d: %s; material: %s\n",line,text,swmm_split_material_last_error());exit(1);}}
#define CHECK(x) require(!!(x),#x,__LINE__)
static char* copy(const char* p,size_t n){char* out=(char*)malloc(n+1);CHECK(out);memcpy(out,p,n);out[n]=0;return out;}
/* Independent C representation: reverse top-level JSON members, keeping the
 * nested values and string escapes intact. The engine must parse the value. */
static char* reordered(const char* p,size_t n){
    size_t starts[32],ends[32],count=0,start=1;int depth=0,quoted=0,escaped=0;
    CHECK(n>1&&p[0]=='{'&&p[n-1]=='}');
    for(size_t i=0;i<n;++i){char c=p[i];
        if(quoted){if(escaped)escaped=0;else if(c=='\\')escaped=1;else if(c=='"')quoted=0;continue;}
        if(c=='"'){quoted=1;continue;}if(c=='{'||c=='[')++depth;
        if((c==','&&depth==1)||(c=='}'&&depth==1)){CHECK(count<32);starts[count]=start;ends[count++]=i;start=i+1;}
        if(c=='}'||c==']')--depth;
    }
    CHECK(!depth&&!quoted&&count);char* out=(char*)malloc(n+1);CHECK(out);size_t at=0;out[at++]='{';
    for(size_t k=count;k>0;--k){size_t i=k-1,len=ends[i]-starts[i];memcpy(out+at,p+starts[i],len);at+=len;if(i)out[at++]=',';}
    out[at++]='}';out[at]=0;CHECK(at==n);return out;
}
int material_test_transport(SWMM_Engine network,int stage,const char* request,size_t length,const char** out,size_t* size,void* user){
    MaterialTestTransport* t=(MaterialTestTransport*)user;CHECK(t&&t->surface&&out&&size);
    CHECK(stage>=1&&stage<=3);
    CHECK(swmm_engine_close(network)==SWMM_ERR_LIFECYCLE);CHECK(swmm_engine_close(t->surface)==SWMM_ERR_LIFECYCLE);
    double untouched=123;
    CHECK(swmm_engine_step(network,&untouched)==SWMM_ERR_LIFECYCLE&&untouched==123);
    CHECK(swmm_engine_step(t->surface,&untouched)==SWMM_ERR_LIFECYCLE&&untouched==123);
    CHECK(swmm_engine_end(network)==SWMM_ERR_LIFECYCLE);CHECK(swmm_engine_end(t->surface)==SWMM_ERR_LIFECYCLE);
    CHECK(swmm_split_material_set_transport(network,material_test_transport,t)==SWMM_ERR_LIFECYCLE);
    if(t->last_request){SWMM_SplitFrame unchanged=(void*)(uintptr_t)1;CHECK(swmm_split_evaluate(t->surface,t->last_phase,t->last_request,&unchanged)==SWMM_ERR_LIFECYCLE);CHECK(unchanged==(void*)(uintptr_t)1);}
    if(stage==SWMM_MATERIAL_OFFER)++t->offers;if(stage==SWMM_MATERIAL_PROOF)++t->proofs;if(stage==SWMM_MATERIAL_ACK)++t->acknowledgements;
    if(stage==SWMM_MATERIAL_OFFER){const char* at=strstr(request,"\"ordinal\":\"");CHECK(at);t->last_ordinal=(uint64_t)strtoull(at+strlen("\"ordinal\":\""),NULL,10);if(strstr(request,"\"leg_limit\":\"0\""))++t->zero_offers;}
    if(stage==SWMM_MATERIAL_PROOF&&strstr(request,"\"legs\":[]"))++t->empty_proofs;
    if(t->log){fprintf(t->log,"%d %.*s\n",stage,(int)length,request);fflush(t->log);}
    if((t->mode==4&&stage==SWMM_MATERIAL_PROOF)||(t->mode==6&&stage==SWMM_MATERIAL_ACK))return SWMM_ERR_PLUGIN;
    swmm_split_packet_free(t->response);t->response=NULL;
    char* changed=reordered(request,length);++t->reordered;
    if(t->mode==1&&stage==SWMM_MATERIAL_OFFER){char* at=strstr(changed,"\"granted\":{\"parts\":[{\"f64\":\"");CHECK(at);at+=strlen("\"granted\":{\"parts\":[{\"f64\":\"");memcpy(at,"40f0000000000000",16);}
    if(t->mode==2&&stage==SWMM_MATERIAL_OFFER){char* at=strstr(changed,"\"recipient\":\"");CHECK(at);at+=strlen("\"recipient\":\"");*at=*at=='9'?'8':'9';}
    if(t->mode==7&&stage==SWMM_MATERIAL_OFFER){char* at=strstr(changed,"\"f64\":\"");CHECK(at);at+=strlen("\"f64\":\"");memcpy(at,"7ff0000000000000",16);}
    if(t->mode==8&&stage==SWMM_MATERIAL_OFFER){
        char* at=strstr(changed,"\"granted\":{\"parts\":[");CHECK(at);at+=strlen("\"granted\":{\"parts\":[");char* end=strchr(at,']');CHECK(end);
        const char* digits=strstr(at,"\"f64\":\"");CHECK(digits);digits+=strlen("\"f64\":\"");char hex[17];memcpy(hex,digits,16);hex[16]=0;
        uint64_t raw=(uint64_t)strtoull(hex,NULL,16);double value;memcpy(&value,&raw,sizeof value);value*=.5;memcpy(&raw,&value,sizeof raw);
        char replacement[96];int n=snprintf(replacement,sizeof replacement,"{\"f64\":\"%016" PRIx64 "\"},{\"f64\":\"%016" PRIx64 "\"}",raw,raw);CHECK(n>0);
        size_t prefix=(size_t)(at-changed),old=(size_t)(end-at),total=length-old+(size_t)n;char* next=(char*)malloc(total+1);CHECK(next);
        memcpy(next,changed,prefix);memcpy(next+prefix,replacement,(size_t)n);memcpy(next+prefix+(size_t)n,end,length-prefix-old+1);free(changed);changed=next;length=total;
    }
    if(stage==SWMM_MATERIAL_OFFER&&t->mode==3){if(!t->saved_offer)t->saved_offer=copy(changed,length);else {free(changed);length=strlen(t->saved_offer);changed=copy(t->saved_offer,length);}}
    if(stage==SWMM_MATERIAL_PROOF&&t->mode==5){if(!t->saved_proof)t->saved_proof=copy(changed,length);else {free(changed);length=strlen(t->saved_proof);changed=copy(t->saved_proof,length);}}
    int rc=swmm_split_material_packet_copy(t->surface,changed,length,&t->response);free(changed);
    if(rc!=SWMM_OK)return rc;
    /* Decode/copy twice is observational. It cannot redeem a second offer. */
    char* second=NULL;CHECK(swmm_split_material_packet_copy(t->surface,t->response,strlen(t->response),&second)==SWMM_OK);
    CHECK(!strcmp(second,t->response));swmm_split_packet_free(second);
    if(t->log){fprintf(t->log,"%d %s\n",stage+10,t->response);fflush(t->log);}
    *out=t->response;*size=strlen(t->response);return SWMM_OK;
}
int material_test_exchange(SWMM_Engine network,int phase,SWMM_SplitFrame request,SWMM_SplitFrame* out,void* user){
    MaterialTestTransport* t=(MaterialTestTransport*)user;char *encoded=NULL,*returned=NULL;SWMM_SplitFrame decoded=NULL,reply=NULL,result=NULL;
    int rc=swmm_split_material_packet_encode(network,request,&encoded);if(rc)goto done;
    if(t->log)fprintf(t->log,"0 %s\n",encoded);
    rc=swmm_split_material_packet_decode(t->surface,encoded,strlen(encoded),&decoded);if(rc)goto done;
    swmm_split_frame_destroy(t->last_request);t->last_request=NULL;t->last_phase=phase;
    rc=swmm_split_material_packet_decode(t->surface,encoded,strlen(encoded),&t->last_request);if(rc)goto done;
    rc=swmm_split_evaluate(t->surface,phase,decoded,&reply);if(rc)goto done;
    rc=swmm_split_material_packet_encode(t->surface,reply,&returned);if(rc)goto done;
    if(t->log)fprintf(t->log,"0 %s\n",returned);
    rc=swmm_split_material_packet_decode(network,returned,strlen(returned),&result);if(rc)goto done;
    *out=result;result=NULL;
 done:
    swmm_split_packet_free(encoded);swmm_split_packet_free(returned);swmm_split_frame_destroy(decoded);swmm_split_frame_destroy(reply);swmm_split_frame_destroy(result);return rc;
}
void material_test_transport_close(MaterialTestTransport* t){swmm_split_packet_free(t->response);free(t->saved_offer);free(t->saved_proof);swmm_split_frame_destroy(t->last_request);if(t->log)fclose(t->log);memset(t,0,sizeof *t);}
#ifndef MATERIAL_TRANSPORT_ONLY
static void guards(int off){
    SWMM_Engine e=swmm_engine_create();CHECK(e);char* text=(char*)(uintptr_t)1;SWMM_SplitFrame frame=(void*)(uintptr_t)1;
    CHECK(swmm_split_material_configure(NULL,1)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_validate_input(NULL)==SWMM_ERR_BADHANDLE);
    CHECK(swmm_split_material_validate_input(e)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));
    CHECK(swmm_split_material_bind(e,NULL,64)==SWMM_ERR_BADHANDLE);CHECK(swmm_split_material_bind(e,e,0)==SWMM_ERR_BADPARAM);
    CHECK(swmm_split_material_packet_encode(e,NULL,&text)==SWMM_ERR_BADPARAM);CHECK(text==(char*)(uintptr_t)1);
    CHECK(swmm_split_material_packet_decode(e,"{}",SWMM_MATERIAL_PACKET_MAX_BYTES+1u,&frame)==SWMM_ERR_BADPARAM);CHECK(frame==(void*)(uintptr_t)1);
    CHECK(swmm_split_material_packet_copy(e,"{}",2,&text)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_BADPARAM));CHECK(text==(char*)(uintptr_t)1);
    CHECK(swmm_split_material_configure(e,1)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));
    CHECK(swmm_split_material_set_transport(e,material_test_transport,NULL)==(off?SWMM_ERR_PLUGIN:SWMM_ERR_LIFECYCLE));
    swmm_engine_destroy(e);
}
#ifdef MATERIAL_SPLIT_OFF
int main(void){guards(1);return 0;}
#else
static void same_nodes(SWMM_Engine ref,SWMM_Engine network){
    double a,b;CHECK(swmm_node_get_quality(ref,0,0,&a)==SWMM_OK);CHECK(swmm_node_get_quality(network,0,0,&b)==SWMM_OK);CHECK(a==b);
    CHECK(swmm_node_get_volume(ref,0,&a)==SWMM_OK);CHECK(swmm_node_get_volume(network,0,&b)==SWMM_OK);CHECK(a==b);
}
static void same(SWMM_Engine ref,SWMM_Engine network,SWMM_Engine surface){
    same_nodes(ref,network);
    int n=0,m=0;CHECK(swmm_2d_cell_count(ref,&n)==SWMM_OK);CHECK(swmm_2d_cell_count(surface,&m)==SWMM_OK);CHECK(n>0&&n==m);
    size_t bytes=(size_t)n*sizeof(double);double* x=(double*)malloc(bytes);double* y=(double*)malloc(bytes);CHECK(x&&y);
    CHECK(swmm_2d_get_depths_bulk(ref,x)==SWMM_OK);CHECK(swmm_2d_get_depths_bulk(surface,y)==SWMM_OK);CHECK(!memcmp(x,y,bytes));
    int nx=0,ny=0;CHECK(swmm_2d_get_cell_conc(ref,0,x,n,&nx)==SWMM_OK);CHECK(swmm_2d_get_cell_conc(surface,0,y,n,&ny)==SWMM_OK);CHECK(nx==n&&ny==n&&!memcmp(x,y,bytes));free(x);free(y);
}
static void identical(const char* a,const char* b){FILE* x=fopen(a,"rb");FILE* y=fopen(b,"rb");CHECK(x&&y);for(;;){int c=fgetc(x),d=fgetc(y);CHECK(c==d);if(c==EOF)break;}fclose(x);fclose(y);}
int main(int argc,char** argv){
    if(argc==2&&!strcmp(argv[1],"--guards")){guards(0);return 0;}if(argc==2&&!strcmp(argv[1],"--off")){guards(1);return 0;}
    CHECK(argc==6||argc==7);const char* inp=argv[1];const char* dir=argv[2];int steps=atoi(argv[3]),mode=atoi(argv[4]),binary=atoi(argv[5]);uint64_t capacity=argc==7?(uint64_t)atoi(argv[6]):64;
    SWMM_Engine ref=swmm_engine_create(),network=swmm_engine_create(),surface=swmm_engine_create();CHECK(ref&&network&&surface);
    char native[4096],paired[4096],log[4096];snprintf(native,sizeof native,"%s/native.out",dir);snprintf(paired,sizeof paired,"%s/network.out",dir);snprintf(log,sizeof log,"%s/packets.jsonl",dir);
    MaterialTestTransport transport={0};transport.surface=surface;transport.mode=mode;transport.log=fopen(log,"w");CHECK(transport.log);
    CHECK(swmm_engine_open(ref,inp,NULL,binary?native:NULL,NULL)==SWMM_OK);CHECK(swmm_engine_open(network,inp,NULL,binary?paired:NULL,NULL)==SWMM_OK);CHECK(swmm_engine_open(surface,inp,NULL,NULL,NULL)==SWMM_OK);
    CHECK(swmm_split_configure(network,1)!=SWMM_OK);CHECK(swmm_split_material_configure(network,1)==SWMM_OK);CHECK(swmm_split_material_configure(surface,2)==SWMM_OK);
    CHECK(swmm_engine_initialize(surface)==SWMM_OK);SWMM_SplitFrame geometry=NULL,imported=NULL;char* encoded=NULL;
    CHECK(swmm_split_geometry(surface,&geometry)==SWMM_OK);CHECK(swmm_split_material_packet_encode(surface,geometry,&encoded)==SWMM_OK);
    CHECK(swmm_engine_initialize(network)==SWMM_OK);CHECK(swmm_split_material_packet_decode(network,encoded,strlen(encoded),&imported)==SWMM_OK);
    CHECK(swmm_split_apply_geometry(network,imported)==SWMM_OK);swmm_split_frame_destroy(geometry);swmm_split_frame_destroy(imported);swmm_split_packet_free(encoded);
    CHECK(swmm_split_set_callback(network,material_test_exchange,&transport)==SWMM_OK);
    CHECK(swmm_engine_initialize(ref)==SWMM_OK);CHECK(swmm_engine_start(ref,binary)==SWMM_OK);CHECK(swmm_passive_exchange_enable(ref,capacity,0)==SWMM_OK);
    CHECK(swmm_engine_start(surface,0)==SWMM_OK);CHECK(swmm_engine_start(network,binary)==SWMM_OK);
    CHECK(swmm_split_material_bind(network,surface,64)==SWMM_ERR_LIFECYCLE);
    CHECK(swmm_split_material_set_transport(network,material_test_transport,&transport)==SWMM_OK);CHECK(swmm_split_material_bind(network,surface,capacity)==SWMM_OK);
    CHECK(swmm_split_material_bind(network,surface,64)==SWMM_ERR_LIFECYCLE);same(ref,network,surface);
    int cells=0;CHECK(swmm_2d_cell_count(surface,&cells)==SWMM_OK);CHECK(cells>0);size_t bytes=(size_t)cells*sizeof(double);
    double* before=(double*)malloc(bytes);double* after=(double*)malloc(bytes);CHECK(before&&after);
    double a=0,b=0;int failure=0;const int until=(mode==3||mode==5)?8:4;
    for(int k=0;k<(mode?until:steps);++k){CHECK(swmm_2d_get_depths_bulk(surface,before)==SWMM_OK);
        int rc=swmm_engine_step(network,&b);
        if(rc){CHECK(mode&&k==until-1);failure=1;if(mode==1||mode==2||mode==3||mode==7||mode==8){CHECK(swmm_2d_get_depths_bulk(surface,after)==SWMM_OK);CHECK(!memcmp(before,after,bytes));}
            char root[4096];snprintf(root,sizeof root,"%s",swmm_get_last_error_msg(network));CHECK(swmm_engine_end(network)!=SWMM_OK);CHECK(!strcmp(root,swmm_get_last_error_msg(network)));break;}
        CHECK(swmm_engine_step(ref,&a)==SWMM_OK);CHECK(a==b);same(ref,network,surface);
    }
    free(before);free(after);
    if(mode)CHECK(failure);else {CHECK(swmm_engine_end(ref)==SWMM_OK);CHECK(swmm_engine_end(network)==SWMM_OK);same_nodes(ref,network);CHECK(transport.offers&&transport.offers==transport.proofs&&transport.proofs==transport.acknowledgements);CHECK(transport.reordered==transport.offers*3);}
    CHECK(swmm_engine_close(network)==SWMM_OK);CHECK(swmm_engine_close(surface)==SWMM_OK);CHECK(swmm_engine_close(ref)==SWMM_OK);
    swmm_engine_destroy(network);swmm_engine_destroy(surface);swmm_engine_destroy(ref);material_test_transport_close(&transport);
    if(!mode&&binary)identical(native,paired);return 0;
}
#endif
#endif
