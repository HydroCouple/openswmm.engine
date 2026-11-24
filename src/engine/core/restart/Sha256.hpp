// SPDX-License-Identifier: Apache-2.0
/**
 * @file      Sha256.hpp
 * @brief     SHA-256 digest (FIPS 180-4) of a byte string, for restart record identity.
 */
#pragma once

#include <string>
#include <string_view>

namespace openswmm::restart {

/**
 * @brief Compute the SHA-256 digest of @p bytes.
 * @param[in] bytes Borrowed input of any length.
 * @return 64 lowercase hexadecimal characters.
 */
std::string sha256Hex(std::string_view bytes);

} // namespace openswmm::restart
