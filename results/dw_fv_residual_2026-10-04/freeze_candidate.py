from pathlib import Path
import shutil,json,hashlib,shlex
R=Path(__file__).resolve().parent;B=R.parent/'dw_fv_p0_2026-10-03/build';C=R/'candidate';C.mkdir()
shutil.copyfile(B/'src/engine/libopenswmm.engine.6.0.0.dylib',C/'libopenswmm.engine.6.dylib')
for label in ('baseline','candidate'):
 d=R/label;shutil.copy2(B/'src/cli/openswmm',d/'openswmm');(d/'run').write_text('#!/bin/sh\nexport DYLD_LIBRARY_PATH='+shlex.quote(str(d))+'\nexport DYLD_PRINT_LIBRARIES=1\nexec '+shlex.quote(str(d/'openswmm'))+' "$@"\n');(d/'run').chmod(0o755)
manifest={label:{f:hashlib.sha256((R/label/f).read_bytes()).hexdigest() for f in ('libopenswmm.engine.6.dylib','openswmm')} for label in ('baseline','candidate')};(R/'binaries.json').write_text(json.dumps(manifest,indent=2))
