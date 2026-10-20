// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include "FvKernels.hpp"
#include <array>

namespace openswmm::fv::implicit_wet {
// Native forward directional arithmetic. Five local variables at most: two
// cell (h,Q) pairs and one node head. No global seeds, allocation or inner solve.
struct Jet {
    double v=0.0;
    std::array<double,5> d{};
    Jet() = default;
    Jet(double value):v(value) {}
    static Jet seed(double value,int column) { Jet x(value); x.d[column]=1.0; return x; }
};
inline Jet operator+(const Jet& a,const Jet& b) {
    Jet r(a.v+b.v); for (int i=0;i<5;++i) r.d[i]=a.d[i]+b.d[i]; return r;
}
inline Jet operator-(const Jet& a,const Jet& b) {
    Jet r(a.v-b.v); for (int i=0;i<5;++i) r.d[i]=a.d[i]-b.d[i]; return r;
}
inline Jet operator*(const Jet& a,const Jet& b) {
    Jet r(a.v*b.v); for (int i=0;i<5;++i) r.d[i]=a.d[i]*b.v+a.v*b.d[i]; return r;
}
inline Jet operator/(const Jet& a,const Jet& b) {
    Jet r(a.v/b.v); for (int i=0;i<5;++i) r.d[i]=(a.d[i]-r.v*b.d[i])/b.v; return r;
}
inline Jet mapped(double value,double slope,const Jet& x) {
    Jet r(value); for (int i=0;i<5;++i) r.d[i]=slope*x.d[i]; return r;
}
inline Jet minimum(const Jet& a,const Jet& b) { return b.v<a.v ? b : a; }
inline Jet maximum(const Jet& a,const Jet& b) { return a.v<b.v ? b : a; }
inline bool finite(const Jet& x) {
    return std::isfinite(x.v) && std::all_of(x.d.begin(),x.d.end(),[](double d){return std::isfinite(d);});
}
struct Closure { Jet area,width,moment; };
inline Closure closure(const FvClosure& c,const Jet& h) {
    const auto e=kernels::closureEval(c,h.v);
    double second=0.0;
    if (h.v<c.y_full) {
        if (c.kind==kClosurePolynomial) second=2.0*c.c2;
        else {
            int i; double t; kernels::closureLocate(c,h.v,i,t);
            second=((12*t-6)*c.A[i]+(6*t-4)*c.M[i]*c.dh+
                (-12*t+6)*c.A[i+1]+(6*t-2)*c.M[i+1]*c.dh)*c.inv_dh*c.inv_dh;
        }
    }
    return {mapped(e.a,e.t,h),mapped(e.t,second,h),mapped(e.i1,e.a,h)};
}
inline double radiusSlope(const FvClosure& c,double h) {
    if (h>=c.y_full) return 0.0;
    if (c.kind==kClosurePolynomial) {
        const auto e=kernels::closureEval(c,h); const double p=c.p0+c.p1*h;
        return (e.t*p-e.a*c.p1)/(p*p);
    }
    int i; double t; kernels::closureLocate(c,h,i,t);
    return (c.R[i+1]-c.R[i])*c.inv_dh;
}
struct State { Jet a,q,u,c,i1; uint8_t press=0; };
inline State state(const FvClosure& table,const Jet& h,const Jet& velocity,uint8_t press) {
    const auto e=closure(table,h);
    const double speed=kernels::celerity(e.area.v,e.width.v);
    Jet c(speed);
    for (int i=0;i<5;++i) c.d[i]=.5*speed*(e.area.d[i]/e.area.v-e.width.d[i]/e.width.v);
    return {e.area,e.area*velocity,velocity,c,e.moment,press};
}
struct Flux { Jet mass,momentum; };
// Differentiate exactly the wet branch of kernels::waveSpeeds/riemannFlux.
// Branches are selected at the current state, including RH min/max bounds;
// their numerical values (celerity, jump speed, pressure) are NOT frozen.
// At ties use the same first-operand choice as std::min/std::max. Residual
// evaluation still calls the shared kernels, and tests enforce primal parity.
inline Flux flux(const State& l,const State& r) {
    auto sl=minimum(l.u-l.c,r.u-r.c), sr=maximum(l.u+l.c,r.u+r.c);
    if (l.press!=r.press && l.press<2 && r.press<2) {
        if (l.press) {
            const auto da=l.a-r.a;
            const auto w=da.v>0.0 ? (l.q-r.q)/da : sr;
            sr=maximum(r.u+r.c,minimum(sr,w));
        } else {
            const auto da=r.a-l.a;
            const auto w=da.v>0.0 ? (r.q-l.q)/da : sl;
            sl=minimum(l.u-l.c,maximum(sl,w));
        }
    }
    const auto fl=l.q*l.u+kernels::kGravity*l.i1, fr=r.q*r.u+kernels::kGravity*r.i1;
    if (sl.v>=0.0) return {l.q,fl};
    if (sr.v<=0.0) return {r.q,fr};
    return {(sr*l.q-sl*r.q+sl*sr*(r.a-l.a))/(sr-sl),
            (sr*fl-sl*fr+sl*sr*(r.q-l.q))/(sr-sl)};
}
} // namespace openswmm::fv::implicit_wet
