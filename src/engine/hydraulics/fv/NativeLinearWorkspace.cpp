// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "ImplicitNetworkSolver.hpp"

namespace openswmm::fv {
std::size_t ImplicitNetworkSolver::LinearWorkspace::factorBytes() const {
    return (source_row_.capacity()+row_.capacity()+diagonal_.capacity()+scatter_.capacity())*sizeof(std::size_t)+
        (source_column_.capacity()+column_.capacity())*sizeof(int)+scalar_lu_.capacity()*sizeof(double)+
        (block_lu_.capacity()+inverses_.capacity())*sizeof(std::array<double,4>);
}
std::size_t ImplicitNetworkSolver::LinearWorkspace::bytes() const {
    return factorBytes()+gmres_.workspaceBytes();
}
void ImplicitNetworkSolver::LinearWorkspace::release() {
    for (auto* v:{&source_row_,&row_,&diagonal_,&scatter_}) std::vector<std::size_t>().swap(*v);
    for (auto* v:{&source_column_,&column_}) std::vector<int>().swap(*v);
    std::vector<double>().swap(scalar_lu_);
    for (auto* v:{&block_lu_,&inverses_}) std::vector<std::array<double,4>>().swap(*v);
    gmres_.clearWorkspace(); valid_=false; pairs_=0;
}
bool ImplicitNetworkSolver::LinearWorkspace::matches(const Matrix& a,Preconditioner backend,std::size_t pairs) const {
    return valid_ && backend_==backend && pairs_==pairs && source_row_==a.row && source_column_==a.column;
}
} // namespace openswmm::fv
