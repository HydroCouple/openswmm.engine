from pathlib import Path
p=Path(__file__).resolve().parent;s=(p/'harness.cpp').read_text();(p/'harness_regular.cpp').write_text(s)
s=s.replace('MeshData m;m.resize_vertices','const double skew=std::getenv("REVIEW_SKEW")?std::atof(std::getenv("REVIEW_SKEW")):0.0;\n MeshData m;m.resize_vertices')
s=s.replace('m.vx[i]=x*dx;m.vy[i]=y*dx;m.vz[i]=bed(x*dx,y*dx);','''const bool inner=x>0&&x<n&&y>0&&y<ny;
  m.vx[i]=x*dx+(inner?skew*dx*std::sin(.7*x+1.1*y):0);m.vy[i]=y*dx+(inner?skew*dx*std::cos(1.3*x-.4*y):0);m.vz[i]=bed(m.vx[i],m.vy[i]);''')
s=s.replace('m.resize_triangles((quad?1:2)*n*ny);int cell=0;','int nc=0;for(int y=0;y<ny;++y)for(int x=0;x<n;++x)nc+=(quad==1||(quad==2&&(x+y)%2))?1:2;\n m.resize_triangles(nc);int cell=0;')
s=s.replace('if(quad)m.set_quad','if(quad==1||(quad==2&&(x+y)%2))m.set_quad')
(p/'harness.cpp').write_text(s)
