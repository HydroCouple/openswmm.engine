from pathlib import Path
import shutil,json,hashlib,subprocess
R=Path(__file__).resolve().parent;P=R.parent/'dw_fv_tier_2026-10-04';E=R.parents[1]
shutil.copytree(P/'source_baseline',R/'source_baseline');shutil.copytree(R/'source_baseline',R/'source_candidate');shutil.copytree(P/'baseline',R/'baseline');shutil.copyfile(P/'network_screen.py',R/'network_screen.py')
m=json.loads((P/'manifest.json').read_text());m['reference_head']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=E,text=True).strip();m['current_source_differences']=[p for p,h in m['files_sha256'].items() if (E/p).exists() and hashlib.sha256((E/p).read_bytes()).hexdigest()!=h]
for p in ['src/engine/hydraulics/fv/ExplicitFvSolver.cpp','src/engine/hydraulics/fv/ExplicitFvSolver.hpp','src/engine/core/PerfTimers.hpp']:assert hashlib.sha256((E/p).read_bytes()).hexdigest()==m['files_sha256'][p]
(R/'manifest.json').write_text(json.dumps(m,indent=2))
