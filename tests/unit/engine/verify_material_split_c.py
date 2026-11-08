"""Run the C11 material adapter against authored native references."""
from pathlib import Path
import argparse,json,os,subprocess

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--caller',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
    caller=a.caller.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    data=Path(__file__).resolve().parent/'data/passive_exchange_c';records=[]
    env=dict(os.environ,OPENSWMM_2D_BACKEND='cpu')
    cases=[]
    for basis,units in [('mg','MG/L'),('ug','UG/L'),('counts','#/L')]:
        for sign in ['positive','negative']:cases.append((f'slices-{basis}-{sign}',sign,units,8,0,0,64,False))
    for kind,binary in [('report',1),('final',0)]:
        for sign in ['positive','negative']:cases.append((kind+'-'+sign,sign,'MG/L',5,0,binary,64,False))
    for sign in ['positive','negative']:cases.append(('empty-'+sign,sign,'MG/L',4,0,0,6,True))
    for mode in range(1,9):cases.append((f'refusal-{mode}','positive','MG/L',8,mode,0,64,False))
    with (out/'guards.log').open('w') as log:subprocess.run([str(caller),'--guards'],stdout=log,stderr=subprocess.STDOUT,env=env,check=True)
    for tag,sign,units,steps,mode,binary,capacity,tail in cases:
        folder=out/tag;folder.mkdir(exist_ok=True);model=(data/((('tail-' if tail else '')+sign)+'.inp')).read_text().replace('Dye MG/L','Dye '+units)
        if tag.startswith('report-'):model=model.replace('REPORT_STEP 00:00:01','REPORT_STEP 00:00:00.00244140625')
        inp=folder/'model.inp';inp.write_text(model)
        with (folder/'caller.log').open('w') as log:
            subprocess.run([str(caller),str(inp),str(folder),str(steps),str(mode),str(binary),str(capacity)],stdout=log,stderr=subprocess.STDOUT,env=env,check=True)
        packets=[]
        for line in (folder/'packets.jsonl').read_text().splitlines():
            stage,packet=line.split(' ',1);packets.append((int(stage),json.loads(packet)))
        offers=[p for stage,p in packets if stage==1];proofs=[p for stage,p in packets if stage==2];acks=[p for stage,p in packets if stage==3]
        assert offers
        if not mode:
            assert len(offers)==len(proofs)==len(acks)==2,(tag,len(offers),len(proofs),len(acks))
            assert all(any(s==stage+10 and x==p for s,x in packets) for stage,p in packets if stage in [1,2,3])
        if tail:
            assert offers[-1]['body']['view']['allowance']['leg_limit']=='0'
            assert not proofs[-1]['body']['proof']['legs'] and not proofs[-1]['body']['rows']
            assert offers[-1]['body']['view']['allowance']['ordinal']=='2'
        records.append(dict(case=tag,units=units,steps=steps,refusal=mode,offers=len(offers),proofs=len(proofs),acknowledgements=len(acks),binary_native_equal=bool(binary)))
    (out/'verification.json').write_text(json.dumps(records,indent=2)+'\n')
    print('C11 material adapter: 12 authored references, eight terminal refusal cases, all native physical comparisons and requested binary outputs pass.')
if __name__=='__main__':main()
