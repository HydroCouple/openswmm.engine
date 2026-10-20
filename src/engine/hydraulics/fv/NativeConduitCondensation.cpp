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
using S=ImplicitNetworkSolver;
using Block=std::array<double,4>;
using Five=std::array<double,10>; // two rows, [AII^-1 bI, AII^-1 AIB(4)]
struct Interior {
    int cell=0;
    std::array<int,4> boundary{}; // protected original scalar indices
    Block upper{};
    Five response{};
};
struct Entry { int row, column; double value; };
using CS=S::CondensationStatus;

// Cached numeric/pattern/Krylov buffers remain owned during Schur assembly.
// Admit them under the SAME structured cap, evicting before new allocations
// when needed. The linear solve later receives the remaining budget.
bool accountCache(std::size_t bytes,std::size_t budget,S::LinearWorkspace* workspace,S::LinearResult& result) {
    if (bytes>budget) { result.condensation.status=CS::MemoryLimit; return false; }
    if (workspace && workspace->bytes()>budget-bytes) {
        workspace->release(); result.workspace.cache_released=true;
    }
    result.condensation.peak_workspace_bytes=std::max(result.condensation.peak_workspace_bytes,
        bytes+(workspace ? workspace->bytes() : 0));
    return true;
}

bool invert(Block a,Block& inverse,double& ratio) {
    double scale=0.0;
    for (double v:a) { if (!std::isfinite(v)) return false; scale=std::max(scale,std::fabs(v)); }
    if (!(scale>0.0)) { ratio=0.0; return false; }
    const bool swap=std::fabs(a[2])>std::fabs(a[0]);
    if (swap) { std::swap(a[0],a[2]); std::swap(a[1],a[3]); }
    ratio=std::fabs(a[0])/scale;
    if (ratio<=1.e-14) return false;
    const double multiplier=a[2]/a[0], pivot=a[3]-multiplier*a[1];
    ratio=std::min(ratio,std::fabs(pivot)/scale);
    if (!std::isfinite(pivot) || ratio<=1.e-14) return false;
    for (int c=0;c<2;++c) {
        const double first=(swap ? c==1 : c==0), second=(swap ? c==0 : c==1);
        inverse[2+c]=(second-multiplier*first)/pivot;
        inverse[c]=(first-a[1]*inverse[2+c])/a[0];
    }
    return std::all_of(inverse.begin(),inverse.end(),[](double v){return std::isfinite(v);});
}
Block product(const Block& a,const Block& b) {
    return {a[0]*b[0]+a[1]*b[2],a[0]*b[1]+a[1]*b[3],
            a[2]*b[0]+a[3]*b[2],a[2]*b[1]+a[3]*b[3]};
}
Five product(const Block& a,const Five& b) {
    Five out{};
    for (int r=0;r<2;++r) for (int c=0;c<5;++c) out[5*r+c]=a[2*r]*b[c]+a[2*r+1]*b[5+c];
    return out;
}
// A distinct, capped production double-precision kernel. The test oracle uses
// an independent long-double augmented matrix and global partial pivoting.
bool dense(const S::Matrix& m,const std::vector<double>& rhs,std::vector<double>& x,
           S::CondensationDiagnostics& diagnostics,std::size_t live,std::size_t budget) {
    const auto n=rhs.size();
    std::vector<double> a(n*n,0.0), b=rhs;
    const auto actual=(a.capacity()+b.capacity())*sizeof(double);
    if (actual>budget-live) return false;
    diagnostics.peak_workspace_bytes=std::max(diagnostics.peak_workspace_bytes,live+actual);
    for (std::size_t r=0;r<n;++r) for (auto t=m.row[r];t<m.row[r+1];++t) a[r*n+m.column[t]]=m.value[t];
    for (std::size_t c=0;c<n;++c) {
        std::size_t p=c;
        for (std::size_t r=c+1;r<n;++r) if (std::fabs(a[r*n+c])>std::fabs(a[p*n+c])) p=r;
        double scale=0.0;
        for (std::size_t j=c;j<n;++j) scale=std::max(scale,std::fabs(a[p*n+j]));
        const double ratio=scale>0 ? std::fabs(a[p*n+c])/scale : 0.0;
        diagnostics.minimum_interface_pivot_ratio=std::min(diagnostics.minimum_interface_pivot_ratio,ratio);
        if (!std::isfinite(ratio) || ratio<=1.e-14) { diagnostics.failed_interface_pivot=static_cast<int>(c); return false; }
        if (p!=c) {
            for (std::size_t j=0;j<n;++j) std::swap(a[p*n+j],a[c*n+j]);
            std::swap(b[p],b[c]);
        }
        for (std::size_t r=c+1;r<n;++r) {
            const double multiplier=a[r*n+c]/a[c*n+c]; a[r*n+c]=multiplier;
            for (std::size_t j=c+1;j<n;++j) a[r*n+j]-=multiplier*a[c*n+j];
            b[r]-=multiplier*b[c];
        }
    }
    x.resize(n);
    for (std::size_t r=n;r-->0;) {
        double value=b[r];
        for (std::size_t j=r+1;j<n;++j) value-=a[r*n+j]*x[j];
        x[r]=value/a[r*n+r]; if (!std::isfinite(x[r])) return false;
    }
    return true;
}

