#!/usr/bin/env python3
"""Independent rational audit of successive represented native leg partitions."""
import argparse
from collections import defaultdict
from fractions import Fraction as F
from pathlib import Path
import json, math, os, struct, subprocess, zlib
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--native-evidence',type=Path)
parser.add_argument('--case-prefix',default='slices')
a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);native=out/'native'
if a.native_evidence:
    native=a.native_evidence.resolve()
else:
    with (out/'authored.log').open('w') as log:
        subprocess.run([str(a.engine.resolve()),'--gtest_filter=ChangingVolumeExactSlices.Authored*','--gtest_output=xml:'+str(out/'authored.xml')],env=dict(os.environ,OPENSWMM_2D_BACKEND='cpu',OPENSWMM_CHANGING_VOLUME_EVIDENCE=str(native)),stdout=log,stderr=subprocess.STDOUT,check=True)
def vals(s):return list(map(float.fromhex,s.split()))
def exact(scalar,parts):
    assert math.isfinite(scalar) and scalar>=0
    if not parts:return F(scalar)
    assert scalar==0 and 2<=len(parts)<=64 and all(math.isfinite(x) and x!=0 for x in parts)
    result=sum(map(F,parts),F(0));assert result>0;return result

class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def get(self, fmt):
        fmt = '<' + fmt
        result = struct.unpack_from(fmt, self.data, self.at)
        self.at += struct.calcsize(fmt)
        return result[0] if len(result) == 1 else result

    def blob(self):
        count = self.get('Q')
        result = self.data[self.at:self.at + count]
        assert len(result) == count
        self.at += count
        return result

    def vector(self):
        return [self.get('d') for _ in range(self.get('Q'))]

    def strings(self):
        return [self.blob() for _ in range(self.get('Q'))]

    def finish(self):
        assert self.at == len(self.data)

def envelope(data, magic, version):
    r = Reader(data)
    assert r.blob() == magic.encode()
    assert r.get('II') == (version, 0x01020304)
    checksum = r.get('I')
    payload = r.blob()
    assert zlib.crc32(payload) == checksum
    r.finish()
    return Reader(payload)

def water_publications(path):
    """Decode actual saved queue observations independently of the engine reader."""
    r = envelope(path.read_bytes(), 'OpenSWMM live passive owner', 12)
    header = r.get('dQQQddBBQddddQ')
    for _ in range(header[-1]):
        r.get('ii')
    b = envelope(r.blob(), 'OpenSWMM changing-volume storage receipt owner', 9)
    r.finish()
    b.strings(); b.strings(); b.get('bQddddddd')
    nodes = b.get('Q')
    assert nodes == 1
    for _ in range(nodes):
        b.get('iddd')
    for _ in range(b.get('Q')):
        b.get('QBdddddQ')
        for _ in range(b.get('Q')):
            for _ in range(2):
                b.get('Qi'); b.blob(); b.get('bdddd')
                for _ in range(4):
                    b.vector()
            b.get('ddd')
        assert b.get('B') == 1
        b.get('d')
        for _ in range(b.get('Q')):
            b.get('Qiid')
        for _ in range(b.get('Q')):
            b.get('QiiBB'); b.get('dddddddddBddddd')
        assert b.get('B') == 0
        b.vector(); b.vector()
    for _ in range(b.get('Q')):
        b.get('ddddQBd')
        for _ in range(33):
            assert len(b.vector()) == nodes
    result = {}
    for _ in range(b.get('Q')):
        b.get('BdddddQ')
        receipt = b.get('Q')
        dt, remaining, factor = b.get('ddd')
        points = [b.get('Qiid') for _ in range(b.get('Q'))]
        before, after = b.vector(), b.vector()
        assert len(before) == len(after) == nodes and b.get('Q') == nodes
        discrepancy = b.vector()
        assert receipt not in result
        result[receipt] = dict(points=points, factor=factor, before=before[0], after=after[0],
                               discrepancy=sum(map(F, discrepancy), F(0)))
    for _ in range(b.get('Q')):
        b.get('iddd')
    for _ in range(b.get('Q')):
        b.get('i'); b.vector()
    b.finish()
    return result


