// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <hdf5.h>

namespace openswmm::io {
namespace detail {
// Result images carry simulation time in their datasets. Wall-clock object
// timestamps would make equal accepted states produce different checkpoints.
template<class Create>
inline hid_t createHdf5Object(hid_t kind,hid_t properties,Create create) {
    const hid_t owned=properties==H5P_DEFAULT?H5Pcreate(kind):H5Pcopy(properties);
    if(owned<0)return -1;
    const hid_t result=H5Pset_obj_track_times(owned,0)>=0?create(owned):-1;
    H5Pclose(owned);return result;
}
}
inline hid_t createHdf5Dataset(hid_t location,const char* name,hid_t type,hid_t space,
                              hid_t links,hid_t creation,hid_t access) {
    return detail::createHdf5Object(H5P_DATASET_CREATE,creation,[&](hid_t properties) {
        return H5Dcreate2(location,name,type,space,links,properties,access);
    });
}
inline hid_t createHdf5Group(hid_t location,const char* name,hid_t links,hid_t creation,hid_t access) {
    return detail::createHdf5Object(H5P_GROUP_CREATE,creation,[&](hid_t properties) {
        return H5Gcreate2(location,name,links,properties,access);
    });
}
inline hid_t createHdf5File(const char* path,unsigned flags,hid_t creation,hid_t access) {
    return detail::createHdf5Object(H5P_FILE_CREATE,creation,[&](hid_t properties) {
        return H5Fcreate(path,flags,properties,access);
    });
}
}
