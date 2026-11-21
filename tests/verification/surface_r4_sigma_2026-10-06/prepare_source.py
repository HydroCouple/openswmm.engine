from pathlib import Path
import subprocess,os,json,hashlib,tarfile,io,shutil
repo=Path(__file__).resolve().parents[3]
e=Path(__file__).resolve().parent
baseline=json.loads((e/'baseline.json').read_text())
subprocess.run(['git','merge-base','--is-ancestor',baseline['head'],'HEAD'],cwd=repo,check=True)
paths=list(baseline['files'])+['tests/unit/engine/test_sigma_conservation.cpp','plans/SURFACE_PROCESS_R4_SIGMA_CONSERVATION_2026-10-06.md']
index=e/'qualification-index';index.unlink(missing_ok=True)
env=dict(os.environ,GIT_INDEX_FILE=str(index))
subprocess.run(['git','read-tree',baseline['head']],cwd=repo,env=env,check=True)
subprocess.run(['git','add','-f','--',*paths],cwd=repo,env=env,check=True)
tree=subprocess.check_output(['git','write-tree'],cwd=repo,env=env).decode().strip()
dest=e/'isolated/openswmm.engine';dest.mkdir(parents=True,exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive',tree],cwd=repo))) as arc:
 for member in arc:
  target=dest/member.name
  if member.isfile() and target.exists() and target.read_bytes()==arc.extractfile(member).read():continue
  if member.isdir() and target.is_dir():continue
  arc.extract(member,dest,filter='data')
source=repo.parent/'openswmm.gui/tests/verification/surface_r4_2026-10-05/isolated/openswmm.engine'
for path in dest.rglob('*'):
 if not path.is_file():continue
 target=source/path.relative_to(dest);target.parent.mkdir(parents=True,exist_ok=True)
 if not target.exists() or target.read_bytes()!=path.read_bytes():shutil.copy2(path,target)
for path in (source/'src').rglob('*'):
 if path.is_file() and path.suffix in {'.cpp','.c','.h','.hpp'}:
  assert (dest/path.relative_to(source)).is_file(),str(path)
manifest={'head':baseline['head'],'task_source_tree':tree,'build_source_path':str(source),
 'files':{p:hashlib.sha256((repo/p).read_bytes()).hexdigest() for p in paths}}
for p,digest in manifest['files'].items():
 assert digest==hashlib.sha256((dest/p).read_bytes()).hexdigest()==hashlib.sha256((source/p).read_bytes()).hexdigest()
(e/'source-audit.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'tree':tree,'selected_files':len(paths),'source':str(source)}))
