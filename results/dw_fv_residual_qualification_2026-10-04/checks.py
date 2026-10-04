from pathlib import Path
import sys,os,json,subprocess,re,collections,hashlib,math
Q=Path(__file__).resolve().parent;R=Q.parent/'dw_fv_residual_2026-10-04';sys.path.insert(0,str(R))
import fixture,network_screen as net
if len(sys.argv)>1 and sys.argv[1]=='worker':
    _,_,label,case,t,out=sys.argv;t=int(t);net.ROOT=Q
    def deck(*args,**kw):
        s=fixture.deck(case,4,t,5,n=8)
        if 'rk2' in case:s=s.replace('[JUNCTIONS]','FV_TIME_INTEGRATION RK2\n[JUNCTIONS]')
        if 'culvert' in case:
            lines=s.splitlines();section=''
            for j,line in enumerate(lines):
                if line.startswith('['):section=line
                elif section=='[XSECTIONS]' and (line.startswith('D') or 'all' in case):lines[j]=line+' 1'
                elif section=='[CONDUITS]' and 'all' in case:
                    parts=line.split()
                    if parts and parts[0][0] in 'AB':parts[1],parts[2]=parts[2],parts[1];lines[j]=' '.join(parts)
            s='\n'.join(lines)+'\n'
        return s
    net.deck=deck;net.worker(label,'FV',4,t,5,False,out);sys.exit()
out=Q/'checks';out.mkdir(exist_ok=False);rows=[]
configs=[('free','global'),('free_rk2','global'),('transition_tpa','global'),('lts','macro'),('culvert',''),('culvert_all',''),('lts_culvert',''),('lts','')]
for case,reject in configs:
    ref=None
    for label,t in [('baseline',1),('candidate',1),('candidate',4),('candidate',8)]:
        name=f'{case}_{reject or "natural"}_{label}_t{t}';env={k:v for k,v in os.environ.items() if not k.startswith(('OMP_','KMP_','OPENSWMM_','SWMM_','DYLD_','FV_'))};env.update(OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',KMP_BLOCKTIME='0',OPENSWMM_FV_RESIDUAL_AUDIT='1',OPENSWMM_FV_OMP_MIN_NODES='1',OPENSWMM_PERF='1',FV_QUALIFY_REJECT=reject)
        cp=subprocess.run([sys.executable,__file__,'worker',label,case,str(t),str(out/name)],env=env,capture_output=True,text=True,timeout=180);(out/(name+'.log')).write_text(cp.stdout+cp.stderr);assert cp.returncode==0,(name,cp.stderr[-2000:])
        trace=collections.defaultdict(list);serial=[];teams=set();counts=collections.Counter();specials=set()
        for line in cp.stderr.splitlines():
            tag=line.split()[0] if line else ''
            if tag=='TEAM':teams.add(int(line.split()[2]))
            elif tag in ('I','R','E','H','SPECIAL'):
                trace[int(line.split()[1])].append(line);counts[tag]+=1
                if tag=='SPECIAL':specials.add(tuple(line.split()[3:]))
            elif tag in ('CELL','REJECT'):serial.append(line);counts[tag]+=1
            if tag in ('I','R','E','CELL'):assert not re.search(r'\b(nan|inf)\b',line,re.I),name
        assert teams=={t},(name,teams)
        assert counts['REJECT']==bool(reject),(name,counts)
        assert counts['I'] and counts['CELL']
        if 'culvert' in case:assert any(int(x[0])>=0 for x in specials),(name,specials)
        result=json.loads((out/name/'result.json').read_text());profilelines=re.findall(r'\[PERF-FV\] ([^\n]+)',cp.stderr);assert len(profilelines)==1
        prof={k:float(v) for k,v in re.findall(r'([\w.]+)=([0-9.e+-]+)',profilelines[0])}
        if reject:assert prof['n.rollback']>=1,(name,prof)
        if reject=='macro':assert prof['n.macrorej']>=1,(name,prof)
        sig=dict(nodes={n:hashlib.sha256('\n'.join(v).encode()).hexdigest() for n,v in trace.items()},serial=hashlib.sha256('\n'.join(serial).encode()).hexdigest(),output=result['output_sha256'],schedule=result['step_times_sha256'],counts={k:v for k,v in prof.items() if k.startswith('n.') and k not in ('n.area','n.width','n.i1')})
        if ref is None:ref=sig
        else:assert sig==ref,name
        row=dict(name=name,label=label,case=case,rejection=reject,threads=t,observed_teams=sorted(teams),records=dict(counts),restore_cell_records=sum(x.startswith('CELL restore') for x in serial),specials=sorted(specials),profile=prof,signature=sig,continuity_percent=result.get('continuity_percent'))
        rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2));print(name,'exact',dict(counts),flush=True)
