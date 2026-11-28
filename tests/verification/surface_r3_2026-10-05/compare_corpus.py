"""Run the current explicit 25-deck manifest; separate 2D API census covers no-aquifer/OFF paths."""
from pathlib import Path
import subprocess,json,hashlib,sys
root=Path.cwd();out=root/'tests/verification/surface_r3_2026-10-05';src=out/'isolated/source';dest=out/'isolated/corpus';dest.mkdir(exist_ok=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
executables={side:(out/'isolated'/side/'openswmm').resolve() for side in ['baseline','candidate']}
provenance={'base_commit':'35547cd082944622b5da459e56967e0e1c7d677c','scope':'Current explicit tests/parity/MANIFEST; the 24-case API census independently covers no-aquifer and infiltration OFF mesh trajectories. No recursive generated-fixture census.','build':'Same Release Ninja configuration and toolchain; candidate is base plus task.patch and the new ET test.','binaries':{side:{'cli_sha256':sha(cli),'engine_sha256':sha(cli.parent/'libopenswmm.engine.6.0.0.dylib'),'engine_link':subprocess.check_output(['otool','-L',str(cli)]).decode()} for side,cli in executables.items()}}
assert provenance['binaries']['baseline']['engine_sha256']!=provenance['binaries']['candidate']['engine_sha256']
rows=[]
for line in (src/'tests/parity/MANIFEST').read_text().splitlines():
 if not line.strip() or line.lstrip().startswith('#'):continue
 rel=line.split('\t')[0];deck=src/rel;assert deck.is_file();runs=[]
 for side,cli in executables.items():
  folder=dest/side/deck.stem;folder.mkdir(parents=True,exist_ok=True);binary=folder/(deck.stem+'.out')
  with (folder/'stdout.log').open('w') as log:p=subprocess.run([str(cli),str(deck),deck.stem+'.rpt',binary.name],cwd=folder,stdout=log,stderr=subprocess.STDOUT)
  runs.append({'returncode':p.returncode,'size':binary.stat().st_size if binary.exists() else 0,'sha256':sha(binary) if binary.exists() else None})
 row={'deck':rel,'input_sha256':sha(deck),'baseline':runs[0],'candidate':runs[1],'bit_identical':runs[0]['returncode']==runs[1]['returncode']==0 and runs[0]['size']>0 and runs[0]['sha256']==runs[1]['sha256']};rows.append(row);print(deck.stem,row['bit_identical'],flush=True)
(dest/'provenance.json').write_text(json.dumps(provenance,indent=2)+'\n');(dest/'summary.json').write_text(json.dumps(rows,indent=2)+'\n');assert len(rows)==25 and all(x['bit_identical'] for x in rows)
print('25/25 outputs byte-identical',flush=True)
