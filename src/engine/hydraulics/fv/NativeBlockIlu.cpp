// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "ImplicitNetworkSolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace openswmm::fv {
namespace {
using Block=std::array<double,4>;
Block product(const Block& a,const Block& b) {
    return {a[0]*b[0]+a[1]*b[2],a[0]*b[1]+a[1]*b[3],
            a[2]*b[0]+a[3]*b[2],a[2]*b[1]+a[3]*b[3]};
}
bool inverse(Block a,int width,double scale,Block& out,double& ratio) {
    out={}; ratio=0.0;
    for (double v:a) if (!std::isfinite(v)) return false;
    if (width==1) {
        ratio=std::fabs(a[0])/scale;
        if (!(ratio>1.e-12)) return false;
        out[0]=1.0/a[0]; return std::isfinite(out[0]);
    }
    const bool swap=std::fabs(a[2])>std::fabs(a[0]);
    if (swap) { std::swap(a[0],a[2]); std::swap(a[1],a[3]); }
    ratio=std::fabs(a[0])/scale;
    if (!(ratio>1.e-12)) return false;
    const double multiplier=a[2]/a[0], pivot=a[3]-multiplier*a[1];
    ratio=std::min(ratio,std::fabs(pivot)/scale);
    if (!std::isfinite(pivot) || !(ratio>1.e-12)) return false;
    for (int c=0;c<2;++c) {
        const double first=(swap ? c==1 : c==0), second=(swap ? c==0 : c==1);
        out[2+c]=(second-multiplier*first)/pivot;
        out[c]=(first-a[1]*out[2+c])/a[0];
    }
    return std::all_of(out.begin(),out.end(),[](double v){return std::isfinite(v);});
}
}