records=[];bookings=[];shares=set();transitions=0;partitions=0
for sign in ['positive','negative']:
    tag=a.case_prefix+'-'+sign;legs={};grouped=defaultdict(list)
    for line in (native/(tag+'-legs.txt')).read_text().splitlines():
        c=line.split();step,rid,k,point,drain=map(int,c[:5]);ow,om,rw,rm,si=vals(' '.join(c[5:]))
        legs[step,rid,k]=(point,drain,ow,om,rw,rm,si);grouped[step,rid,point,drain].append((ow,om,rw,rm,si))
    rows={}
    for line in (native/(tag+'-rows.txt')).read_text().splitlines():
        identity,wp,mp=line.split('|');c=identity.split();key=tuple(map(int,c[:5]));w,m=vals(' '.join(c[5:]));rows[key]=(exact(w,vals(wp)),exact(m,vals(mp)))
    consumed=defaultdict(F);pending=defaultdict(F)
    for (step,rid,remaining,point,drain),(water,mass) in rows.items():
        raw=grouped.get((step,rid,point,drain),[])
        wi,mi=(2,3) if remaining else (0,1)
        assert water==sum((F(v[wi]) for v in raw),F(0)) and mass==sum((F(v[mi]) for v in raw),F(0))
        if not remaining:
            rest=rows[step,rid,1,point,drain];consumed[step]+=(mass-rest[1])*(1 if drain else -1)
            if not drain:pending[step]+=rest[1]
    previous_mass=previous_consumed=initial=None; schedule=[];case_shares=set()
    for line in (native/(tag+'-states.txt')).read_text().splitlines():
        c=line.split();step=int(c[0]);volume,oldv,conc,oldc,span,dt=vals(' '.join(c[1:7]));roundoff=sum(map(F,vals(' '.join(c[7:]))),F(0))
        if 5<=step<=8:schedule.append(str(F(dt)/F(span)))
        for (at,rid,k),leg in legs.items():
            if at!=step or (step-1,rid,k) not in legs:continue
            before=legs[step-1,rid,k]
            assert before[:4]==leg[:4] and before[6]==leg[6]
            for q in [4,5]:
                value,after=before[q],leg[q];piece=(value/span)*dt if span>dt else value
                assert after==value-piece and F(piece)+F(after)==F(value)
                if value>0:
                    if span>dt:assert piece>0 and after>0
                    else:assert after==0
                    shares.add(str(F(piece)/F(value)));case_shares.add(str(F(piece)/F(value)));partitions+=1
                records.append(dict(case=tag,step=step,receipt=rid,leg=k,quantity='water' if q==4 else 'material',before=value.hex(),slice=piece.hex(),after=after.hex(),span=span.hex(),dt=dt.hex()))
        mass=F(conc*volume);delivered=consumed[step]
        if step==0:initial=mass
        else:
            delta=delivered-previous_consumed;assert F(oldc*oldv)==previous_mass
            target=previous_mass+delta
            assert conc==float(F(float(previous_mass+F(float(delta))))/F(volume))
            assert abs(target-mass)<=F((2*2**-52)*abs(float(target)))
            if delta:
                baseline=float(previous_mass/F(volume));assert conc!=baseline and mass!=F(baseline*volume)
            transitions+=1
        assert mass+roundoff==initial+delivered and mass>=pending[step] and mass+roundoff>=pending[step]
        previous_mass,previous_consumed=mass,delivered
    assert schedule==['1/4','1/3','1/2','1'],(tag,schedule)
    assert {'1/4','1/3','1/2','1'}<=case_shares,(tag,case_shares)
    proof=defaultdict(list)
    for line in (native/(tag+'-points.txt')).read_text().splitlines():
        c=line.split();step,rid,point=map(int,c[:3]);si,factor=vals(' '.join(c[3:]));signed=0.;gross=F(0)
        raw=[(d,v) for d in [1,0] for v in grouped.get((step,rid,point,d),[])]
        assert not (grouped.get((step,rid,point,1)) and grouped.get((step,rid,point,0)))
        for d,v in raw:
            signed+=v[4] if d else -v[4];assert v[0]==v[4]*factor;gross+=F(v[0])*(1 if d else -1)
        assert signed==si;proof[step,rid].append((si*factor,gross))
    for (step,rid),points in proof.items():
        booked=0.
        for v,_ in points:booked+=v
        if step==4:assert (booked>0) if sign=='positive' else (booked<0),(tag,booked)
        gross=sum((v[1] for v in points),F(0));observed=water_publications(native/f'{tag}-{step}.owner')[rid]
        assert [p[3]*observed['factor'] for p in observed['points']]==[p[0] for p in points]
        assert observed['discrepancy']==F(booked)-gross and observed['after']==observed['before']+booked
        bookings.append(dict(case=tag,step=step,receipt=rid,booking=booked.hex(),discrepancy=str(observed['discrepancy']),queue_before=observed['before'].hex(),queue_after=observed['after'].hex()))
assert transitions==16 and partitions>0 and {'1/4','1/3','1/2','1'}<=shares
(out/'accounting.json').write_text(json.dumps(dict(receiver_transitions=transitions,positive_leg_partitions=partitions,represented_shares=sorted(shares),leg_records=records,water_bookings=bookings),indent=2)+'\n')
print(f'{transitions} receiver transitions and {partitions} positive native leg partitions conserve exact represented amounts; shares: {sorted(shares)}.')
