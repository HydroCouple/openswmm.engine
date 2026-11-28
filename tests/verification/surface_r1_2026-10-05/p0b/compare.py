from pathlib import Path
import subprocess,sys,json,hashlib,os,struct
root=Path(__file__).resolve().parent; api=root.parent/'api_census'; base,candidate=map(lambda s:str(Path(s).resolve()),sys.argv[1:3])
rows=[]
for source in sorted(api.glob('*.inp')):
 for mode in ['control','conductivity','recovery']:
  deck=root/(mode+'_'+source.name);body=source.read_text()
  if mode=='conductivity':body+='\n[ADJUSTMENTS]\nCONDUCTIVITY '+' '.join(['.4']*12)+'\n'
  if mode=='recovery' and 'curve_number' in source.name:body=body.replace('CURVE_NUMBER 75 0 0.1 0 0', 'CURVE_NUMBER 75 0 0.5 0 0')
  if mode=='recovery':body+='\n[EVAPORATION]\nRECOVERY Recovery\n\n[PATTERNS]\nRecovery MONTHLY '+' '.join(['.2']*12)+'\n'
  if mode=='recovery' and 'green_ampt' in source.name:body=body.replace('END_TIME             02:00:00','END_TIME             23:59:00')
  deck.write_text(body);stats=[]
  for side,lib in [('p0a',base),('p0b',candidate)]:
   out=root/side/deck.stem;out.mkdir(parents=True,exist_ok=True)
   env=dict(os.environ,SURFACE_R1_WET_SECONDS='600',SURFACE_R1_SECOND_WET_SECONDS='72000' if mode=='recovery' and 'green_ampt' in source.name else '5400',OPENSWMM_GPU_BACKEND='cpu')
   with (out/'stdout.log').open('w') as f:subprocess.run([sys.executable,str(api/'compare.py'),'worker',lib,str(deck),str(out)],check=True,stdout=f,stderr=subprocess.STDOUT,env=env)
   stats.append(json.loads((out/'stats.json').read_text()))
  changed=stats[0]['series_sha256']!=stats[1]['series_sha256']
  method=source.stem.rsplit('_',1)[0]
  expected=mode=='conductivity' and method!='curve_number' or mode=='recovery' and method!='constant'
  if mode!='recovery' or method=='constant':assert changed==expected,(deck.name,stats,expected)
  rows.append({'deck':deck.name,'input_sha256':hashlib.sha256(deck.read_bytes()).hexdigest(),'cause':mode,'configured_factor_applies':expected,'changed':changed,'before':stats[0],'after':stats[1]})
# Actual mesh initialization plus a monthly crossing while a CONSTANT soil is ponded.
source=api/'constant_si.inp';deck=root/'monthly_constant_si.inp';body=source.read_text().replace('START_DATE           01/01/2026','START_DATE           01/31/2026').replace('START_TIME           00:00:00','START_TIME           23:00:00').replace('END_DATE             01/01/2026','END_DATE             02/01/2026').replace('END_TIME             02:00:00','END_TIME             01:00:00').replace('0.03        0.0','0.03        0.1');body+='\n[ADJUSTMENTS]\nCONDUCTIVITY .4 .8 '+' '.join(['1']*10)+'\n';deck.write_text(body)
stats=[]
for side,lib in [('p0a',base),('p0b',candidate)]:
 out=root/side/deck.stem;out.mkdir(parents=True,exist_ok=True)
 with (out/'stdout.log').open('w') as f:subprocess.run([sys.executable,str(api/'compare.py'),'worker',lib,str(deck),str(out)],check=True,stdout=f,stderr=subprocess.STDOUT)
 stats.append(json.loads((out/'stats.json').read_text()))
after=stats[1];physical=.5*.0254/3600
assert all(abs(x-.4*physical)<1e-15 for x in after['initial_rate_m_s']),after
blob=(root/'p0b'/deck.stem/'series.bin').read_bytes();width=8*(2+3*after['cells']);new_seen=False
for offset in range(0,len(blob),width):
 row=struct.unpack('d'*(width//8),blob[offset:offset+width]);t=row[0]*86400;rates=row[1+after['cells']:1+2*after['cells']]
 if t>3900:
  assert all(abs(x-.8*physical)<1e-15 for x in rates),(t,rates);new_seen=True
assert new_seen
rows.append({'deck':deck.name,'input_sha256':hashlib.sha256(deck.read_bytes()).hexdigest(),'cause':'month_boundary','changed':True,'before':stats[0],'after':stats[1]})
summary={'baseline_engine_sha256':hashlib.sha256(Path(base).read_bytes()).hexdigest(),'candidate_engine_sha256':hashlib.sha256(Path(candidate).read_bytes()).hexdigest(),'fixtures':rows,'changed':sum(r['changed'] for r in rows),'unchanged':sum(not r['changed'] for r in rows)}
(root/'affected_deck_manifest.json').write_text(json.dumps(summary,indent=2));print(summary['changed'],'intentional changes;',summary['unchanged'],'unchanged fixture trajectories; monthly initialization/cadence passed')