// The core uses the same native bounded dense/ILU-GMRES paths as the unpeeled
// interface. Tolerances always refer back to the ORIGINAL full-system budget.
bool interfaceSolve(const S::Matrix& a,const std::vector<double>& b,std::size_t cell_blocks,
                    const NativeGmres::Controls& linear,const S::CondensationControls& controls,
                    S::LinearResult& result,std::size_t live,double* image,std::vector<double>& x,S::LinearWorkspace* workspace) {
    auto& d=result.condensation; const auto n=b.size();
    if (live>controls.workspace_limit_bytes) { d.status=CS::MemoryLimit; return false; }
    if (!n) { x.clear(); return true; }
    double norm=0.0;
    for (double v:b) norm=std::hypot(norm,v);
    const double tolerance=std::min(linear.absolute_tolerance+linear.relative_tolerance*norm,.5*d.full_tolerance);
    const auto available=controls.workspace_limit_bytes-live;
    bool solved=false;
    if (!controls.reduced_gmres_only && n<=static_cast<std::size_t>(controls.dense_limit) &&
        n<=available/sizeof(double)/n && n*sizeof(double)<=available-n*n*sizeof(double)) {
        const auto required=n*n*sizeof(double)+n*sizeof(double);
        if (workspace && workspace->bytes()>available-required) {
            workspace->release(); result.workspace.cache_released=true;
        }
        const auto dense_live=live+(workspace ? workspace->bytes() : 0);
        d.used_dense=true; solved=dense(a,b,x,d,dense_live,controls.workspace_limit_bytes);
        if (solved) {
            d.core_true_residual=0.0;
            if (!a.apply(x.data(),image)) solved=false;
            for (std::size_t i=0;i<n && solved;++i)
                d.core_true_residual=std::hypot(d.core_true_residual,image[i]-b[i]);
            solved=solved && d.core_true_residual<=tolerance;
        }
    }
    if (!solved) {
        d.used_reduced_gmres=true;
        auto limits=linear; limits.workspace_limit_bytes=std::min(limits.workspace_limit_bytes,available);
        const double quarter=.25*d.full_tolerance;
        if (quarter>0.0) {
            limits.absolute_tolerance=std::min(limits.absolute_tolerance,quarter);
            if (norm>0.0 && quarter/norm>0.0)
                limits.relative_tolerance=std::min(limits.relative_tolerance,quarter/norm);
        }
        auto reduced=controls.preconditioner==S::Preconditioner::BlockIlu0 ?
            S::solveBlockLinear(a,b,cell_blocks,limits,x,workspace) : S::solveLinear(a,b,limits,x,workspace);
        const bool assembly_evicted=result.workspace.cache_released;
        result.workspace=reduced.workspace; result.workspace.cache_released|=assembly_evicted;
        d.reduced_workspace=reduced.workspace;
        d.reduced=reduced.gmres;
        d.reduced_block_ilu=reduced.block_ilu; result.block_ilu=reduced.block_ilu;
        result.gmres=reduced.gmres; result.shifted_pivots=reduced.shifted_pivots;
        result.preconditioner_bytes=reduced.preconditioner_bytes;
        d.peak_workspace_bytes=std::max(d.peak_workspace_bytes,live+reduced.preconditioner_bytes+reduced.gmres.workspace_bytes);
        d.core_true_residual=reduced.gmres.true_residual;
        if (!d.used_tree) d.reduced_true_residual=d.core_true_residual;
        if (!reduced.converged()) { d.status=CS::InterfaceFailure; return false; }
    }
    return true;
}

