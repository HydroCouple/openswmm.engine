from pathlib import Path
import subprocess,sys,json
p=Path(__file__).resolve().parent;v=sys.argv[1] if len(sys.argv)>1 else 'baseline';src=p/f'src_{v}';out=p/f'build_{v}';out.mkdir(exist_ok=True)
files=['mesh/MeshBuilder.cpp','solver/ExplicitInertialSolver.cpp','solver/InertialEdges.cpp','solver/SurfaceFluxCalculator.cpp','coupling/NodeCoupling.cpp','subsurface/SubsurfaceSolver.cpp','subsurface/SigmaColumn.cpp','subsurface/SoilCharacteristic.cpp','subsurface/SubsurfaceData.cpp']
flags=['-std=c++20','-O3','-g','-DNDEBUG','-fno-fast-math','-ffp-contract=off','-fno-math-errno','-mcpu=native','-DSWMM_USE_OPENMP','-DSWMM_OS_MACOS','-Xpreprocessor','-fopenmp','-I/opt/homebrew/opt/libomp/include','-I'+str(src/'src/engine'),'-I'+str(src/'include'),'-I'+str(src/'include/openswmm/engine')]
if v=='profile':flags+=['-include',str(p/'profiler.hpp')]
objects=[];cmds=[]
for file in files+['harness.cpp']:
 f=p/file if file=='harness.cpp' else src/'src/engine/2d'/file;o=out/(f.stem+'.o');cmd=['clang++',*flags,'-c',str(f),'-o',str(o)];cmds.append(cmd);subprocess.run(cmd,check=True);objects.append(str(o))
cmd=['clang++',*objects,'-L/opt/homebrew/opt/libomp/lib','-lomp','-Wl,-rpath,/opt/homebrew/opt/libomp/lib','-o',str(out/'review')];cmds.append(cmd);subprocess.run(cmd,check=True);(out/'commands.json').write_text(json.dumps(cmds,indent=2))
