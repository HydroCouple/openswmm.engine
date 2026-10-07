from pathlib import Path
import shutil,re,json,hashlib
p=Path(__file__).resolve().parent;q=p.parent/'cpu_phase6';decks=p/'engine_decks';decks.mkdir(exist_ok=True)
shutil.copy(q/'run_engine.py',p/'run_engine.py');rows=[]
def emit(name,s,origin,changes):
 d=decks/name;d.mkdir(exist_ok=True);(d/'model.inp').write_text(s);rows.append(dict(case=name,origin=str(origin),changes=changes,sha256=hashlib.sha256(s.encode()).hexdigest()))
def options(s,values):
 a=re.search(r'^\[2D_OPTIONS\][ \t]*$',s,re.M).start();b=s.find('\n[',a+1);b=len(s)if b<0 else b;sec=s[a:b]
 for key,value in values.items():
  sec=re.sub(r'^'+key+r'\s+.*\n','',sec,flags=re.M)
  sec+='\n'+key+' '+str(value)+'\n'
 return s[:a]+sec+s[b:]
for f in sorted((q/'engine_decks').glob('*/model.inp')):
 emit(f.parent.name,f.read_text(),f,'Unchanged phase-6 qualification fixture')
 if f.parent.name in ['parking_lot','two_way_groundwater']:
  for closure in ['FLAT','VFR']:
   cfg=dict(MOMENTUM_EQUATION='FULL_SWE',RECONSTRUCTION_ORDER=2,LTS_TIERS=1,CELL_CLOSURE=closure)
   emit(f.parent.name+'_swe2_'+closure.lower(),options(f.read_text(),cfg),f,cfg)
f=q/'engine_decks/surface_output_species32/model.inp';s=f.read_text()
s=re.sub(r'^\[(?:POLLUTANTS|2D_INITIAL_QUALITY)\][\s\S]*?(?=^\[|\Z)','',s,flags=re.M)
# Initialize a wet surface so this small-duration test actually exercises second-order reconstruction.
s=re.sub(r'(^\[2D_TRIANGLES\]\n)([\s\S]*?)(?=^\[)',lambda m:m[1]+''.join(' '.join(line.split()[:4]+['0.03'])+'\n'if line.strip()and not line.lstrip().startswith(';')else line+'\n'for line in m[2].splitlines()),s,flags=re.M)
for threads in [1,4]:
 cfg=dict(MOMENTUM_EQUATION='FULL_SWE',RECONSTRUCTION_ORDER=2,LTS_TIERS=1,CELL_CLOSURE='FLAT')
 t=re.sub(r'^THREADS .*$',f'THREADS {threads}',options(s,cfg),flags=re.M)
 emit(f'surface_wet_swe2_t{threads}',t,f,dict(**cfg,THREADS=threads,removed=['POLLUTANTS','2D_INITIAL_QUALITY'],initial_depth_m=.03))
(p/'deck_provenance.json').write_text(json.dumps(rows,indent=2))
s=(q/'check_models.py').read_text().replace("q=p.parent/'cpu_phase5'","q=p")
s=s.replace("gw['active']==('groundwater' in deck.name or 'dunne' in deck.name)","gw['active']==('groundwater' in deck.name or 'dunne' in deck.name)")
s=s.replace("if deck.name in ['parking_lot','surface_output_species32']:assert h5", "if 'parking_lot' in deck.name or 'surface_' in deck.name:assert h5")
# An order-2 fixture must never silently fall back to first order.
s=s.replace("result.check_returncode()", "result.check_returncode()\n  if 'swe2' in deck.name: assert 'using first order' not in result.stderr")
(p/'check_models.py').write_text(s)
