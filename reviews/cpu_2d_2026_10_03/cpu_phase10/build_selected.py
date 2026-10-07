from pathlib import Path
import shutil,json,subprocess
p=Path(__file__).resolve().parent;shutil.copytree(p/'src_inlined',p/'src_selected',dirs_exist_ok=True)
# Compile the extended field-output harness for both sides of the comparison.
for v in ['baseline','selected']:subprocess.run(['python3',str(p/'build_variant.py'),v],check=True)
subprocess.run(['python3',str(p/'build_performance.py'),'baseline','selected'],check=True)
for v in ['baseline','selected']:subprocess.run(['python3',str(p/'build_engine.py'),v],check=True)
