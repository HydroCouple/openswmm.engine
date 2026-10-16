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
 * @file smoke_component.cpp
 * @brief D2 (program plan §C.1) — the test double for component discovery.
 *
 * A REAL shared library, loaded through dlopen exactly as a third-party
 * component would be. Not a mock: it implements every pure virtual of an
 * ABI-4 `HydroCouple::IModelComponentInfo` (22 of them) against the same
 * headers the engine compiles against.
 *
 * One source, three builds, selected by compile definitions:
 *
 *   SMOKE_MODE 0  stamped — `HYDROCOUPLE_DECLARE_COMPONENT`, the convention
 *   SMOKE_MODE 1  MIS-stamped — identical code, but its stamp claims iface=2.
 *                 That is the exact lie the pre-D0 header told for every
 *                 ABI-3/4 component; the engine must refuse it and say so.
 *   SMOKE_MODE 2  legacy — only `CreateComponentInfo`, the unstamped Python
 *                 loader convention; the engine must load it and report it
 *                 as unstamped, never as verified.
 *
 * `SMOKE_COMPONENT_ID` differs per build so that one discovery pass can see
 * every variant without the duplicate-id rule masking any of them.
 *
 * `createComponentInstance()` returns an empty pointer in this round, and it
 * is the one thing here that is not a real implementation. D2 gates
 * DISCOVERY — find, stamp-check, index — and never instantiates. A real
 * instance needs the plan's SDK-free `ComponentBase` (src/couplers, phase
 * A1/F1), which does not exist yet; hand-writing an ABI-4 IModelComponent
 * lifecycle here would be a second copy of that base, the pattern this
 * sequence keeps having to undo.
 */

#include <hydrocouple.h>
#include <hydrocouplecomponentabi.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef SMOKE_COMPONENT_ID
#error "SMOKE_COMPONENT_ID must be defined by the build"
#endif
#ifndef SMOKE_MODE
#error "SMOKE_MODE must be defined by the build (0 stamped, 1 mis-stamped, 2 legacy)"
#endif

namespace {

class SmokeInfo final : public HydroCouple::IModelComponentInfo {
public:
    // ── ISignal<std::string> (IPropertyChanged) ───────────────────────────
    void connect(const std::shared_ptr<HydroCouple::ISlot<std::string>>&) override {}
    void disconnect(const std::shared_ptr<HydroCouple::ISlot<std::string>>&) override {}
    void blockSignals(bool) override {}
    void emit(std::string) override {}

    // ── IDescription / IIdentity ──────────────────────────────────────────
    const std::string& caption() const override { return caption_; }
    void setCaption(const std::string& c) override { caption_ = c; }
    const std::string& description() const override { return description_; }
    void setDescription(const std::string& d) override { description_ = d; }
    const std::string& id() const override { return id_; }

    // ── IComponentInfo ────────────────────────────────────────────────────
    std::string libraryFilePath() const override { return library_path_; }
    void setLibraryFilePath(const std::string& p) override { library_path_ = p; }
    std::string iconFilePath() const override { return {}; }
    std::string developer() const override { return "openswmm test suite"; }
    std::vector<std::string> documentation() const override { return {}; }
    std::string license() const override { return "Apache-2.0"; }
    std::string copyright() const override { return "Copyright 2026 Caleb Buahin"; }
    std::string url() const override { return {}; }
    std::string email() const override { return {}; }
    std::string version() const override { return "0.0.1"; }
    std::set<std::string> tags() const override { return {"test", "smoke"}; }

    // ── IModelComponentInfo ───────────────────────────────────────────────
    std::unique_ptr<HydroCouple::IModelComponent> createComponentInstance() override {
        return {};   // see the file comment: D2 never instantiates
    }
    std::vector<HydroCouple::IAdaptedOutputFactory*> adaptedOutputFactories() const override {
        return {};
    }

private:
    std::string id_          = SMOKE_COMPONENT_ID;
    std::string caption_     = "Smoke (" SMOKE_COMPONENT_ID ")";
    std::string description_ = "D2 discovery test double";
    std::string library_path_;
};

} // namespace

#if SMOKE_MODE == 0

HYDROCOUPLE_DECLARE_COMPONENT(SmokeInfo)

#elif SMOKE_MODE == 1

// Everything the real stamp says, except the interface ABI — the precise
// failure mode of the Composer-era header. Assembled from the same pieces so
// the ONLY difference from the engine's stamp is `iface`.
#define SMOKE_FORGED_STAMP                                                     \
    "hc-abi/" HYDROCOUPLE_COMPONENT_ABI_REVISION                               \
    ";iface=2"                                                                 \
    ";cxx=" HYDROCOUPLE_ABI_COMPILER                                           \
    ";stdlib=" HYDROCOUPLE_ABI_STDLIB                                          \
    ";bits=" HYDROCOUPLE_ABI_STRINGIFY(__SIZEOF_POINTER__)

extern "C" HYDROCOUPLE_COMPONENT_EXPORT const char* hydrocouple_component_abi_v1(void) {
    return SMOKE_FORGED_STAMP;
}
// Present, so that a loader which skipped the stamp comparison WOULD load
// this library — which is what makes the refusal falsifiable.
extern "C" HYDROCOUPLE_COMPONENT_EXPORT HydroCouple::IComponentInfo*
hydrocouple_component_info_v1(void) {
    static SmokeInfo s_info;
    return &s_info;
}

#elif SMOKE_MODE == 2

extern "C" HYDROCOUPLE_COMPONENT_EXPORT HydroCouple::IModelComponentInfo*
CreateComponentInfo(void) {
    static SmokeInfo s_info;
    return &s_info;
}

#else
#error "unknown SMOKE_MODE"
#endif
