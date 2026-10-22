// SPDX-License-Identifier: Apache-2.0

/**
 * @file      Hdf5ObjectCreation.hpp
 * @brief     Checked creation of HDF5 objects and their properties.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <hdf5.h>

namespace openswmm::io {
namespace detail {
// Result images carry simulation time in their datasets. Wall-clock object
// timestamps would make equal accepted states produce different checkpoints.
/**
 * @brief Create an HDF5 object with wall-clock object timestamps disabled.
 * @tparam Create Callable receiving a temporary creation-property list and
 * returning a new hid_t or a negative failure value.
 * @param[in] kind HDF5 creation-property class used when properties is H5P_DEFAULT.
 * @param[in] properties Borrowed creation properties copied before modification,
 * or H5P_DEFAULT to construct a fresh list of kind.
 * @param[in] create Synchronous callable; must not retain the temporary list.
 * @return New owned object identifier, or -1 if list allocation/copying or
 * timestamp configuration fails. A negative callback result is propagated.
 * @note The temporary property list is closed after the call; the original
 * list is unchanged. The callback must not throw, since exception cleanup of
 * the temporary raw identifier is not provided.
 */
template <class Create> inline hid_t createHdf5Object(hid_t kind, hid_t properties, Create create) {
    const hid_t owned = properties == H5P_DEFAULT ? H5Pcreate(kind) : H5Pcopy(properties);
    if (owned < 0)
        return -1;
    const hid_t result = H5Pset_obj_track_times(owned, 0) >= 0 ? create(owned) : -1;
    H5Pclose(owned);
    return result;
}
}
/**
 * @brief Create a dataset without wall-clock object timestamps.
 * @param[in] location Borrowed file or group identifier for the new link.
 * @param[in] name Non-null null-terminated dataset path, borrowed for this call.
 * @param[in] type Borrowed HDF5 datatype identifier.
 * @param[in] space Borrowed dataspace defining the dataset shape.
 * @param[in] links Borrowed link-creation properties or H5P_DEFAULT.
 * @param[in] creation Borrowed dataset-creation properties or H5P_DEFAULT;
 * copied before disabling timestamp tracking.
 * @param[in] access Borrowed dataset-access properties or H5P_DEFAULT.
 * @return Owned dataset identifier to close with H5Dclose(), or a negative
 * value on failure. No input identifier or pointer is retained.
 */
inline hid_t createHdf5Dataset(hid_t location, const char* name, hid_t type, hid_t space,
                               hid_t links, hid_t creation, hid_t access) {
    return detail::createHdf5Object(H5P_DATASET_CREATE, creation, [&](hid_t properties) {
        return H5Dcreate2(location, name, type, space, links, properties, access);
    });
}
/**
 * @brief Create a group without wall-clock object timestamps.
 * @param[in] location Borrowed file or group identifier for the new link.
 * @param[in] name Non-null null-terminated group path, borrowed for this call.
 * @param[in] links Borrowed link-creation properties or H5P_DEFAULT.
 * @param[in] creation Borrowed group-creation properties or H5P_DEFAULT;
 * copied before disabling timestamp tracking.
 * @param[in] access Borrowed group-access properties or H5P_DEFAULT.
 * @return Owned group identifier to close with H5Gclose(), or a negative
 * value on failure. No input identifier or pointer is retained.
 */
inline hid_t createHdf5Group(hid_t location, const char* name, hid_t links, hid_t creation,
                             hid_t access) {
    return detail::createHdf5Object(H5P_GROUP_CREATE, creation, [&](hid_t properties) {
        return H5Gcreate2(location, name, links, properties, access);
    });
}
/**
 * @brief Create an HDF5 file without wall-clock object timestamps.
 * @param[in] path Non-null null-terminated destination filename.
 * @param[in] flags H5Fcreate access flags, such as H5F_ACC_TRUNC or H5F_ACC_EXCL.
 * @param[in] creation Borrowed file-creation properties or H5P_DEFAULT;
 * copied before disabling timestamp tracking.
 * @param[in] access Borrowed file-access properties or H5P_DEFAULT.
 * @return Owned file identifier to close with H5Fclose(), or a negative value
 * on failure. Existing-file handling follows flags; no input is retained.
 */
inline hid_t createHdf5File(const char* path, unsigned flags, hid_t creation, hid_t access) {
    return detail::createHdf5Object(H5P_FILE_CREATE, creation, [&](hid_t properties) {
        return H5Fcreate(path, flags, properties, access);
    });
}
}
