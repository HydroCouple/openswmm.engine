from pathlib import Path
import re,json,hashlib,shutil
p=Path(__file__).resolve().parent;root=p.parents[2];f=root/'tests/output/2d_perf_2026-09-06/bellinge116k_LI_30m.inp';s=f.read_text();data=p/'external_data';data.mkdir(exist_ok=True);deps=[]
for original in [Path('/Users/calebbuahin/Downloads/bellinge_2d/BellingeSWMM_v021_nopervious.2dm'),Path('/Users/calebbuahin/Downloads/bellinge_2d/rg_bellinge_Jun2010_Aug2021.dat')]:
 dest=data/original.name;shutil.copy(original,dest);s=s.replace(str(original),str(dest));deps.append(dict(source=str(original),snapshot=str(dest),sha256=hashlib.sha256(dest.read_bytes()).hexdigest()))
changes=dict(END_TIME='04:25:00',MOMENTUM_EQUATION='FULL_SWE',RECONSTRUCTION_ORDER='2',LTS_TIERS='1',BACKEND='CPU',CFL_NUMBER='.4')
for k,v in changes.items():
 if re.search('^'+k+r'\s+',s,re.M):s=re.sub('^'+k+r'\s+.*$',k+' '+v,s,flags=re.M)
 else:s=s.replace('[2D_OPTIONS]\n','[2D_OPTIONS]\n'+k+' '+v+'\n')
d=p/'engine_decks/bellinge_10min_swe2';d.mkdir(exist_ok=True);(d/'model.inp').write_text(s)
(p/'bellinge_provenance.json').write_text(json.dumps(dict(source=str(f),source_sha256=hashlib.sha256(f.read_bytes()).hexdigest(),sha256=hashlib.sha256(s.encode()).hexdigest(),changes=changes,dependencies=deps),indent=2))
f=p/'check_models.py';s=f.read_text().replace('import subprocess,json,os,hashlib,re,math','import subprocess,json,os,hashlib,re,math,sys');s=s.replace("for deck in sorted((p/'engine_decks').iterdir()):", "for deck in sorted((p/'engine_decks').iterdir()):\n if len(sys.argv)>1 and deck.name not in sys.argv[1:]:continue")
s=s.replace("p/'model_results.json'","p/('bellinge_results.json' if len(sys.argv)>1 else 'model_results.json')")
s=s.replace("or 'surface_' in deck.name:assert h5", "or 'surface_' in deck.name or 'bellinge' in deck.name:assert h5")
f.write_text(s)
with (p/'.gitignore').open('a')as f:f.write('/external_data/\n/engine_decks/\n/__pycache__/\n')
