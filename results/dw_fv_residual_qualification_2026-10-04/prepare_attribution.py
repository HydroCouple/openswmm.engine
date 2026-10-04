from pathlib import Path
import sys,difflib
Q=Path(__file__).resolve().parent;R=Q.parent/'dw_fv_residual_2026-10-04';W=Q.parent/'dw_fv_p0_2026-10-03';p=Path('src/engine/hydraulics/fv/ExplicitFvSolver.cpp');label=sys.argv[1];original=(R/f'source_{label}'/p).read_text();s='#include <ctime>\n'+original
old='''#pragma omp parallel for schedule(dynamic, 2) private(counters)
        for (int i = 0; i < nh; ++i)
            solveAlgebraicNode(heavy[static_cast<std::size_t>(i)],
                               dt_of(heavy[static_cast<std::size_t>(i)]), forcing);
        return;'''
new='''        // Isolated attribution only: clocks add overhead; never use as clean timing.
        struct alignas(64) Sample {double cpu=0, busy=0, wait=0; int count=0;};
        std::vector<Sample> samples(static_cast<std::size_t>(omp_get_max_threads()));
        int team=1;
        auto cpuClock=[] {struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts); return double(ts.tv_sec)+1.e-9*double(ts.tv_nsec);};
        const double region_start=omp_get_wtime();
#pragma omp parallel private(counters)
        {
            const int tid=omp_get_thread_num();
            if(tid==0) team=omp_get_num_threads();
            auto& x=samples[static_cast<std::size_t>(tid)];
            const double cpu_start=cpuClock();
#pragma omp for schedule(dynamic, 2) nowait
            for (int i=0;i<nh;++i) {
                const double start=omp_get_wtime();
                solveAlgebraicNode(heavy[static_cast<std::size_t>(i)],
                    dt_of(heavy[static_cast<std::size_t>(i)]),forcing);
                x.busy+=omp_get_wtime()-start; ++x.count;
            }
            x.cpu=cpuClock()-cpu_start;
            const double wait_start=omp_get_wtime();
#pragma omp barrier
            x.wait=omp_get_wtime()-wait_start;
        }
        const double elapsed=omp_get_wtime()-region_start;
        double cpu=0,busy=0,max_busy=0,max_wait=0;int count=0;
        for(const auto& x:samples) {cpu+=x.cpu;busy+=x.busy;max_busy=std::max(max_busy,x.busy);max_wait=std::max(max_wait,x.wait);count+=x.count;}
        std::fprintf(stderr,"NODETEAM %d %d %.9g %.9g %.9g %.9g %.9g %d\\n",nh,team,elapsed,cpu,busy,max_busy,max_wait,count);
        return;'''
assert s.count(old)==1;s=s.replace(old,new)
# Force the existing multithread branch through one-thread OpenMP too, for attribution only.
s=s.replace('if (nh >= kOmpMinNodes && omp_get_max_threads() > 1) {','if (nh >= kOmpMinNodes) {',1)
(Q/f'attribution_{label}.cpp').write_text(s);(Q/f'attribution_{label}.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='clean',tofile='attribution')));(W/'source'/p).write_text(s)
