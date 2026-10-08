// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file Hdf5PluginInfo.cpp
 * @brief Factory methods for the HDF5 plugin info singleton.
 * @ingroup engine_hdf5
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Hdf5PluginInfo.hpp"
#include "Hdf5InputPlugin.hpp"

namespace openswmm::h5io {

IInputPlugin* Hdf5PluginInfo::create_input_plugin() const {
    // The caller owns the instance; swmm_model_write_with_plugin creates one
    // transiently per call and deletes it, so the engine's primary input
    // plugin is never disturbed.
    return new Hdf5InputPlugin();
}

}  // namespace openswmm::h5io
