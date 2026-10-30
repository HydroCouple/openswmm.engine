from pathlib import Path
import subprocess,difflib,json,hashlib,tarfile,io,shutil
root=Path.cwd();out=root/'tests/verification/surface_r3_2026-10-05';source=out/'isolated/source';source.parent.mkdir(exist_ok=True)
if not source.exists():
 source.mkdir();archive=subprocess.check_output(['git','archive','HEAD']);tarfile.open(fileobj=io.BytesIO(archive)).extractall(source,filter='data')
diff=[];records=[];local_updates=[]
for before in sorted((out/'before').rglob('*')):
 if not before.is_file():continue
 rel=before.relative_to(out/'before');work=root/rel
 if work.read_bytes()==before.read_bytes():continue
 result=subprocess.run(['git','show',f'HEAD:{rel}'],capture_output=True)
 if result.returncode:
  local_updates.extend(difflib.unified_diff(before.read_text().splitlines(True),work.read_text().splitlines(True),fromfile='a/'+str(rel),tofile='b/'+str(rel)));continue
 head=result.stdout;a=out/'isolated/merge-head';a.write_bytes(head)
 merge=subprocess.run(['git','merge-file','-p','--diff3',str(a),str(before),str(work)],capture_output=True)
 if merge.returncode:raise RuntimeError(str(rel)+' merge conflict '+merge.stdout.decode())
 merged=merge.stdout;dest=source/rel;dest.parent.mkdir(parents=True,exist_ok=True)
 if not dest.exists() or dest.read_bytes()!=merged:dest.write_bytes(merged)
 diff.extend(difflib.unified_diff(head.decode().splitlines(True),merged.decode().splitlines(True),fromfile='a/'+str(rel),tofile='b/'+str(rel)))
 records.append({'path':str(rel),'sha256':hashlib.sha256(merged).hexdigest()})
rel=Path('tests/unit/engine/test_surface_et_budget.cpp');(source/rel).write_bytes((root/rel).read_bytes());records.append({'path':str(rel),'sha256':hashlib.sha256((root/rel).read_bytes()).hexdigest()})
(out/'local_untracked_plan_updates.patch').write_text(''.join(local_updates));(out/'task.patch').write_text(''.join(diff));(out/'source_hashes.json').write_text(json.dumps({'base_commit':subprocess.check_output(['git','rev-parse','HEAD']).decode().strip(),'sources':records},indent=2)+'\n');print(len(records),'task source files isolated')
