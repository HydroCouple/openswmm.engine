from pathlib import Path
import sys,os
import network_screen as net

def deck(case,cells,threads,duration,n=256):
    n=int(os.environ.get('FV_TEST_GROUPS',str(n)))
    initial,A,B,D=(1.05,1.1,1.09,1.0)
    if 'rest' in case:initial,A,B,D=(1.,1.,1.,1.)
    if 'transition' in case:initial,A,B,D=(3.9,4.2,4.1,3.8)
    if 'pressure' in case:initial,A,B,D=(4.3,4.4,4.35,4.2)
    if 'dry' in case:initial,A,B,D=(0.,1.1,.5,0.)
    lts='lts' in case
    s=f'''[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING FV
SURCHARGE_METHOD SLOT
START_DATE 01/01/2026
END_DATE 01/01/2026
START_TIME 00:00:00
END_TIME {duration//3600:02d}:{duration//60%60:02d}:{duration%60:02d}
REPORT_STEP 1
ROUTING_STEP {5 if lts else .25}
THREADS {threads}
FV_MIN_CELLS {cells}
FV_LTS {'YES' if lts else 'NO'}
FV_ORDER {2 if 'order2' in case else 1}
'''
    if 'tpa' in case:s+='FV_PRESSURE_CLOSURE TPA\n'
    s+='[JUNCTIONS]\n'+''.join(f'J{i} 0 10 {initial} 0 0\n' for i in range(n))
    s+='[OUTFALLS]\n'+''.join(f'A{i} 0 FIXED {A} NO\nB{i} 0 FIXED {B} NO\nD{i} 0 FIXED {D} NO\n' for i in range(n))
    s+='[CONDUITS]\n'
    for i in range(n):
        for leg,a,b in [('A',f'A{i}',f'J{i}'),('B',f'B{i}',f'J{i}'),('D',f'J{i}',f'D{i}')]:
            if 'reverse' in case and leg=='B':a,b=b,a
            length=5 if lts and leg=='A' else 40
            s+=f'{leg}{i} {a} {b} {length} .013 0 0\n'
    s+='[XSECTIONS]\n'+''.join(f'{b}{i} CIRCULAR {3.5 if "mixed" in case and b=="B" else 4} 0 0 0 1\n' for i in range(n) for b in 'ABD')
    if 'gate' in case:s+='[LOSSES]\n'+''.join(f'A{i} 0 0 0 YES\n' for i in range(n))
    return s+'[REPORT]\nNODES ALL\nLINKS ALL\n'
if __name__=='__main__':
    label,case,cells,t,duration,out=sys.argv[1:];cells=int(cells);t=int(t);duration=int(duration)
    net.deck=lambda *a,**kw:deck(case,cells,t,duration)
    if case=='chain':
        scope={'__file__':str(Path(__file__).with_name('network_graded.py'))}
        source=Path(scope['__file__']).read_text();exec(source[:source.index('if len(sys.argv)==5:')],scope);net.deck=scope['deck']
    net.worker(label,'FV',cells,t,duration,False,out)