struct LeafNode {
    Block diagonal{}, upper{}; // upper = D^-1 A_leaf,parent after elimination
    std::array<double,2> right{}; // becomes D^-1 b_leaf
    int begin=0, size=1, parent=-1, degree=0;
    bool active=true;
};
struct LeafLink { int row, column; Block value{}; };
struct LeafEdge { int column; Block value{}; };

// Block graph is the union of actual nonzero row AND column couplings. A
// directed edge alone still prevents elimination as an isolated block. Leaf
// Schur updates only modify the parent diagonal/RHS, so they introduce no fill.
// Cell endpoints are 2-variable blocks; algebraic heads are scalar blocks.
bool treeSolve(const S::Matrix& a,const std::vector<double>& b,const std::vector<int>& retained,
               std::size_t cells,const NativeGmres::Controls& linear,
               const S::CondensationControls& controls,S::LinearResult& result,
               std::size_t live,double* image,std::vector<double>& x,S::LinearWorkspace* workspace) {
    auto& d=result.condensation; d.used_tree=true;
    const auto n=b.size(); std::size_t blocks=0;
    for (std::size_t i=0;i<n;++blocks) {
        const bool pair=retained[i]<static_cast<int>(2*cells);
        i+=pair ? 2 : 1;
    }
    auto fits=[&](std::initializer_list<std::pair<std::size_t,std::size_t>> terms) {
        std::size_t bytes=live;
        for (auto [count,size]:terms) {
            if (bytes>controls.workspace_limit_bytes ||
                (count && size>(controls.workspace_limit_bytes-bytes)/count)) {
                d.status=CS::MemoryLimit; return false;
            }
            bytes+=count*size;
        }
        return accountCache(bytes,controls.workspace_limit_bytes,workspace,result);
    };
    // Reserve worst-case symmetric graph contributions before multiplication.
    if (!fits({{n,2*sizeof(int)},{blocks,sizeof(LeafNode)+3*sizeof(int)},
               {blocks+1,sizeof(std::size_t)},{a.value.size(),2*(sizeof(LeafLink)+sizeof(LeafEdge))}})) return false;
    std::vector<int> block_of(n), core_map(n,-1), order, queue;
    order.reserve(blocks); queue.reserve(2*blocks);
    std::vector<LeafNode> nodes(blocks);
    for (std::size_t k=0,i=0;k<blocks;++k) {
        auto& node=nodes[k]; node.begin=static_cast<int>(i);
        node.size=retained[i]<static_cast<int>(2*cells) ? 2 : 1;
        for (int j=0;j<node.size;++j,++i) { block_of[i]=static_cast<int>(k); node.right[j]=b[i]; }
    }
    std::vector<LeafLink> links; links.reserve(2*a.value.size());
    for (std::size_t r=0;r<n;++r) for (auto t=a.row[r];t<a.row[r+1];++t) {
        const int col=a.column[t], rb=block_of[r], cb=block_of[col];
        const int rr=static_cast<int>(r)-nodes[rb].begin, cc=col-nodes[cb].begin;
        if (rb==cb) nodes[rb].diagonal[2*rr+cc]=a.value[t];
        else if (a.value[t]!=0.0) {
            LeafLink value{rb,cb,{}}; value.value[2*rr+cc]=a.value[t];
            links.push_back(value); links.push_back({cb,rb,{}});
        }
    }
    std::sort(links.begin(),links.end(),[](const LeafLink& a,const LeafLink& b) {
        return a.row<b.row || (a.row==b.row && a.column<b.column);
    });
    std::vector<std::size_t> row; row.reserve(blocks+1); row.push_back(0);
    std::vector<LeafEdge> edges; edges.reserve(links.size());
    for (std::size_t k=0,t=0;k<blocks;++k) {
        while (t<links.size() && links[t].row==static_cast<int>(k)) {
            LeafEdge edge{links[t].column,{}};
            do {
                for (int j=0;j<4;++j) edge.value[j]+=links[t].value[j];
                ++t;
            } while (t<links.size() && links[t].row==static_cast<int>(k) && links[t].column==edge.column);
            edges.push_back(edge);
        }
        row.push_back(edges.size()); nodes[k].degree=static_cast<int>(row[k+1]-row[k]);
        if (nodes[k].degree<=1) queue.push_back(static_cast<int>(k));
    }
    const auto persistent=block_of.capacity()*sizeof(int)+core_map.capacity()*sizeof(int)+
        (order.capacity()+queue.capacity())*sizeof(int)+nodes.capacity()*sizeof(LeafNode)+
        row.capacity()*sizeof(std::size_t)+edges.capacity()*sizeof(LeafEdge);
    if (!fits({{persistent,1},{links.capacity(),sizeof(LeafLink)}})) return false;
    std::vector<LeafLink>().swap(links);
    for (std::size_t q=0;q<queue.size();++q) {
        const int leaf=queue[q]; auto& node=nodes[leaf];
        if (!node.active || node.degree>1) continue;
        int parent=-1; Block upper{}, lower{};
        for (auto t=row[leaf];t<row[leaf+1];++t) if (nodes[edges[t].column].active) {
            parent=edges[t].column; upper=edges[t].value;
        }
        if (parent>=0) {
            auto first=edges.begin()+row[parent], last=edges.begin()+row[parent+1];
            const auto reverse=std::lower_bound(first,last,leaf,[](const LeafEdge& e,int col){ return e.column<col; });
            if (reverse==last || reverse->column!=leaf) { d.status=CS::InterfaceFailure; return false; }
            lower=reverse->value;
        }
        double scale=0.0, diagonal_scale=0.0;
        for (double v:node.diagonal) diagonal_scale=std::max(diagonal_scale,std::fabs(v));
        scale=diagonal_scale;
        for (const auto& block:{upper,lower}) for (double v:block) scale=std::max(scale,std::fabs(v));
        Block inverse{}; double ratio=0.0; bool safe=false;
        if (node.size==1) {
            ratio=scale>0.0 ? std::fabs(node.diagonal[0])/scale : 0.0;
            if (ratio>1.e-14) { inverse[0]=1.0/node.diagonal[0]; safe=std::isfinite(inverse[0]); }
        } else {
            safe=invert(node.diagonal,inverse,ratio);
            ratio=scale>0.0 ? ratio*(diagonal_scale/scale) : 0.0;
            safe=safe && ratio>1.e-14;
        }
        d.minimum_leaf_pivot_ratio=std::min(d.minimum_leaf_pivot_ratio,ratio);
        if (!safe) { ++d.unsafe_leaf_pivots; continue; } // retain the original equations in the core
        const auto response=std::array<double,2>{inverse[0]*node.right[0]+inverse[1]*node.right[1],
                                               inverse[2]*node.right[0]+inverse[3]*node.right[1]};
        const auto response_upper=product(inverse,upper);
        for (double v:response) if (!std::isfinite(v)) { d.status=CS::InterfaceFailure; return false; }
        for (double v:response_upper) if (!std::isfinite(v)) { d.status=CS::InterfaceFailure; return false; }
        node.right=response; node.upper=response_upper; node.parent=parent;
        node.active=false; order.push_back(leaf); d.tree_eliminated+=node.size;
        if (parent>=0) {
            auto& target=nodes[parent]; const auto update=product(lower,node.upper);
            for (int j=0;j<4;++j) {
                target.diagonal[j]-=update[j];
                if (!std::isfinite(target.diagonal[j])) { d.status=CS::InterfaceFailure; return false; }
            }
            for (int j=0;j<target.size;++j) {
                target.right[j]-=lower[2*j]*node.right[0]+lower[2*j+1]*node.right[1];
                if (!std::isfinite(target.right[j])) { d.status=CS::InterfaceFailure; return false; }
            }
            if (--target.degree<=1) queue.push_back(parent);
        }
    }
    std::size_t nc=0, entries=0, core_cells=0;
    for (std::size_t k=0;k<blocks;++k) if (nodes[k].active) {
        const auto& node=nodes[k];
        if (node.size==2) ++core_cells;
        for (int j=0;j<node.size;++j) core_map[node.begin+j]=static_cast<int>(nc++);
        entries+=node.size*node.size;
        for (auto t=row[k];t<row[k+1];++t) if (nodes[edges[t].column].active)
            entries+=node.size*nodes[edges[t].column].size;
    }
    d.core_scalars=static_cast<int>(nc); d.core_entries=entries;
    if (entries>controls.max_schur_entries) { d.status=CS::FillLimit; return false; }
    if (!fits({{persistent,1},{nc,2*sizeof(double)},{nc+1,sizeof(std::size_t)},
               {entries,sizeof(int)+sizeof(double)}})) return false;
    S::Matrix core; core.row.reserve(nc+1); core.column.reserve(entries); core.value.reserve(entries); core.row.push_back(0);
    std::vector<double> right; right.reserve(nc); std::vector<double> solution; solution.reserve(nc);
    // Merge the diagonal block with ordered edge blocks: the native ILU path
    // requires sorted scalar CSR rows. Zero reverse entries preserve the graph.
    for (std::size_t k=0;k<blocks;++k) if (nodes[k].active) {
        const auto& node=nodes[k];
        for (int r=0;r<node.size;++r) {
            auto append=[&](int col,const Block& value) {
                for (int j=0;j<nodes[col].size;++j) {
                    core.column.push_back(core_map[nodes[col].begin+j]); core.value.push_back(value[2*r+j]);
                }
            };
            bool diagonal=false;
            for (auto t=row[k];t<row[k+1];++t) if (nodes[edges[t].column].active) {
                if (!diagonal && edges[t].column>static_cast<int>(k)) { append(static_cast<int>(k),node.diagonal); diagonal=true; }
                append(edges[t].column,edges[t].value);
            }
            if (!diagonal) append(static_cast<int>(k),node.diagonal);
            core.row.push_back(core.value.size()); right.push_back(node.right[r]);
        }
    }
    const auto core_bytes=core.row.capacity()*sizeof(std::size_t)+core.column.capacity()*sizeof(int)+
        (core.value.capacity()+right.capacity()+solution.capacity())*sizeof(double);
    if (!fits({{persistent,1},{core_bytes,1}})) return false;
    if (!interfaceSolve(core,right,core_cells,linear,controls,result,live+persistent+core_bytes,image,solution,workspace)) return false;
    x.assign(n,0.0);
    for (std::size_t i=0;i<n;++i) if (core_map[i]>=0) x[i]=solution[core_map[i]];
    for (auto it=order.rbegin();it!=order.rend();++it) {
        const auto& node=nodes[*it];
        for (int r=0;r<node.size;++r) {
            double value=node.right[r];
            if (node.parent>=0) {
                const auto& parent=nodes[node.parent];
                for (int j=0;j<parent.size;++j) value-=node.upper[2*r+j]*x[parent.begin+j];
            }
            x[node.begin+r]=value;
        }
    }
    return true; // caller independently checks the original, unpeeled Schur matrix
}

