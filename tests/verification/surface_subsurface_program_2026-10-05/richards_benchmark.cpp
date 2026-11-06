#include "hydrology/RichardsColumn.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace openswmm::richards;
Column make(int n, double se) {
    Column c; c.options.enabled=true; c.cells.push_back({.1,1,1,.1}); c.water.push_back(.02);
    for(int k=0;k<n;++k) {
        Cell x; x.dz=x.volume=1./n; x.area=1; x.z=1-(k+.5)/n; x.theta_s=.45;
        x.Ks=1e-5; x.wilting=.1; x.material={.05,2,1.6,.5,1e-4};
        c.cells.push_back(x); c.water.push_back(x.volume*(.05+.4*se));
    }
    return c;
}
int main(int argc,char**argv) {
    if(argc<2)return 2; std::ofstream csv(argv[1]);
    csv<<"columns,cells_per_column,seconds_simulated,wall_seconds,accepted,rejected,rhs,balance_m3\n";
    const bool quick=argc>2 && std::string(argv[2])=="quick";
    const double duration=quick ? 120.0 : 600.0;
    if(argc==2 || quick) for(int n:{8,32,64})for(int lanes:{1,quick?16:100}) {
        std::vector<Column> columns; for(int a=0;a<lanes;++a)columns.push_back(make(n,.2+.6*(a%17)/16.));
        std::vector<Column*> pointers; for(auto& c:columns)pointers.push_back(&c);
        auto start=std::chrono::steady_clock::now(); auto reports=openswmm::richards::advance(pointers,duration);
        auto wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        int accepted=0,rejected=0,rhs=0; double balance=0;
        for(const auto& r:reports){if(!r.ok){std::cerr<<r.error<<'\n';return 1;}accepted+=r.accepted;rejected+=r.rejected;rhs+=r.rhs;balance=std::max(balance,std::abs(r.balance));}
        csv<<lanes<<','<<n<<','<<duration<<','<<wall<<','<<accepted<<','<<rejected<<','<<rhs<<','<<balance<<'\n';csv.flush();
    }
    for(int n:{8,16,32,64}) {
        auto c=make(n,.3);c.options.atol=1e-9;c.options.rtol=1e-7;std::vector<Column*> p{&c};auto r=openswmm::richards::advance(p,600.);if(!r[0].ok)return 1;
        std::ofstream profile(std::string(argv[1])+".profile"+std::to_string(n)+".csv");profile.precision(17);profile<<"z,water,theta,pressure\n";
        for(int i=0;i<=n;++i)profile<<c.cells[i].z<<','<<c.water[i]<<','<<c.water[i]/c.cells[i].volume<<','<<(i?pressure(c.cells[i].material,.45,c.water[i]/c.cells[i].volume):c.water[i])<<'\n';
    }
}
