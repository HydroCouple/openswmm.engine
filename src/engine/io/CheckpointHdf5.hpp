// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <hdf5.h>

namespace openswmm::io {
// Calls serialize with the mesh writer through resultHdf5Mutex().
/// Owned access properties for a rename-sharing, locked native-descriptor VFD.
/// The caller closes them with H5Pclose(); files retain their own properties.
hid_t checkpointHdf5Access();
/// Default mesh access on POSIX; rename-sharing access on Windows.
hid_t createCheckpointHdf5(const char* path, unsigned flags);
hid_t openCheckpointHdf5(const char* path, unsigned flags);
/// Borrow the descriptor of a sec2 or checkpoint VFD, or return -1.
int checkpointHdf5Descriptor(hid_t file);
}
