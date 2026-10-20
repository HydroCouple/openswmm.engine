// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "NativeGmres.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace openswmm::fv {
namespace {
bool finite(const double* x, std::size_t n) {
    for (std::size_t i=0;i<n;++i) if (!std::isfinite(x[i])) return false;
    return true;
}
double norm(const double* x, std::size_t n) {
    double value=0.0;
    for (std::size_t i=0;i<n;++i) value=std::hypot(value,x[i]);
    return value;
}
double dot(const double* a, const double* b, std::size_t n) {
    long double sum=0.0;
    for (std::size_t i=0;i<n;++i) sum+=static_cast<long double>(a[i])*b[i];
    return static_cast<double>(sum);
}
}

std::size_t NativeGmres::bytes() const {
    std::size_t count=0;
    for (const auto* v:{&basis_,&hessenberg_,&cosine_,&sine_,&g_,&y_,&x_,&r_,&w_,&z_,&update_,&image_})
        count+=v->capacity();
    return sizeof(double)*count;
}
void NativeGmres::release() {
    for (auto* v:{&basis_,&hessenberg_,&cosine_,&sine_,&g_,&y_,&x_,&r_,&w_,&z_,&update_,&image_})
        std::vector<double>().swap(*v);
}

NativeGmres::Result NativeGmres::solve(const std::vector<double>& b, const Apply& matrix,
                                     const Apply& inverse, const Controls& controls,
                                     std::vector<double>& output) {
    Result result;
    const auto n=b.size();
    if (!n || !matrix || controls.restart<=0 || controls.max_iterations<0 || controls.stagnant_restarts<=0 ||
        !std::isfinite(controls.absolute_tolerance) || controls.absolute_tolerance<0 ||
        !std::isfinite(controls.relative_tolerance) || controls.relative_tolerance<0 ||
        (controls.absolute_tolerance==0 && controls.relative_tolerance==0) || !finite(b.data(),n)) return result;
    const auto m=std::min<std::size_t>(n,std::min(controls.restart,std::max(1,controls.max_iterations)));
    if (bytes()>controls.workspace_limit_bytes) release();
    // Six length-n work vectors, (m+1)*n basis entries, (m+1)*m H,
    // three length-m arrays and one length-(m+1) least-squares RHS.
    const auto limit=std::min(controls.workspace_limit_bytes/sizeof(double),std::vector<double>().max_size());
    std::size_t count=0;
    auto addProduct=[&](std::size_t a,std::size_t c) {
        if (a && c>(limit-count)/a) return false;
        count+=a*c; return true;
    };
    if (!addProduct(m+1,n) || !addProduct(m+1,m) || !addProduct(6,n) || !addProduct(4,m) || !addProduct(1,1)) {
        result.status=Status::MemoryLimit; return result;
    }
    std::size_t retained=0;
    bool exceeds=false;
    auto request=[&](const std::vector<double>& v,std::size_t size) {
        const auto capacity=std::max(v.capacity(),size);
        if (capacity>limit-retained) exceeds=true; else retained+=capacity;
    };
    request(basis_,(m+1)*n); request(hessenberg_,(m+1)*m);
    request(cosine_,m); request(sine_,m); request(g_,m+1); request(y_,m);
    for (const auto* v:{&x_,&r_,&w_,&z_,&update_,&image_}) request(*v,n);
    if (exceeds) release();
    const bool had_workspace=bytes()>0;
    auto allocate=[&]() {
        auto size=[&](std::vector<double>& v,std::size_t size) {
            if (v.capacity()<size) { v.reserve(size); ++result.workspace_growths; }
            v.resize(size);
        };
        size(basis_,(m+1)*n); size(hessenberg_,(m+1)*m);
        size(cosine_,m); size(sine_,m); size(g_,m+1); size(y_,m);
        for (auto* v:{&x_,&r_,&w_,&z_,&update_,&image_}) size(*v,n);
    };
    try {
        allocate();
    } catch (const std::bad_alloc&) { release(); result.status=Status::MemoryLimit; return result; }
    result.workspace_bytes=bytes();
    result.reused_workspace=had_workspace && result.workspace_growths==0;
    if (result.workspace_bytes>controls.workspace_limit_bytes) {
        release(); result.status=Status::MemoryLimit; return result;
    }
    std::fill(x_.begin(),x_.end(),0.0);
    const double bnorm=norm(b.data(),n);
    result.tolerance=controls.absolute_tolerance+controls.relative_tolerance*bnorm;
    if (!std::isfinite(bnorm) || !std::isfinite(result.tolerance)) { result.status=Status::NonFinite; return result; }
    auto apply=[&](const double* in,double* out) {
        ++result.matvecs;
        if (!matrix(in,out)) { result.status=Status::OperatorFailure; return false; }
        if (!finite(out,n)) { result.status=Status::NonFinite; return false; }
        return true;
    };
    auto precondition=[&](const double* in,double* out) {
        if (inverse) {
            ++result.preconditioner_applications;
            if (!inverse(in,out)) { result.status=Status::PreconditionerFailure; return false; }
        } else std::copy_n(in,n,out);
        if (!finite(out,n)) { result.status=Status::NonFinite; return false; }
        return true;
    };
    auto trueResidual=[&]() {
        if (!apply(x_.data(),image_.data())) return false;
        for (std::size_t i=0;i<n;++i) r_[i]=b[i]-image_[i];
        result.true_residual=norm(r_.data(),n);
        if (!std::isfinite(result.true_residual)) { result.status=Status::NonFinite; return false; }
        return true;
    };
    if (!trueResidual()) return result;
    if (result.true_residual<=result.tolerance) { output=x_; result.status=Status::Converged; return result; }
    int stagnant=0;
    while (result.iterations<controls.max_iterations) {
        const double old_residual=result.true_residual;
        std::fill(hessenberg_.begin(),hessenberg_.end(),0.0); std::fill(g_.begin(),g_.end(),0.0);
        g_[0]=old_residual;
        for (std::size_t i=0;i<n;++i) basis_[i]=r_[i]/old_residual;
        std::size_t used=0;
        bool breakdown=false;
        for (std::size_t j=0;j<m && result.iterations<controls.max_iterations;++j) {
            if (!precondition(basis_.data()+j*n,z_.data()) || !apply(z_.data(),w_.data())) return result;
            const double before=norm(w_.data(),n);
            if (!std::isfinite(before)) { result.status=Status::NonFinite; return result; }
            auto h=[&](std::size_t row)->double& { return hessenberg_[j*(m+1)+row]; };
            for (std::size_t k=0;k<=j;++k) {
                h(k)=dot(basis_.data()+k*n,w_.data(),n);
                for (std::size_t i=0;i<n;++i) w_[i]-=h(k)*basis_[k*n+i];
            }
            double after=norm(w_.data(),n);
            if (after<.7*before) {
                ++result.reorthogonalizations;
                for (std::size_t k=0;k<=j;++k) {
                    const double correction=dot(basis_.data()+k*n,w_.data(),n); h(k)+=correction;
                    for (std::size_t i=0;i<n;++i) w_[i]-=correction*basis_[k*n+i];
                }
                after=norm(w_.data(),n);
            }
            if (!std::isfinite(after)) { result.status=Status::NonFinite; return result; }
            breakdown=after<=64*std::numeric_limits<double>::epsilon()*before;
            h(j+1)=after;
            if (!breakdown) for (std::size_t i=0;i<n;++i) basis_[(j+1)*n+i]=w_[i]/after;
            for (std::size_t k=0;k<j;++k) {
                const double a=h(k), bnext=h(k+1);
                h(k)=cosine_[k]*a+sine_[k]*bnext; h(k+1)=-sine_[k]*a+cosine_[k]*bnext;
            }
            const double rotation=std::hypot(h(j),h(j+1));
            ++result.iterations;
            if (!std::isfinite(rotation) || rotation==0.0) { result.status=Status::Breakdown; return result; }
            cosine_[j]=h(j)/rotation; sine_[j]=h(j+1)/rotation; h(j)=rotation; h(j+1)=0;
            const double a=g_[j]; g_[j]=cosine_[j]*a; g_[j+1]=-sine_[j]*a;
            result.estimated_residual=std::fabs(g_[j+1]); used=j+1;
            if (breakdown || result.estimated_residual<=result.tolerance) break;
        }
        for (std::size_t i=used;i-->0;) {
            double value=g_[i];
            for (std::size_t j=i+1;j<used;++j) value-=hessenberg_[j*(m+1)+i]*y_[j];
            y_[i]=value/hessenberg_[i*(m+1)+i];
            if (!std::isfinite(y_[i])) { result.status=Status::Breakdown; return result; }
        }
        std::fill(update_.begin(),update_.end(),0.0);
        for (std::size_t j=0;j<used;++j) for (std::size_t i=0;i<n;++i) update_[i]+=basis_[j*n+i]*y_[j];
        if (!finite(update_.data(),n) || !precondition(update_.data(),z_.data())) {
            if (result.status==Status::InvalidInput) result.status=Status::NonFinite;
            return result;
        }
        for (std::size_t i=0;i<n;++i) x_[i]+=z_[i];
        if (!finite(x_.data(),n)) { result.status=Status::NonFinite; return result; }
        if (!trueResidual()) return result;
        if (result.true_residual<=result.tolerance) {
            output=x_; result.status=Status::Converged; result.happy_breakdown=breakdown; return result;
        }
        if (breakdown) { result.status=Status::Breakdown; return result; }
        if (result.true_residual>=old_residual*(1.0-1.e-3)) ++stagnant; else stagnant=0;
        if (stagnant>=controls.stagnant_restarts) { result.status=Status::Stagnated; return result; }
        if (result.iterations<controls.max_iterations) ++result.restarts;
    }
    result.status=Status::IterationLimit;
    return result;
}
} // namespace openswmm::fv