S::LinearResult attempt(const NetworkMeshData& mesh,const S::Matrix& a,const std::vector<double>& rhs,
                        const NativeGmres::Controls& linear,const S::CondensationControls& controls,
                        std::vector<double>& output,S::LinearWorkspace* workspace) {
    S::LinearResult result; auto& d=result.condensation; d.status=CS::InvalidInput;
    const auto n=rhs.size(), cells=mesh.cell_dx.size();
    if (!n || n>static_cast<std::size_t>(std::numeric_limits<int>::max()) || cells>n/2 ||
        controls.dense_limit<0 || linear.restart<=0 || linear.max_iterations<0 || linear.stagnant_restarts<=0 ||
        (controls.preconditioner!=S::Preconditioner::ScalarIlu0 && controls.preconditioner!=S::Preconditioner::BlockIlu0) ||
        !std::isfinite(linear.absolute_tolerance) || linear.absolute_tolerance<0 ||
        !std::isfinite(linear.relative_tolerance) || linear.relative_tolerance<0 ||
        (linear.absolute_tolerance==0 && linear.relative_tolerance==0) ||
        a.row.size()!=n+1 || a.row.front()!=0 || a.row.back()!=a.value.size() || a.value.size()!=a.column.size() ||
        mesh.conduit_cell_begin.size()!=mesh.conduit_cell_count.size()) return result;
    double rhs_norm=0.0;
    for (std::size_t r=0;r<n;++r) {
        if (!std::isfinite(rhs[r]) || a.row[r]>a.row[r+1] || a.row[r+1]>a.value.size()) return result;
        rhs_norm=std::hypot(rhs_norm,rhs[r]); int previous=-1;
        for (auto t=a.row[r];t<a.row[r+1];++t) {
            if (a.column[t]<=previous || a.column[t]>=static_cast<int>(n) || !std::isfinite(a.value[t])) return result;
            previous=a.column[t];
        }
    }
    d.full_tolerance=linear.absolute_tolerance+linear.relative_tolerance*rhs_norm;
    if (!std::isfinite(d.full_tolerance)) return result;
    std::size_t covered=0, ni=0;
    for (std::size_t c=0;c<mesh.conduit_cell_begin.size();++c) {
        const int count=mesh.conduit_cell_count[c];
        if (count<=0 || mesh.conduit_cell_begin[c]!=static_cast<int>(covered) ||
            static_cast<std::size_t>(count)>cells-covered) return result;
        ni+=count>2 ? count-2 : 0; covered+=count;
    }
    if (covered!=cells) return result;
    const auto nb=n-2*ni;
    d.eliminated=static_cast<int>(2*ni); d.retained=static_cast<int>(nb);
    // Check allocation-size products before reserving. The budget covers
    // dynamic numeric/index buffers; caller A/b/output and object/stack overhead
    // are excluded. Capacities are rechecked after construction and assembly.
    auto fits=[&](std::initializer_list<std::pair<std::size_t,std::size_t>> terms) {
        std::size_t bytes=0;
        for (auto [count,size]:terms) {
            if (count && size>(controls.workspace_limit_bytes-bytes)/count) { d.status=CS::MemoryLimit; return false; }
            bytes+=count*size;
        }
        return accountCache(bytes,controls.workspace_limit_bytes,workspace,result);
    };
    if (!fits({{n,sizeof(int)},{cells,sizeof(int)},{nb,sizeof(int)},{ni,sizeof(Interior)},
               {nb,2*sizeof(double)},{n,2*sizeof(double)}})) return result;
    std::vector<int> retained_index(n,-1), interior_index(cells,-1), retained;
    retained.reserve(nb);
    std::vector<Interior> interiors; interiors.reserve(ni);
    for (std::size_t c=0;c<mesh.conduit_cell_begin.size();++c) {
        const int begin=mesh.conduit_cell_begin[c], end=begin+mesh.conduit_cell_count[c];
        for (int i=begin+1;i<end-1;++i) {
            interior_index[i]=static_cast<int>(interiors.size());
            Interior item; item.cell=i; item.boundary={2*begin,2*begin+1,2*(end-1),2*(end-1)+1};
            interiors.push_back(item);
        }
    }
    for (std::size_t i=0;i<n;++i) if (i>=2*cells || interior_index[i/2]<0) {
        retained_index[i]=static_cast<int>(retained.size()); retained.push_back(static_cast<int>(i));
    }
    std::vector<double> b(nb), x; x.reserve(nb);
    std::vector<double> candidate(n), image(n);
    const auto base_bytes=retained_index.capacity()*sizeof(int)+interior_index.capacity()*sizeof(int)+
        retained.capacity()*sizeof(int)+interiors.capacity()*sizeof(Interior)+
        (b.capacity()+x.capacity()+candidate.capacity()+image.capacity())*sizeof(double);
    if (!fits({{base_bytes,1}})) return result;
    // Factor each eligible J_II chain once while solving five simultaneous
    // right-hand sides (history plus its four protected endpoint coordinates).
    for (std::size_t c=0;c<mesh.conduit_cell_begin.size();++c) {
        const int begin=mesh.conduit_cell_begin[c], end=begin+mesh.conduit_cell_count[c];
        for (int cell=begin+1;cell<end-1;++cell) {
            auto& item=interiors[interior_index[cell]];
            Block diagonal{}, lower{}, upper{}; Five right{};
            for (int r=0;r<2;++r) {
                const int row=2*cell+r; right[5*r]=rhs[row];
                for (auto t=a.row[row];t<a.row[row+1];++t) {
                    const int col=a.column[t]; const double value=a.value[t];
                    if (value==0.0) continue;
                    if (col/2==cell) diagonal[2*r+col%2]=value;
                    else if (cell>begin+1 && col/2==cell-1) lower[2*r+col%2]=value;
                    else if (cell<end-2 && col/2==cell+1) upper[2*r+col%2]=value;
                    else {
                        const auto hit=std::find(item.boundary.begin(),item.boundary.end(),col);
                        if (hit==item.boundary.end()) { d.status=CS::UnsupportedStencil; return result; }
                        right[5*r+1+(hit-item.boundary.begin())]=value;
                    }
                }
            }
            if (cell>begin+1) {
                const auto& previous=interiors[interior_index[cell-1]];
                const auto du=product(lower,previous.upper); const auto dr=product(lower,previous.response);
                for (int i=0;i<4;++i) diagonal[i]-=du[i];
                for (int i=0;i<10;++i) right[i]-=dr[i];
            }
            Block inverse{}; double ratio=0.0;
            const bool safe=invert(diagonal,inverse,ratio);
            d.minimum_block_pivot_ratio=std::min(d.minimum_block_pivot_ratio,ratio);
            if (!safe) { d.failed_cell=cell; d.status=CS::BlockPivot; return result; }
            item.upper=product(inverse,upper); item.response=product(inverse,right);
            for (double value:item.response) if (!std::isfinite(value)) { d.status=CS::BlockPivot; d.failed_cell=cell; return result; }
        }
        for (int cell=end-3;cell>begin;--cell) {
            auto& item=interiors[interior_index[cell]];
            const auto correction=product(item.upper,interiors[interior_index[cell+1]].response);
            for (int i=0;i<10;++i) item.response[i]-=correction[i];
        }
    }
    std::size_t contributions=nb;
    for (int r:retained) for (auto t=a.row[r];t<a.row[r+1];++t) {
        const auto extra=retained_index[a.column[t]]>=0 ? 1u : 4u;
        if (contributions>std::numeric_limits<std::size_t>::max()-extra) { d.status=CS::MemoryLimit; return result; }
        contributions+=extra;
    }
    if (!fits({{base_bytes,1},{contributions,sizeof(Entry)+sizeof(int)+sizeof(double)},
               {nb+1,sizeof(std::size_t)}})) return result;
    std::vector<Entry> entries; entries.reserve(contributions);
    for (std::size_t r=0;r<nb;++r) {
        const int original=retained[r]; b[r]=rhs[original]; entries.push_back({static_cast<int>(r),static_cast<int>(r),0.0});
        for (auto t=a.row[original];t<a.row[original+1];++t) {
            const int col=a.column[t], mapped=retained_index[col]; const double value=a.value[t];
            if (mapped>=0) entries.push_back({static_cast<int>(r),mapped,value});
            else {
                const auto& item=interiors[interior_index[col/2]]; const int component=col%2;
                b[r]-=value*item.response[5*component];
                for (int k=0;k<4;++k) entries.push_back({static_cast<int>(r),retained_index[item.boundary[k]],-value*item.response[5*component+1+k]});
            }
        }
    }
    std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){
        return a.row<b.row || (a.row==b.row && a.column<b.column);
    });
    S::Matrix schur; schur.row.reserve(nb+1); schur.column.reserve(contributions); schur.value.reserve(contributions);
    schur.row.push_back(0); std::size_t t=0;
    for (std::size_t r=0;r<nb;++r) {
        while (t<entries.size() && entries[t].row==static_cast<int>(r)) {
            const int col=entries[t].column; double value=0.0;
            do { value+=entries[t++].value; } while (t<entries.size() && entries[t].row==static_cast<int>(r) && entries[t].column==col);
            if (!std::isfinite(value) || !std::isfinite(b[r])) { d.status=CS::InterfaceFailure; return result; }
            if (schur.value.size()>=controls.max_schur_entries) { d.status=CS::FillLimit; return result; }
            schur.column.push_back(col); schur.value.push_back(value);
        }
        schur.row.push_back(schur.value.size());
    }
    d.schur_entries=schur.value.size();
    const auto live=base_bytes+schur.row.capacity()*sizeof(std::size_t)+
        schur.column.capacity()*sizeof(int)+schur.value.capacity()*sizeof(double);
    if (!fits({{live,1},{entries.capacity(),sizeof(Entry)}})) return result;
    std::vector<Entry>().swap(entries); // release assembly storage before a solve
    const bool solved=controls.tree_interface ?
        treeSolve(schur,b,retained,cells,linear,controls,result,live,image.data(),x,workspace) :
        interfaceSolve(schur,b,cells-ni,linear,controls,result,live,image.data(),x,workspace);
    if (!solved) return result;
    // Leaf reconstruction must satisfy the original, UNPEELED Schur equations.
    double reduced_norm=0.0;
    for (double value:b) reduced_norm=std::hypot(reduced_norm,value);
    const double reduced_tolerance=std::min(linear.absolute_tolerance+linear.relative_tolerance*reduced_norm,
                                             .5*d.full_tolerance);
    d.reduced_true_residual=0.0;
    if (!schur.apply(x.data(),image.data())) { d.status=CS::ReconstructionFailure; return result; }
    for (std::size_t i=0;i<nb;++i) d.reduced_true_residual=std::hypot(d.reduced_true_residual,image[i]-b[i]);
    if (!std::isfinite(d.reduced_true_residual) || d.reduced_true_residual>reduced_tolerance) {
        d.status=CS::ReconstructionFailure; return result;
    }
    for (std::size_t i=0;i<nb;++i) candidate[retained[i]]=x[i];
    for (const auto& item:interiors) for (int r=0;r<2;++r) {
        double value=item.response[5*r];
        for (int k=0;k<4;++k) value-=item.response[5*r+1+k]*candidate[item.boundary[k]];
        candidate[2*item.cell+r]=value;
    }
    if (!a.apply(candidate.data(),image.data())) { d.status=CS::ReconstructionFailure; return result; }
    for (std::size_t i=0;i<n;++i) d.full_true_residual=std::hypot(d.full_true_residual,image[i]-rhs[i]);
    if (!std::isfinite(d.full_true_residual) || d.full_true_residual>d.full_tolerance) { d.status=CS::ReconstructionFailure; return result; }
    output=std::move(candidate); d.status=CS::Converged; return result;
}
} // namespace

