// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_FOOTPRINT_GEOMETRY_HPP
#define OPENSWMM_FOOTPRINT_GEOMETRY_HPP
#include <algorithm>
#include <functional>
#include <cmath>
#include <numeric>
#include <limits>
#include <string>
#include <vector>
namespace openswmm::twoD::footprint {
struct Point { double x=0,y=0; };
using Ring=std::vector<Point>;
inline double cross(Point a,Point b,Point c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
inline double signedArea(const Ring& p){
    if(p.size()<3)return 0;double sum=0;
    for(std::size_t i=1;i+1<p.size();++i)sum+=cross(p[0],p[i],p[i+1]);
    return .5*sum;
}
inline double tolerance(const Ring& p){
    if(p.empty())return 0;double span=0;
    for(auto v:p)span=std::max(span,std::max(std::abs(v.x-p[0].x),std::abs(v.y-p[0].y)));
    return 128*std::numeric_limits<double>::epsilon()*std::max(span*span,1e-24);
}
inline bool between(Point a,Point b,Point p,double eps){
    return std::abs(cross(a,b,p))<=eps&&p.x>=std::min(a.x,b.x)&&p.x<=std::max(a.x,b.x)&&p.y>=std::min(a.y,b.y)&&p.y<=std::max(a.y,b.y);
}
inline bool intersects(Point a,Point b,Point c,Point d,double eps){
    const auto x=cross(a,b,c),y=cross(a,b,d),z=cross(c,d,a),w=cross(c,d,b);
    if(((x>eps&&y<-eps)||(x<-eps&&y>eps))&&((z>eps&&w<-eps)||(z<-eps&&w>eps)))return true;
    return between(a,b,c,eps)||between(a,b,d,eps)||between(c,d,a,eps)||between(c,d,b,eps);
}
// Validate simple rings, then triangulate: disconnected intersections of a
// concave footprint remain a sum of disjoint triangles, not a bridged ring.
inline std::string triangulate(Ring& p,std::vector<Ring>& triangles,const std::function<bool()>& running={}){
    triangles.clear();
    if(p.size()>1&&p.front().x==p.back().x&&p.front().y==p.back().y)p.pop_back();
    if(p.size()<3)return "Missing polygon (unmapped).";
    for(auto v:p)if(!std::isfinite(v.x)||!std::isfinite(v.y))return "Non-finite polygon coordinate.";
    const double eps=tolerance(p);
    for(std::size_t i=0;i<p.size();++i){
        if(running&&!running())return "Preview cancelled.";
        const auto j=(i+1)%p.size();if(p[i].x==p[j].x&&p[i].y==p[j].y)return "Duplicate polygon vertex.";
        for(std::size_t k=i+1;k<p.size();++k){
            const auto l=(k+1)%p.size();if(j==k||l==i)continue;
            if(intersects(p[i],p[j],p[k],p[l],eps))return "Self-intersecting polygon; holes/multipart rings are unsupported.";
        }
    }
    if(std::abs(signedArea(p))<=eps)return "Degenerate polygon.";
    if(signedArea(p)<0)std::reverse(p.begin(),p.end());
    // Collinear boundary points are valid but do not need to become ears.
    bool changed=true;while(changed&&p.size()>3){changed=false;for(std::size_t i=0;i<p.size();++i){
        if(std::abs(cross(p[(i+p.size()-1)%p.size()],p[i],p[(i+1)%p.size()]))<=eps){p.erase(p.begin()+i);changed=true;break;}
    }}
    std::vector<int> ids(p.size());std::iota(ids.begin(),ids.end(),0);
    while(ids.size()>3){if(running&&!running())return "Preview cancelled.";bool found=false;
        for(std::size_t k=0;k<ids.size();++k){
            const int a=ids[(k+ids.size()-1)%ids.size()],b=ids[k],c=ids[(k+1)%ids.size()];
            if(cross(p[a],p[b],p[c])<=eps)continue;
            bool contains=false;for(int d:ids)if(d!=a&&d!=b&&d!=c&&cross(p[a],p[b],p[d])>=-eps&&cross(p[b],p[c],p[d])>=-eps&&cross(p[c],p[a],p[d])>=-eps){contains=true;break;}
            if(contains)continue;triangles.push_back({p[a],p[b],p[c]});ids.erase(ids.begin()+k);found=true;break;
        }
        if(!found)return "Polygon triangulation failed; repair topology.";
    }
    triangles.push_back({p[ids[0]],p[ids[1]],p[ids[2]]});return {};
}
inline double intersectionArea(Ring subject,const Ring& clip){
    for(std::size_t i=0;i<clip.size()&&!subject.empty();++i){
        const auto a=clip[i],b=clip[(i+1)%clip.size()];Ring next;
        for(std::size_t k=0;k<subject.size();++k){const auto p=subject[k],q=subject[(k+1)%subject.size()];const auto sp=cross(a,b,p),sq=cross(a,b,q);
            if(sp>=0)next.push_back(p);
            if((sp>=0)!=(sq>=0)){const auto t=sp/(sp-sq);next.push_back({p.x+t*(q.x-p.x),p.y+t*(q.y-p.y)});}
        }subject=std::move(next);
    }return std::abs(signedArea(subject));
}
}
#endif
