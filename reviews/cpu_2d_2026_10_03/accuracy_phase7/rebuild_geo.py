from pathlib import Path
import subprocess,shlex,json,concurrent.futures
p=Path(__file__).resolve().parent;root=p.parents[2];b=root/'build/flow-trace';src=p/'engine_src_baseline';out=p/'engine_common/geo';out.mkdir(exist_ok=True);target='src/engine/input/geopackage/libopenswmm.geopackage.a';lines=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(b),'-t','commands',target],text=True).splitlines();cmds=[];objects=[]
for line in lines:
 if ' -c 'not in line:continue
 cmd=shlex.split(line);obj=out/(Path(cmd[-1]).name+'.o');cmd=[x.replace(str(root/'src/engine'),str(src/'src/engine')).replace(str(root/'include'),str(src/'include'))for x in cmd];cmd[cmd.index('-o')+1]=str(obj);cmd[cmd.index('-MF')+1]=str(obj)+'.d';cmd[cmd.index('-MT')+1]=str(obj);cmds.append(cmd);objects.append(str(obj))
def compile(cmd):subprocess.run(cmd,cwd=b,check=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=4)as pool:list(pool.map(compile,cmds))
archive=out/'libopenswmm.geopackage.a'
if archive.exists():archive.unlink()
subprocess.run(['ar','qc',str(archive),*objects],check=True);subprocess.run(['ranlib',str(archive)],check=True)
links=json.loads((p/'support_link_commands.json').read_text())
for cmd in links:
 cmd=[str(archive)if x.endswith('libopenswmm.geopackage.a')else x for x in cmd];subprocess.run(cmd,cwd=b,check=True)
(p/'geo_commands.json').write_text(json.dumps(cmds,indent=2));print('Rebuilt GeoPackage archive against the same frozen C++ headers',flush=True)