ImplicitNetworkSolver::LinearResult ImplicitNetworkSolver::solveCondensed(
    const NetworkMeshData& mesh,const Matrix& a,const std::vector<double>& rhs,
    const NativeGmres::Controls& linear,const CondensationControls& controls,
    bool allow_fallback,std::vector<double>& output,LinearWorkspace* workspace) {
    LinearResult result; bool evicted=false;
    auto drop=[&]() {
        if (workspace && workspace->bytes()) { workspace->release(); evicted=true; }
    };
    auto finish=[&]() {
        if (workspace) {
            if (!result.converged()) drop();
            result.workspace.enabled=true; result.workspace.retained_bytes=workspace->bytes();
            result.workspace.cache_released|=evicted;
        }
        return result;
    };
    if (workspace && workspace->bytes()>std::min(linear.workspace_limit_bytes,controls.workspace_limit_bytes)) drop();
    try { result=attempt(mesh,a,rhs,linear,controls,output,workspace); }
    catch (const std::bad_alloc&) { result.condensation.status=CondensationStatus::MemoryLimit; }
    if (result.condensation.status==CondensationStatus::Converged) return finish();
    drop(); // no reduced cache may coexist with the original-matrix fallback
    if (!allow_fallback || result.condensation.status==CondensationStatus::InvalidInput) return finish();
    const auto diagnostics=result.condensation;
    evicted|=result.workspace.cache_released;
    result=controls.preconditioner==Preconditioner::BlockIlu0 ?
        solveBlockLinear(a,rhs,mesh.cell_dx.size(),linear,output,workspace) : solveLinear(a,rhs,linear,output,workspace);
    result.condensation=diagnostics; result.fallback=true;
    result.condensation.peak_workspace_bytes=std::max(result.condensation.peak_workspace_bytes,
        result.preconditioner_bytes+result.gmres.workspace_bytes);
    return finish();
}
} // namespace openswmm::fv
