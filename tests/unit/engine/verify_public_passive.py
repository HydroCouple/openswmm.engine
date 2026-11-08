#!/usr/bin/env python3
"""C11 public native reference compared with the qualified C++/Fraction traces."""
import argparse
from fractions import Fraction as F
from pathlib import Path
import json, os, subprocess, sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--engine',type=Path,required=True);p.add_argument('--caller',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
with (out/'native-reference.log').open('w') as log:
    subprocess.run([sys.executable,str(Path(__file__).with_name('verify_native_bases.py')),'--engine',str(a.engine.resolve()),'--output',str(out/'reference')],stdout=log,stderr=subprocess.STDOUT,check=True)
def floats(text):return list(map(float.fromhex,text.split()))
def parts(scalar,components):return components if components else ([scalar] if scalar else [])
def amount(words,at):
    n=int(words[at]);assert 0<=n<=64
    v=list(map(float.fromhex,words[at+1:at+1+n]));return v,at+1+n
cases=[]
for label,code,unit in [('mg',0,'MG/L'),('ug',1,'UG/L'),('counts',2,'#/L')]:
    for sign in ['positive','negative']:
        case=out/f'{label}-{sign}';case.mkdir(exist_ok=True);template=Path(__file__).parent/'data/passive_exchange_c'/(sign+'.inp')
        model=template.read_text().replace('Dye MG/L','Dye '+unit)
        inp=case/'model.inp';inp.write_text(model)
        refusal=case/'refusal.inp';lines=model.splitlines();inside=False
        for k,line in enumerate(lines):
            if line=='[2D_VERTEX_NODE_MAP]':inside=True;continue
            if line.startswith('['):inside=False
            if inside and line.strip():fields=line.split();fields[-1]='0.01';lines[k]=' '.join(fields)
        refusal.write_text('\n'.join(lines)+'\n')
        with (case/'caller.log').open('w') as log:
            subprocess.run([str(a.caller.resolve()),str(inp),str(case),str(code),str(refusal),'1' if sign=='positive' else '-1'],env=dict(os.environ,OPENSWMM_2D_BACKEND='cpu'),stdout=log,stderr=subprocess.STDOUT,check=True)
        ref=out/'reference/native';tag=f'basis-{label}-{sign}'
        expected_states={}
        for line in (ref/(tag+'-states.txt')).read_text().splitlines():
            c=line.split();expected_states[int(c[0])]=floats(' '.join(c[1:]))
        expected_legs={}
        for line in (ref/(tag+'-legs.txt')).read_text().splitlines():
            c=line.split();expected_legs[tuple(map(int,c[:3]))]=(tuple(map(int,c[3:5])),floats(' '.join(c[5:])))
        expected_rows={};pending={k:[F(0),F(0)] for k in range(9)}
        for line in (ref/(tag+'-rows.txt')).read_text().splitlines():
            identity,wp,mp=line.split('|');c=identity.split();step,rid,remaining,point,drain=map(int,c[:5]);w,m=floats(' '.join(c[5:]));wv,mv=parts(w,floats(wp)),parts(m,floats(mp))
            expected_rows[step,rid,remaining,point,drain]=(wv,mv)
            if remaining:pending[step][0 if drain else 1]+=sum(map(F,mv),F(0))
        seen_pools=set();seen_rows=set();seen_legs=set()
        for line in (case/'trace.txt').read_text().splitlines():
            c=line.split();kind=c[0];step=int(c[1]);at=2
            if kind=='P':
                v,conc,physical,hydraulic=floats(' '.join(c[2:6]));at=6;r,at=amount(c,at);d,at=amount(c,at);s,at=amount(c,at)
                expected=expected_states[step];assert v==hydraulic==expected[0] and conc==expected[2] and physical==v*conc
                assert r==expected[6:] and sum(map(F,d),F(0))==pending[step][0] and sum(map(F,s),F(0))==pending[step][1]
                assert step not in seen_pools;seen_pools.add(step)
            elif kind=='R':
                rid,remaining,point=map(int,c[2:5]);at=5;dw,at=amount(c,at);sw,at=amount(c,at);dm,at=amount(c,at);sm,at=amount(c,at)
                assert (dw,dm)==expected_rows[step,rid,remaining,point,1]
                assert (sw,sm)==expected_rows[step,rid,remaining,point,0]
                key=(step,rid,remaining,point);assert key not in seen_rows;seen_rows.add(key)
            else:
                assert kind=='L';rid,k,point,drain=map(int,c[2:6]);actual=floats(' '.join(c[6:]));at=len(c)
                assert ((point,drain),actual)==expected_legs[step,rid,k]
                key=(step,rid,k);assert key not in seen_legs;seen_legs.add(key)
            assert at==len(c)
        assert seen_pools==set(range(9))
        assert seen_rows=={key[:4] for key in expected_rows}
        assert seen_legs==set(expected_legs)
        for step in range(9):
            assert (case/f'source-{step}.bin').read_bytes()==(ref/f'{tag}-{step}.bin').read_bytes(),(label,sign,step,'full native checkpoint')
        cases.append(dict(basis=label,sign=sign,pool_observations=len(seen_pools),row_observations=len(seen_rows),leg_observations=len(seen_legs),full_native_checkpoints=9,continued_routing_steps=2,matching_restored_checkpoints=3))
(out/'accounting.json').write_text(json.dumps(dict(cases=cases,native_checkpoints=54,pool_observations=54,continued_routing_steps=12,matching_restored_checkpoints=18,cpp_fraction_reference=True,split_species_still_refused=True),indent=2)+'\n')
print('C11 facade: six cases, 54 exact native checkpoints and pool observations, 12 continued routing steps and 18 matching restored checkpoints, callback/terminal/argument guards pass.')