ImplicitNetworkSolver::LinearResult ImplicitNetworkSolver::solveBlockLinear(
    const Matrix& a,const std::vector<double>& rhs,std::size_t pairs,
    const NativeGmres::Controls& controls,std::vector<double>& output,LinearWorkspace* workspace) {
    LinearResult result; auto& d=result.block_ilu; d.used=true;
    LinearWorkspace local; auto& work=workspace ? *workspace : local;
    result.workspace.enabled=workspace!=nullptr;
    auto drop=[&]() {
        if (work.bytes()) { work.release(); result.workspace.cache_released=true; }
    };
    auto finish=[&]() {
        if (workspace) {
            result.preconditioner_bytes=std::max(result.preconditioner_bytes,work.factorBytes());
            if (!result.converged()) drop();
            result.workspace.retained_bytes=work.bytes();
        }
        return result;
    };
    if (workspace && work.bytes()>controls.workspace_limit_bytes) drop();
    const auto n=rhs.size();
    if (!n || n>static_cast<std::size_t>(std::numeric_limits<int>::max()) || pairs>n/2 ||
        a.row.size()!=n+1 || a.row.front()!=0 || a.row.back()!=a.value.size() || a.column.size()!=a.value.size() ||
        controls.restart<=0 || controls.max_iterations<0 || controls.stagnant_restarts<=0 ||
        !std::isfinite(controls.absolute_tolerance) || controls.absolute_tolerance<0.0 ||
        !std::isfinite(controls.relative_tolerance) || controls.relative_tolerance<0.0 ||
        (controls.absolute_tolerance==0.0 && controls.relative_tolerance==0.0)) return finish();
    for (std::size_t r=0;r<n;++r) {
        if (!std::isfinite(rhs[r]) || a.row[r]>a.row[r+1] || a.row[r+1]>a.value.size()) return finish();
        int previous=-1;
        for (auto t=a.row[r];t<a.row[r+1];++t) {
            if (a.column[t]<=previous || a.column[t]>=static_cast<int>(n) || !std::isfinite(a.value[t])) return finish();
            previous=a.column[t];
        }
    }
    const bool reuse=workspace && work.matches(a,Preconditioner::BlockIlu0,pairs);
    if (workspace && !reuse) drop();
    const auto blocks=n-pairs; d.blocks=static_cast<int>(blocks);
    const auto blockOf=[&](std::size_t scalar){return static_cast<int>(scalar<2*pairs ? scalar/2 : scalar-pairs);};
    const auto begin=[&](std::size_t block){return block<pairs ? 2*block : block+pairs;};
    const auto width=[&](std::size_t block){return block<pairs ? 2 : 1;};
    auto fits=[&](std::initializer_list<std::pair<std::size_t,std::size_t>> terms) {
        std::size_t bytes=0;
        for (auto [count,size]:terms) {
            if (count && size>(controls.workspace_limit_bytes-bytes)/count) {
                result.gmres.status=NativeGmres::Status::MemoryLimit; return false;
            }
            bytes+=count*size;
        }
        return true;
    };
    // Merge at most two ordered scalar rows directly into block rows. Setup
    // needs no sort/contribution matrix or dense graph scratch. Missing diagonal
    // blocks and internal structural zeros belong ONLY to the preconditioner.
    auto visit=[&](std::size_t i,const auto& emit) {
        const auto first=begin(i); const int size=width(i);
        std::array<std::size_t,2> pos={a.row[first],size==2 ? a.row[first+1] : 0};
        const std::array<std::size_t,2> end={a.row[first+1],size==2 ? a.row[first+2] : 0};
        bool diagonal=false;
        while (pos[0]<end[0] || pos[1]<end[1]) {
            int col=std::numeric_limits<int>::max();
            for (int r=0;r<size;++r) if (pos[r]<end[r]) col=std::min(col,blockOf(a.column[pos[r]]));
            if (!diagonal && col>static_cast<int>(i)) { emit(static_cast<int>(i),Block{}); diagonal=true; }
            Block value{};
            for (int r=0;r<size;++r) while (pos[r]<end[r] && blockOf(a.column[pos[r]])==col) {
                const int scalar=a.column[pos[r]], component=scalar<static_cast<int>(2*pairs) ? scalar%2 : 0;
                value[2*r+component]=a.value[pos[r]++];
            }
            emit(col,value); if (col==static_cast<int>(i)) diagonal=true;
        }
        if (!diagonal) emit(static_cast<int>(i),Block{});
    };
    try {
        auto& row=work.row_; auto& diagonal=work.diagonal_;
        auto& column=work.column_; auto& lu=work.block_lu_; auto& inverses=work.inverses_;
        if (!reuse) {
            if (!fits({{blocks+1,sizeof(std::size_t)},{blocks,sizeof(std::size_t)}})) return finish();
            row.resize(blocks+1); diagonal.resize(blocks); row[0]=0;
            result.preconditioner_bytes=work.factorBytes();
            if (!fits({{result.preconditioner_bytes,1}})) return finish();
            std::size_t entries=0;
            for (std::size_t i=0;i<blocks;++i) {
                visit(i,[&](int,const Block&){++entries;}); row[i+1]=entries;
            }
            d.entries=entries;
            if (!fits({{row.capacity()+diagonal.capacity(),sizeof(std::size_t)},
                       {entries,sizeof(int)+sizeof(Block)},{blocks,sizeof(Block)},
                       {workspace ? a.row.size()+a.value.size() : 0,sizeof(std::size_t)},
                       {workspace ? a.column.size() : 0,sizeof(int)}})) return finish();
            column.reserve(entries); lu.reserve(entries); inverses.resize(blocks);
            for (std::size_t i=0;i<blocks;++i) visit(i,[&](int col,const Block& value) {
                if (col==static_cast<int>(i)) diagonal[i]=column.size();
                column.push_back(col); lu.push_back(value);
            });
            if (workspace) {
                if (entries>std::numeric_limits<std::size_t>::max()/4) {
                    result.gmres.status=NativeGmres::Status::MemoryLimit; return finish();
                }
                work.scatter_.resize(a.value.size());
                for (std::size_t r=0;r<n;++r) {
                    const auto rb=blockOf(r);
                    for (auto t=a.row[r];t<a.row[r+1];++t) {
                        const int cb=blockOf(a.column[t]);
                        const auto hit=std::lower_bound(column.begin()+row[rb],column.begin()+row[rb+1],cb);
                        const int component=a.column[t]<static_cast<int>(2*pairs) ? a.column[t]%2 : 0;
                        work.scatter_[t]=4*static_cast<std::size_t>(hit-column.begin())+2*(r-begin(rb))+component;
                    }
                }
                work.source_row_=a.row; work.source_column_=a.column;
                work.backend_=Preconditioner::BlockIlu0; work.pairs_=pairs; work.valid_=true;
                result.workspace.symbolic_rebuilt=true;
            }
        } else {
            d.entries=lu.size(); result.workspace.symbolic_reused=true;
            std::fill(lu.begin(),lu.end(),Block{});
            for (std::size_t t=0;t<a.value.size();++t) lu[work.scatter_[t]/4][work.scatter_[t]%4]=a.value[t];
        }
        result.preconditioner_bytes=work.factorBytes();
        if (!fits({{result.preconditioner_bytes,1}})) return finish();
        for (std::size_t i=0;i<blocks;++i) {
            d.failed_block=static_cast<int>(i);
            for (auto t=row[i];t<diagonal[i];++t) {
                const int j=column[t]; lu[t]=product(lu[t],inverses[j]); // RIGHT block inverse
                for (auto u=diagonal[j]+1;u<row[j+1];++u) {
                    const auto first=column.begin()+t+1, last=column.begin()+row[i+1];
                    const auto hit=std::lower_bound(first,last,column[u]);
                    if (hit!=last && *hit==column[u]) {
                        auto& target=lu[hit-column.begin()]; const auto update=product(lu[t],lu[u]);
                        for (int k=0;k<4;++k) target[k]-=update[k];
                    }
                }
            }
            double scale=1.0;
            for (auto t=row[i];t<row[i+1];++t) for (double v:lu[t]) {
                if (!std::isfinite(v)) { result.gmres.status=NativeGmres::Status::PreconditionerFailure; return finish(); }
                scale=std::max(scale,std::fabs(v));
            }
            const auto original=lu[diagonal[i]]; bool safe=false;
            for (double shift:{0.0,.01,.1,1.0}) {
                auto trial=original;
                for (int r=0;r<width(i);++r) trial[3*r]+=std::copysign(shift*scale,original[3*r]);
                double ratio=0.0; safe=inverse(trial,width(i),scale,inverses[i],ratio);
                d.minimum_pivot_ratio=std::min(d.minimum_pivot_ratio,ratio);
                if (safe) {
                    lu[diagonal[i]]=trial;
                    if (shift>0.0) { ++result.shifted_pivots; d.largest_shift=std::max(d.largest_shift,shift*scale); }
                    break;
                }
            }
            if (!safe) { result.gmres.status=NativeGmres::Status::PreconditionerFailure; return finish(); }
        }
        d.failed_block=-1;
        auto apply=[&](const double* x,double* y){return a.apply(x,y);};
        auto precondition=[&](const double* x,double* y) {
            for (std::size_t i=0;i<blocks;++i) for (int r=0;r<width(i);++r) {
                const auto scalar=begin(i)+r; double value=x[scalar];
                for (auto t=row[i];t<diagonal[i];++t) for (int c=0;c<width(column[t]);++c)
                    value-=lu[t][2*r+c]*y[begin(column[t])+c];
                if (!std::isfinite(value)) return false;
                y[scalar]=value;
            }
            for (std::size_t i=blocks;i-->0;) {
                std::array<double,2> value{};
                for (int r=0;r<width(i);++r) {
                    value[r]=y[begin(i)+r];
                    for (auto t=diagonal[i]+1;t<row[i+1];++t) for (int c=0;c<width(column[t]);++c)
                        value[r]-=lu[t][2*r+c]*y[begin(column[t])+c];
                }
                for (int r=0;r<width(i);++r) {
                    y[begin(i)+r]=inverses[i][2*r]*value[0]+inverses[i][2*r+1]*value[1];
                    if (!std::isfinite(y[begin(i)+r])) return false;
                }
            }
            return true;
        };
        auto limits=controls; limits.workspace_limit_bytes-=result.preconditioner_bytes;
        result.gmres=work.gmres_.solve(rhs,apply,precondition,limits,output);
    } catch (const std::bad_alloc&) { result.gmres.status=NativeGmres::Status::MemoryLimit; }
    return finish();
}
} // namespace openswmm::fv
