// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_GROUNDWATER_DISPERSION_HPP
#define OPENSWMM_GROUNDWATER_DISPERSION_HPP
#include <algorithm>
#include <array>
#include <cmath>
namespace openswmm::twoD::dispersion {
// Inverse-distance least squares; the rank-one limit retains only the
// resolved direction instead of inventing an unobserved transverse slope.
struct GradientFit {
    double xx=0,xy=0,yy=0,bx=0,by=0;
    void add(double x,double y,double difference) {
        const double d=x*x+y*y;if(!(d>0))return;
        xx+=x*x/d;xy+=x*y/d;yy+=y*y/d;
        bx+=x*difference/d;by+=y*difference/d;
    }
    std::array<double,2> value()const {
        const double trace=xx+yy,det=xx*yy-xy*xy;
        if(!(trace>0))return {0,0};
        if(det>1e-12*trace*trace)return {(yy*bx-xy*by)/det,(xx*by-xy*bx)/det};
        return {bx/trace,by/trace};
    }
};
struct Tensor {
    double xx=0,xy=0,yy=0;
    std::array<double,2> apply(const std::array<double,2>& g)const {
        return {xx*g[0]+xy*g[1],xy*g[0]+yy*g[1]};
    }
};
inline Tensor tensor(double vx,double vy,double longitudinal,double transverse,double molecular) {
    const double speed=std::hypot(vx,vy),base=molecular+transverse*speed;
    if(!(speed>0))return {molecular,0,molecular};
    const double anisotropy=(longitudinal-transverse)/speed;
    return {base+anisotropy*vx*vx,anisotropy*vx*vy,base+anisotropy*vy*vy};
}
// Harmonic normal resistance with a deferred tangential/nonorthogonal
// correction. Exact for a linear field and constant tensor on skewed cells.
// a/b include the water cross-section per unit edge length (theta*h).
inline double faceFlux(Tensor a,Tensor b,double nx,double ny,double dl,double dr,
                       std::array<double,2> armL,std::array<double,2> armR,
                       std::array<double,2> gradL,std::array<double,2> gradR,
                       double left,double right,double width) {
    const auto ag=a.apply(gradL),bg=b.apply(gradR);
    const double an=nx*(a.xx*nx+a.xy*ny)+ny*(a.xy*nx+a.yy*ny);
    const double bn=nx*(b.xx*nx+b.xy*ny)+ny*(b.xy*nx+b.yy*ny);
    if(!(an>0&&bn>0&&dl>0&&dr>0))return 0;
    const double crossL=gradL[0]*armL[0]+gradL[1]*armL[1]-dl*(nx*ag[0]+ny*ag[1])/an;
    const double crossR=gradR[0]*armR[0]+gradR[1]*armR[1]+dr*(nx*bg[0]+ny*bg[1])/bn;
    return width*(left-right+crossL-crossR)/(dl/an+dr/bn);
}
}
#endif
