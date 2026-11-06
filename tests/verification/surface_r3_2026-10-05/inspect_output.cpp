#include <hdf5.h>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
int main(int argc,char**argv){
 const std::vector<std::string> names={"Mesh2_face_et_pending","Mesh2_face_et_potential_cum","Mesh2_face_et_surface_cum","Mesh2_face_et_soil_cum","Mesh2_face_et_unused_cum","Mesh2_face_et_surface","Mesh2_face_et_potential","Mesh2_face_et_stress","Mesh2_face_et_refresh","Mesh2_face_gw_et","groundwater_ledger"};
 std::cout<<"[";
 for(int k=1;k<argc;++k){auto f=H5Fopen(argv[k],H5F_ACC_RDONLY,H5P_DEFAULT);if(f<0)return 2;
  if(k>1)std::cout<<",";std::string path=argv[k];std::cout<<"{\"file\":\""<<path.substr(path.find_last_of('/')+1)<<"\",\"fields\":{";
  for(size_t j=0;j<names.size();++j){auto d=H5Dopen2(f,names[j].c_str(),H5P_DEFAULT);if(d<0)return 3;auto s=H5Dget_space(d);int rank=H5Sget_simple_extent_ndims(s);std::vector<hsize_t> dims(rank);H5Sget_simple_extent_dims(s,dims.data(),nullptr);size_t n=1;for(auto v:dims)n*=v;std::vector<double> vals(n);if(H5Dread(d,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,vals.data())<0)return 4;
   size_t finite=0,zero=0;for(auto v:vals){finite+=std::isfinite(v);zero+=v==0;}
   auto a=H5Aopen(d,"units",H5P_DEFAULT);auto t=H5Aget_type(a);std::string units;if(H5Tis_variable_str(t)){char* text=nullptr;H5Aread(a,t,&text);units=text;H5free_memory(text);}else{std::vector<char> text(H5Tget_size(t)+1);H5Aread(a,t,text.data());units=text.data();}
   if(j)std::cout<<",";std::cout<<"\""<<names[j]<<"\":{\"shape\":[";for(int q=0;q<rank;++q){if(q)std::cout<<",";std::cout<<dims[q];}std::cout<<"],\"units\":\""<<units<<"\",\"finite\":"<<finite<<",\"zero\":"<<zero<<"}";
   H5Tclose(t);H5Aclose(a);H5Sclose(s);H5Dclose(d);
  }std::cout<<"}}";H5Fclose(f);
 }std::cout<<"]\n";
}
