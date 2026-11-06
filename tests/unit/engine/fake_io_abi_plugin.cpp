// SPDX-License-Identifier: Apache-2.0
#include <openswmm/plugin_sdk/PluginAbi.hpp>
#include <openswmm/plugin_sdk/IPluginComponentInfo.hpp>
#include <cstdlib>

#if TEST_PLUGIN_ABI == 1
OPENSWMM_PLUGIN_EXPORT const char* openswmm_plugin_abi_tag() { return "obsolete-sdk"; }
#elif TEST_PLUGIN_ABI == 2
OPENSWMM_DECLARE_PLUGIN_ABI()
#endif

OPENSWMM_PLUGIN_EXPORT openswmm::IPluginComponentInfo* openswmm_plugin_info() {
#if TEST_PLUGIN_ABI != 2
    std::abort(); // An incompatible factory must never be entered.
#else
    class Info final : public openswmm::IPluginComponentInfo {
    public:
        std::string id() const override { return "test.current-plugin-abi"; }
        std::string caption() const override { return "ABI test"; }
        std::string description() const override { return "ABI test"; }
        std::string version() const override { return "1"; }
        std::string vendor() const override { return "test"; }
        std::string url() const override { return ""; }
        std::string license_type() const override { return "Apache-2.0"; }
        std::string license_text() const override { return ""; }
    };
    static Info info;
    return &info;
#endif
}
