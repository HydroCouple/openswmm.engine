// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      Element.hpp
 * @brief     Shared surface element types for snow and infiltration models.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
namespace openswmm::surface {
/// Non-owning geometry/site view. SI lengths and area, slope as rise/run,
/// aspect/location in degrees. Null optional fields mean no authored override.
struct SurfaceElement {
    const double *area = nullptr, *slope = nullptr, *aspect = nullptr;
    const double *latitude = nullptr, *longitude = nullptr, *elevation = nullptr;
    const double *forest_cover = nullptr, *drift = nullptr, *ground_heat_flux = nullptr;
    const double* albedo_extinction_depth = nullptr;
};
}
