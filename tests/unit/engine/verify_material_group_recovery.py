"""Qualify component-module collective recovery using independent fresh executable hosts."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, struct, subprocess, zlib

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--caller',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();caller=args.caller.resolve();out=args.output.resolve()
# Fresh destinations are part of the native contract. Keep earlier attempts
# reviewable rather than replacing their checkpoint/output files.
if out.exists():
    index=1
    while out.with_name(out.name+'-prior-'+str(index)).exists():index+=1
    out.rename(out.with_name(out.name+'-prior-'+str(index)))
out.mkdir(parents=True)
env=dict(os.environ,OPENSWMM_2D_BACKEND='cpu')
data=Path(__file__).resolve().parent/'data/passive_exchange_c'
def string(raw,at):
    n=struct.unpack_from('=Q',raw,at)[0];at+=8;assert n<=len(raw)-at
    return raw[at:at+n],at+n
def encoded(value):return struct.pack('=Q',len(value))+value
def normalize(raw):
    magic,i=string(raw,0);version,endian,width=struct.unpack_from('=III',raw,i);model,i=string(raw,i+12)
    crc=struct.unpack_from('=I',raw,i)[0];body,i=string(raw,i+4);assert i==len(raw) and crc==zlib.crc32(body)
    name=b'OpenSWMM acknowledged network/surface storage owners';start=body.index(name)-16
    child,end=string(body,start);nm,j=string(child,0);v,e,w,c=struct.unpack_from('=IIII',child,j);payload,j=string(child,j+16)
    assert j==len(child) and nm==name and c==zlib.crc32(payload)
    identity=struct.unpack_from('=QQQ',payload);roles=struct.unpack_from('=QQ',body,len(body)-16)
    assert identity[:2]==roles and all(identity) and roles[0]!=roles[1]
    payload=bytes(24)+payload[24:];child=encoded(nm)+struct.pack('=IIII',v,e,w,zlib.crc32(payload))+encoded(payload)
    body=body[:start]+encoded(child)+body[end:]
    if version==2:
        name=b'OpenSWMM material pair output prefixes v1';start=body.index(name)-16;child,end=string(body,start)
        label,j=string(child,0);binary,j=string(child,j);mesh,j=string(child,j);assert j==len(child) and label==name and binary and mesh
        body=body[:start]+encoded(encoded(label)+encoded(b'')+encoded(b''))+body[end:]
    body=body[:-16]+bytes(16)
    return encoded(magic)+struct.pack('=III',version,endian,width)+encoded(model)+struct.pack('=I',zlib.crc32(body))+encoded(body),identity
def normalize_group(raw):
    assert raw[:8]==b'OSWMGR01'
    m,n,c=struct.unpack_from('<III',raw,8);assert m+n==len(raw)-20 and c==zlib.crc32(raw[20:])
    meta=json.loads(raw[20:20+m]);native,identity=normalize(raw[20+m:])
    for role in meta['observations']:
        if role['last']:role['last']['session']='historical'
    return (meta,native),identity
def run(folder,command):
    folder.mkdir(parents=True,exist_ok=True)
    with (folder/'caller.log').open('w') as log:subprocess.run(list(map(str,command)),env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
records=[];hdf=[];snapshots=0;continued=0;identities=[]
h5dump=shutil.which('h5dump');h5diff=shutil.which('h5diff')
def same_hdf(first,second,folder):
    if not h5dump or not h5diff:return False
    values=[]
    for tag,file in [('expected',first),('actual',second)]:
        value=subprocess.check_output([h5dump,'-m','%.17g','-w','0',str(file)]).split(b'\n',1)[1]
        (folder/(tag+'.h5dump')).write_bytes(value);values.append(value)
    assert values[0]==values[1] and b'DATASET "Mesh2_face_species_conc"' in values[1]
    with (folder/'h5diff.log').open('w') as log:subprocess.run([h5diff,str(first),str(second)],stdout=log,stderr=subprocess.STDOUT,check=True)
    return True
profiles=[(mode,code,units,sign,tail)
    for tail in [False,True] for mode in ([0] if tail else [0,1])
    for code,units in enumerate(['MG/L','UG/L','#/L']) for sign in ['negative','positive']]
for output,code,units,sign,tail in profiles:
    case=out/(('tail-' if tail else '')+f'{output}-{code}-{sign}');case.mkdir();input_file=case/'model.inp'
    model=(data/(('tail-' if tail else '')+sign+'.inp')).read_text().replace('Dye MG/L','Dye '+units)
    if output:model=model.replace('REPORT_STEP 00:00:01','REPORT_STEP 00:00:00.0009765625').replace('REPORT_2D NO','REPORT_2D YES\nREPORT_2D_STEP 0.0009765625')
    input_file.write_text(model);parent=case/'original';run(parent,[caller,'--tail-original' if tail else '--original',input_file,parent,output,6 if tail else 5,parent])
    if output:
        assert (parent/'network/model.out').read_bytes()==(parent/'native.out').read_bytes()
        hdf.append(same_hdf(parent/'native.h5',parent/'surface/model.2d.h5',parent))
    for capture,repeat in ([(6,0)] if tail else [(5,0),(5,1),(6,0)]):
        child=case/(f'restored-{capture}'+('-repeat' if repeat else ''));run(child,[caller,'--tail-restore' if tail else '--restore',input_file,child,output,capture,parent])
        result=json.loads((child/'result.json').read_text());last=80 if tail else 12 if output else 8
        accepted=result['continued_steps'];assert result['fresh_process'] and accepted==last-capture
        assert result['observer_failures']==0 and result['callbacks']>0
        continued+=accepted
        for step in range(capture,last+1):
            before=parent/f'original-{step}.bin';after=child/f'restored-{step}.bin'
            if before.exists():
                a,old=normalize_group(before.read_bytes());b,new=normalize_group(after.read_bytes());assert a==b,(case,step)
                assert new[0] not in old[:2] and new[1] not in old[:2] and new[2]!=old[2]
                snapshots+=1
                if step==capture:identities.append(new)
            else:assert not after.exists() and ((output==1 and step>8) or (tail and step>6))
        if output:
            assert (child/'network/model.out').read_bytes()==(parent/'network/model.out').read_bytes()
            hdf.append(same_hdf(parent/'surface/model.2d.h5',child/'surface/model.2d.h5',child))
        records.append(dict(tail=tail,mode=output,basis=units,sign=sign,capture=capture,repeat=repeat,steps=accepted,
            status_callbacks=result['callbacks'],observer_failures=result['observer_failures'],one_ulp_refusal_retry=True,no_prefix_execution=True))
assert len(identities)==len(set(identities))==len(records)
summary=dict(profiles=len(profiles),full_duration_tail_restores=sum(r['tail'] for r in records),fresh_process_restores=len(records),continued_steps=continued,
    complete_normalized_snapshot_comparisons=snapshots,unique_restored_sessions=len(set(identities)),
    no_prefix_execution=True,complete_hdf5_comparisons=sum(hdf),hdf5_tools_available=bool(h5dump and h5diff),
    binary_outputs_match=True,records=records)
(out/'verification.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='records'},indent=2))
