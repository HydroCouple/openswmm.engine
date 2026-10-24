from pathlib import Path
import json,subprocess,sys,os,hashlib,struct
r=Path(__file__).resolve().parent;manifest=json.loads((r/'affected_deck_manifest.json').read_text());lib=Path(sys.argv[1]).resolve();results=[]
for row in manifest['fixtures']:
 deck=r/row['deck'];out=r/'merged'/deck.stem;out.mkdir(parents=True,exist_ok=True);env=dict(os.environ,OPENSWMM_GPU_BACKEND='cpu')
 if row['cause']!='month_boundary':env.update(SURFACE_R1_WET_SECONDS='600',SURFACE_R1_SECOND_WET_SECONDS='72000' if row['cause']=='recovery' and 'green_ampt' in deck.name else '5400')
 with (out/'stdout.log').open('w') as f:subprocess.run([sys.executable,str(r.parent/'api_census/compare.py'),'worker',str(lib),str(deck),str(out)],check=True,env=env,stdout=f,stderr=subprocess.STDOUT)
 actual=json.loads((out/'stats.json').read_text());a=(r/'p0b'/deck.stem/'series.bin').read_bytes();b=(out/'series.bin').read_bytes();assert len(a)==len(b),deck.name
 n=actual['cells'];stride=2+3*n;aa=struct.unpack('d'*(len(a)//8),a);bb=struct.unpack('d'*(len(b)//8),b)
 maxima=[max(abs(aa[k]-bb[k]) for k in range(i,len(aa),stride)) for i in range(stride)]
 assert maxima[0]==0,deck.name
 assert max(maxima[1:1+3*n])<=1e-12,(deck.name,maxima)
 assert maxima[-1]<=1e-10*max(1,abs(actual['volume_m3'])),(deck.name,maxima)
 results.append({'deck':deck.name,'bit_identical':a==b,'max_depth_m':max(maxima[1:1+n]),'max_capacity_m_s':max(maxima[1+n:1+2*n]),'max_cumulative_depth_m':max(maxima[1+2*n:1+3*n]),'max_volume_m3':maxima[-1],'passes_compatibility_tolerances':True})
(r/'merged_summary.json').write_text(json.dumps({'merged_engine_sha256':hashlib.sha256(lib.read_bytes()).hexdigest(),'scope':'Mixed working tree compatibility check; unrelated source changes and LTO differ from the isolated candidate. P0a/P0b bit-identity claims use the isolated snapshots only.','depth_capacity_abs_tolerance':1e-12,'volume_relative_tolerance':1e-10,'fixtures':results},indent=2));print(len(results),'merged-source trajectories pass compatibility tolerances')
