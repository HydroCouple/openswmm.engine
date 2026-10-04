from pathlib import Path
import hashlib,json,re,shutil
p=Path(__file__).resolve().parent
# The last polish changed comments only. Verify token-equivalence before
# updating build snapshots, so the recorded compiled implementation is exact.
def tokens(s):
 s=re.sub(r'/\*.*?\*/','',s,flags=re.S);s=re.sub(r'//[^\n]*','',s);return ''.join(s.split())
for name in ['ExplicitInertialSolver.cpp','SweKernels.hpp']:
 a=p/'src_selected/src/engine/2d/solver'/name;b=p/'engine_src_selected/src/engine/2d/solver'/name;assert tokens(a.read_text())==tokens(b.read_text()),name;shutil.copy(a,b)
(p/'comment_verification.json').write_text(json.dumps(dict(comments_only=True,files=['ExplicitInertialSolver.cpp','SweKernels.hpp']),indent=2))
