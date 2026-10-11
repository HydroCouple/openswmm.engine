// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once
#include <mutex>
namespace openswmm::io {
// Mesh output and its diagnostic artifacts share the engine's HDF5 instance.
// Recursive because a mesh publication may call the source-history writer.
inline std::recursive_mutex& resultHdf5Mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
}
