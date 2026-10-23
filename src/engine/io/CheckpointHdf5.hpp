// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
/**
 * @file CheckpointHdf5.hpp
 * @brief Checkpoint result-file access with rename-sharing descriptor support.
 * @ingroup engine_io
 * @author Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license Apache-2.0
 */
#pragma once
#include <hdf5.h>

namespace openswmm::io {
// Calls serialize with the mesh writer through resultHdf5Mutex().
/**
 * @brief Create access properties using the locked native-descriptor checkpoint VFD.
 *
 * Serializes registration/property creation through resultHdf5Mutex. The VFD uses native
 * descriptors with shared/exclusive nonblocking file locks; Windows opens with FILE_SHARE_DELETE so
 * checkpoint publication can rename an open result file. Callers serialize subsequent HDF5
 * operations using the same mutex; file objects retain their own properties after this list closes.
 * @return Owned H5P_FILE_ACCESS identifier to close with H5Pclose, or H5I_INVALID_HID when
 *     registration/property setup fails.
 */
hid_t checkpointHdf5Access();
/**
 * @brief Create a checkpoint-compatible HDF5 result file.
 *
 * Creation disables wall-clock object timestamps for deterministic checkpoint images. Serializes
 * this operation with resultHdf5Mutex, uses the checkpoint VFD on Windows and default HDF5 access
 * on POSIX. Temporary access properties are released before return. Caller must serialize later
 * HDF5 operations and own the returned file lifetime; opening/creating does not retain caller path
 * storage.
 *
 * @param[in] path Non-null null-terminated filename, borrowed for the call; UTF-8 on Windows.
 * @param[in] flags H5Fcreate flags, such as H5F_ACC_TRUNC or H5F_ACC_EXCL; determine
 *     replacement/exclusive-create behavior.
 * @return Owned HDF5 file identifier to close with H5Fclose, or a negative identifier on failure.
 */
hid_t createCheckpointHdf5(const char* path, unsigned flags);
/**
 * @brief Open a checkpoint-compatible HDF5 result file.
 *
 * Serializes this operation with resultHdf5Mutex, uses the checkpoint VFD on Windows and default
 * HDF5 access on POSIX. Temporary access properties are released before return. Caller must
 * serialize later HDF5 operations and own the returned file lifetime; opening/creating does not
 * retain caller path storage.
 *
 * @param[in] path Non-null null-terminated filename, borrowed for the call; UTF-8 on Windows.
 * @param[in] flags H5Fopen access flags, normally H5F_ACC_RDONLY or H5F_ACC_RDWR.
 * @return Owned HDF5 file identifier to close with H5Fclose, or a negative identifier on failure.
 */
hid_t openCheckpointHdf5(const char* path, unsigned flags);
/**
 * @brief Borrow the native descriptor of a sec2 or checkpoint-VFD file.
 *
 * Serializes metadata lookup with resultHdf5Mutex. Does not duplicate the descriptor: do not close
 * it independently, and do not use it after the HDF5 file closes. The lock is released before
 * return; caller must coordinate later descriptor IO with HDF5 file operations. May register the
 * checkpoint driver while comparing VFD kinds.
 *
 * @param[in] file Borrowed live HDF5 file identifier; caller keeps it open throughout descriptor
 *     use.
 * @return Borrowed descriptor value, or -1 when file/access lookup fails or the VFD is unsupported.
 */
int checkpointHdf5Descriptor(hid_t file);
}
