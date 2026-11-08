#!/usr/bin/env python3
"""Qualify native pollutant basis identity without converting the numerical store."""
import argparse
from fractions import Fraction as F
from pathlib import Path
import json, os, struct, subprocess, sys, zlib
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);native=out/'native'
with (out/'authored.log').open('w') as log:
    subprocess.run([str(a.engine.resolve()),'--gtest_filter=ChangingVolumeNativeBases.Authored*','--gtest_output=xml:'+str(out/'authored.xml')],env=dict(os.environ,OPENSWMM_2D_BACKEND='cpu',OPENSWMM_CHANGING_VOLUME_EVIDENCE=str(native)),stdout=log,stderr=subprocess.STDOUT,check=True)

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


def identity(path):
    r=envelope(path.read_bytes(),'OpenSWMM live passive owner',12)
    header=r.get('dQQQddBBQddddQ')
    for _ in range(header[-1]):r.get('ii')
    b=envelope(r.blob(),'OpenSWMM changing-volume storage receipt owner',9);r.finish()
    assert b.strings()==[b'Tank'] and b.strings()==[b'Dye']
    basis=b.get('b');b.get('Qddddddd')
    for _ in range(b.get('Q')):b.get('iddd')
    rows=[];proofs={}
    for _ in range(b.get('Q')):
        receipt=b.get('QBdddddQ')[0]
        for _ in range(b.get('Q')):
            for _ in range(2):
                b.get('Qi');assert b.blob()==b'Dye';rows.append(b.get('b'));b.get('dddd')
                for _ in range(4):b.vector()
            b.get('ddd')
        assert b.get('B')==1;factor=b.get('d')
        for _ in range(b.get('Q')):b.get('Qiid')
        for k in range(b.get('Q')):
            point,cell,node,drain,applied=b.get('QiiBB')
            water,material,native_water,native_material,before,after,sample,start,end=b.get('ddddddddd')
            b.get('Bddddd')
            proofs[receipt,k]=dict(water=water,material=material,native_water=native_water,native_material=native_material,factor=factor)

        assert b.get('B')==0;b.vector();b.vector()
    return basis,rows,proofs
bases=[('mg',0,'MG/L','kg',F('0.001')),('ug',1,'UG/L','kg',F('0.000001')),('counts',2,'#/L','counts',F(1000))]
ft3_to_m3=F('0.028316846592')
expected_conversion=1/((0.3048*0.3048)*0.3048)
reports={};identities=[];physical=[];proofs_by_step={}
for label,code,token,unit,factor in bases:
    folder=out/label
    with (out/(label+'-audit.log')).open('w') as log:
        subprocess.run([sys.executable,str(Path(__file__).with_name('verify_exact_slices.py')),'--engine',str(a.engine.resolve()),'--output',str(folder),'--native-evidence',str(native),'--case-prefix','basis-'+label],stdout=log,stderr=subprocess.STDOUT,check=True)
    reports[label]=json.loads((folder/'accounting.json').read_text())
    for sign in ['positive','negative']:
        tag=f'basis-{label}-{sign}'
        for suffix in ['-states.txt','-rows.txt','-legs.txt','-points.txt']:
            assert (native/(tag+suffix)).read_bytes()==(native/(f'basis-mg-{sign}'+suffix)).read_bytes()
        for step in range(9):
            basis,rows,proofs=identity(native/f'{tag}-{step}.owner');proofs_by_step[tag,step]=proofs;assert basis==code and all(v==code for v in rows)
            identities.append(dict(case=tag,step=step,owner_basis=basis,typed_rows=len(rows)))
            if label!='mg':
                for suffix in ['.bin','.owner']:assert (native/f'{tag}-{step}{suffix}').read_bytes()!=(native/f'basis-mg-{sign}-{step}{suffix}').read_bytes()
        # Read each actual original transfer once, at its publication boundary.
        for line in (native/(tag+'-legs.txt')).read_text().splitlines():
            c=line.split();step,rid,k,point,drain=map(int,c[:5])
            if (rid,step) not in [(1,4),(2,8)]:continue
            water,mass,rw,rm,si=map(float.fromhex,c[5:]);assert water>0 and mass>0
            # Source leg material is recorded in C*m3; its native counterpart is C*ft3.
            # Both values have already undergone their actual represented native operations.
            # Physical interpretation uses the declared basis and the exact unit definition.
            # The once-converted amount can carry an observed conversion discrepancy.
            proof=proofs_by_step[tag,step][rid,k];conversion=proof['factor'];assert conversion==expected_conversion
            assert proof['native_water']==water and proof['native_material']==mass and proof['water']==si
            assert proof['native_water']==proof['water']*conversion and mass==proof['material']*conversion
            actual_source=F(proof['material'])*factor;actual_native=F(mass)*ft3_to_m3*factor
            physical.append(dict(case=tag,receipt=rid,leg=k,basis=token,physical_units=unit,native_amount=mass.hex(),
                source_amount=proof['material'].hex(),geometric_volume_conversion=conversion.hex(),
                native_physical_amount=str(actual_native),source_physical_amount=str(actual_source),
                represented_native_conversion_roundoff=str(F(mass)-F(proof['material'])*F(conversion)),
                physical_conversion_difference=str(actual_native-actual_source)))
assert all(r['receiver_transitions']==16 and r['positive_leg_partitions']==128 for r in reports.values())
assert len(identities)==54 and len(physical)==96
assert any(F(v["represented_native_conversion_roundoff"])!=0 for v in physical)
# Equal numeric C*volume means distinct physical quantities in mg/L and ug/L.
for i in range(32):
    assert F(physical[32+i]['native_physical_amount'])*1000==F(physical[i]['native_physical_amount'])
    assert physical[64+i]['physical_units']=='counts'
    assert F(physical[64+i]['native_physical_amount'])==F(physical[i]['native_physical_amount'])*1000000
    assert F(physical[64+i]['source_physical_amount'])==F(float.fromhex(physical[64+i]['source_amount']))*1000
summary=dict(bases=[dict(label=l,enum=c,authored_token=t,physical_units=u,surface_factor=str(f),native_factor=str(ft3_to_m3*f)) for l,c,t,u,f in bases],
    receiver_transitions=sum(r['receiver_transitions'] for r in reports.values()),positive_leg_partitions=sum(r['positive_leg_partitions'] for r in reports.values()),
    identities=identities,physical_interpretations=physical,numeric_traces_identical_across_bases=True,
    nonzero_represented_conversion_differences=sum(F(v["represented_native_conversion_roundoff"])!=0 for v in physical))
(out/'accounting.json').write_text(json.dumps(summary,indent=2)+'\n')
print('Three authored bases, both signs: 48 receiver transitions, 384 positive leg partitions, 54 typed restart boundaries and distinct kg/count interpretations pass.')
