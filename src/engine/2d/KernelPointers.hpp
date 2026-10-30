// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_MSC_VER)
#define OPENSWMM_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define OPENSWMM_RESTRICT __restrict__
#else
#define OPENSWMM_RESTRICT
#endif
