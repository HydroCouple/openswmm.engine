// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <openswmm_abi_tag.h>

#if defined(_WIN32)
#define OPENSWMM_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define OPENSWMM_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Export once per plugin, alongside openswmm_plugin_info. The installed tag
// identifies engine API, plugin SDK, compiler ABI and enabled build features.
#define OPENSWMM_DECLARE_PLUGIN_ABI() \
    OPENSWMM_PLUGIN_EXPORT const char* openswmm_plugin_abi_tag() { \
        return OPENSWMM_ENGINE_ABI_TAG; \
    }
